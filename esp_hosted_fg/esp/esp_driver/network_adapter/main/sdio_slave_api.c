// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2024 Espressif Systems (Shanghai) PTE LTD
//
// ESP-Hosted FG SDIO slave transport.
//
// To-host path: producers enqueue frames on the to-host queue(s); a SINGLE
// send_task drains them, packs a batch of frames into one buffer, and
// blocking-transmits. No mutex, no timer, no per-producer aggregation - the
// single consumer makes packing lock-free.
//
// E2H send strategy is a build-time switch (idf.py -DTX_MODE=...):
//   SW_AGGR (default) - pack the queued batch into one buffer, blocking transmit
//   STREAM            - one buffer per frame via send_queue (SLC concatenates)
//   PACKET            - one frame per blocking transmit
//
// Wire format is identical for all modes: a contiguous stream of 4-byte-aligned
// [esp_payload_header | payload] frames, so the host is agnostic to the mode.

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_hosted_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "interface.h"
#include "adapter.h"
#include "sdio_slave_api.h"
#include "driver/sdio_slave.h"
#include "soc/sdio_slave_periph.h"
#include "hal/sdio_slave_ll.h"
#include "mempool.h"
#include "endian.h"
#include "stats.h"
#include "esp_fw_version.h"

/* ===================== TX strategy (menuconfig) =====================
 * Selected via Kconfig: "SDIO Configuration" -> "SDIO E2H TX strategy".
 * CONFIG_ESP_SDIO_TX_MODE_VAL carries the chosen value (0..2). */
#define TX_MODE_SW_AGGR   0
#define TX_MODE_STREAM    1
#define TX_MODE_PACKET    2
#ifdef CONFIG_ESP_SDIO_TX_MODE_VAL
#define TX_MODE   CONFIG_ESP_SDIO_TX_MODE_VAL
#else
#define TX_MODE   TX_MODE_SW_AGGR
#endif

/* Per-aggregate instrumentation + payload sequence number (payload[0:4]) for
 * drop tracing at the host. Off by default (the seq number overwrites the first
 * 4 payload bytes, which is only safe for the raw-throughput test).
 * Kconfig: "SDIO Configuration" -> "SDIO TX per-aggregate debug instrumentation". */
#ifdef CONFIG_ESP_SDIO_TX_DEBUG
#define SDIO_TX_DEBUG     1
#else
#define SDIO_TX_DEBUG     0
#endif

/* ===================== tunables ===================== */
#define SDIO_RX_BUFFER_NUM        2     /* host->slave RX buffers */
/* Per-lane to-host queue depth. Each queued handle pins an upstream buffer
 * (WiFi eb for OTHERS, malloc'd for SERIAL) until it is packed into the
 * aggregate, so total depth = peak in-flight buffers committed. SERIAL/BT are
 * low-rate control/BT -> shallow is plenty and avoids over-committing buffers.
 * OTHERS is the bulk datapath -> keep >= 2 aggregates of run-ahead
 * (15872/1536 ~= 10 frames/aggregate) so E2H pipelining isn't starved. */
#define SDIO_Q_DEPTH_SERIAL   5   /* PRIO_Q_SERIAL: control/serial lane */
#define SDIO_Q_DEPTH_BT       5   /* PRIO_Q_BT: BT/HCI lane */
#define SDIO_Q_DEPTH_OTHERS   32  /* PRIO_Q_OTHERS: bulk datapath. KEEP 32.
   *
   * MEASURED (i.MX HT40, warmed-up sweep, TCP up/down, UDP):
   *   32: TCP 74/53  UDP 94/76   <- best
   *   20: TCP 56/53  UDP 94/76   (TCP-up -18: E2H ack-return rides this queue)
   *   10: TCP 57/broken UDP-down FAIL (downlink dies)
   *
   * WHO SETS THE LIMIT: this 32 is a fixed queue length, not auto-tuned. Each
   * slot holds a WiFi RX buffer (eb) until it is sent. The WiFi driver hands us
   * eb's up to CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM and, when that pool empties
   * (we hold too many, bus too slow), stops RX itself. So the WiFi driver's pool
   * + flow control is the real auto-regulator; 32 just sits at the common pool
   * size so it never binds first. 32 is also where E2H run-ahead stops starving.
   *
   * MEMORY: the queue copies a ~24 B handle, not the frame -> ~0.75 KB total for
   * 32 slots. Frame data stays in WiFi eb's; we only hold them longer (~50 KB of
   * the WiFi pool in flight at peak). This knob allocates almost nothing itself.
   *
   * TUNING: don't. Smaller starves run-ahead (above). Larger gains nothing (SDIO
   * bus is the ceiling) and on C6/C61 (pool 64) lets this queue drain the whole
   * WiFi pool, starving AMPDU reorder. For more run-ahead raise the WiFi pool
   * (DYNAMIC_RX_BUFFER_NUM), not this. Memory win is SERIAL/BT=5, not OTHERS. */

static const uint16_t to_host_q_depth[MAX_PRIORITY_QUEUES] = {
	[PRIO_Q_SERIAL] = SDIO_Q_DEPTH_SERIAL,
	[PRIO_Q_BT]     = SDIO_Q_DEPTH_BT,
	[PRIO_Q_OTHERS] = SDIO_Q_DEPTH_OTHERS,
};
#define SDIO_HDR_SIZE             (sizeof(struct esp_payload_header))

#if TX_MODE == TX_MODE_STREAM
#define SDIO_STREAM_QUEUE_SIZE    14    /* HW send-queue depth for STREAM (per-packet) */
#define SDIO_DRIVER_TX_QUEUE_SIZE SDIO_STREAM_QUEUE_SIZE
#define SDIO_TX_BUFFER_SIZE       1536  /* one MTU frame + header (fits 1460 raw-TP too) */
#define SDIO_TX_MEMPOOL_MARGIN    4     /* pool slack over in-flight depth */
#define SDIO_TX_MEMPOOL_BLOCKS    (SDIO_STREAM_QUEUE_SIZE + SDIO_TX_MEMPOOL_MARGIN)
#else
#define SDIO_DRIVER_TX_QUEUE_SIZE 8     /* IDF sdio_slave driver send-queue depth */
#endif

#if CONFIG_ESP_SDIO_PSEND_PSAMPLE
  #define SDIO_SLAVE_TIMING SDIO_SLAVE_TIMING_PSEND_PSAMPLE
#elif CONFIG_ESP_SDIO_NSEND_PSAMPLE
  #define SDIO_SLAVE_TIMING SDIO_SLAVE_TIMING_NSEND_PSAMPLE
#elif CONFIG_ESP_SDIO_PSEND_NSAMPLE
  #define SDIO_SLAVE_TIMING SDIO_SLAVE_TIMING_PSEND_NSAMPLE
#elif CONFIG_ESP_SDIO_NSEND_NSAMPLE
  #define SDIO_SLAVE_TIMING SDIO_SLAVE_TIMING_NSEND_NSAMPLE
#else
  #error No SDIO Slave Timing configured
#endif

/* ===================== state ===================== */
static const char *TAG = "SDIO_SLAVE";
interface_context_t context;
interface_handle_t if_handle_g;
extern volatile uint8_t datapath;        /* set when the host opens the data path */

static uint32_t sdio_rx_buf_size = MAX_TRANSPORT_BUF_SIZE;
static uint8_t *sdio_slave_rx_buffer[SDIO_RX_BUFFER_NUM];

static QueueHandle_t to_host_queue[MAX_PRIORITY_QUEUES]; /* per-priority to-host queues
	 * (PRIO_Q_SERIAL/BT/OTHERS) - serial/control gets its own lane, drained ahead
	 * of bulk data so RPC responses aren't batched behind a data aggregate. */

#if TX_MODE == TX_MODE_SW_AGGR
static uint8_t *tx_aggr_buf;             /* one persistent DMA aggregate buffer */
#elif TX_MODE == TX_MODE_PACKET
static uint8_t *tx_pkt_buf;              /* one persistent DMA frame buffer */
#elif TX_MODE == TX_MODE_STREAM
static SemaphoreHandle_t tx_stream_sem;  /* bounds in-flight send_queue buffers */
static struct hosted_mempool *buf_mp_tx_g; /* pre-allocated DMA TX blocks (no per-pkt malloc) */
static uint32_t stream_tx_acc;           /* bytes since last yield (idle-starvation guard) */
#define STREAM_YIELD_BYTES (512u * 1024u) /* yield to idle every ~512KB -> feed 5s task WDT */
#endif

#if SDIO_TX_DEBUG
static uint32_t tx_seq;                  /* payload sequence number, for drop tracing */
static uint64_t dbg_frames, dbg_aggs, dbg_bytes;
static int64_t  dbg_t0;
#endif

static interface_handle_t *sdio_init(void);
static int32_t sdio_write(interface_handle_t *handle, interface_buffer_handle_t *buf_handle);
static int sdio_read(interface_handle_t *if_handle, interface_buffer_handle_t *buf_handle);
static esp_err_t sdio_reset(interface_handle_t *handle);
static void sdio_deinit(interface_handle_t *handle);

if_ops_t if_ops = {
	.init   = sdio_init,
	.write  = sdio_write,      /* enqueue; the single send_task does the rest */
	.read   = sdio_read,
	.reset  = sdio_reset,
	.deinit = sdio_deinit,
};

static uint32_t sdio_hw_max_rx_buf_size(void)
{
	sdio_slave_ll_desc_t dummy;
	memset(&dummy, 0xFF, sizeof(dummy));
	return (dummy.size / 512) * 512;
}

static void sdio_read_done(void *handle)
{
	sdio_slave_recv_load_buf((sdio_slave_buf_handle_t) handle);
}

static inline void free_tx_buf(interface_buffer_handle_t *b)
{
	if (b->free_buf_handle && b->priv_buffer_handle) {
		b->free_buf_handle(b->priv_buffer_handle);
		b->priv_buffer_handle = NULL;
	}
}

/* Build one [header|payload] frame at dst; returns the 4-byte-aligned frame len. */
static inline uint16_t build_frame(uint8_t *dst, interface_buffer_handle_t *b)
{
	struct esp_payload_header *h = (struct esp_payload_header *)dst;
	uint16_t frame_len = SDIO_HDR_SIZE + b->payload_len;
	uint16_t aligned = (frame_len + 3) & ~3;

	memset(h, 0, SDIO_HDR_SIZE);
	h->if_type = b->if_type;
	h->if_num  = b->if_num;
	h->len     = htole16(b->payload_len);
	h->offset  = htole16(SDIO_HDR_SIZE);
	h->seq_num = htole16(b->seq_num);
	h->flags   = b->flag;
	UPDATE_HEADER_TX_PKT_NO(h);

	memcpy(dst + SDIO_HDR_SIZE, b->payload, b->payload_len);
#if SDIO_TX_DEBUG
	if (b->payload_len >= 4)                          /* drop-trace seq in payload[0:4] */
		*(uint32_t *)(dst + SDIO_HDR_SIZE) = tx_seq++;
#endif
	if (aligned > frame_len)
		memset(dst + frame_len, 0, aligned - frame_len);
#if CONFIG_ESP_SDIO_CHECKSUM
	h->checksum = htole16(compute_checksum(dst, frame_len));
#endif
	return aligned;
}

/* ===================== single send_task ===================== */
#if TX_MODE == TX_MODE_SW_AGGR
/* SW-aggregation TX path: a single send_task drains the to-host queue(s),
 * packs frames via build_frame, and blocking-transmits with sdio_slave_transmit. */
#ifndef SDIO_TX_LATENCY_BYPASS_SIZE
#define SDIO_TX_LATENCY_BYPASS_SIZE 256
#endif

/* Drain ONE priority queue: pack its frames into aggr_buf and blocking-transmit.
 * Each priority drains separately so control frames aren't stuck behind data.
 *
 * SINGLE-CONSUMER INVARIANT: only send_task receives from these queues, so the
 * peek-then-receive below is safe (nobody else removes the front in between).
 * A second consumer would break it - rework before adding one. */
static void process_tx_queue(QueueHandle_t q, uint16_t queued, uint8_t *aggr_buf)
{
	interface_buffer_handle_t buf = {0};
	uint16_t aggr_len = 0;
	bool flush_after_pkt = false;

	if (!queued)
		return;

	if (!aggr_buf) {
		if (xQueueReceive(q, &buf, portMAX_DELAY))
			free_tx_buf(&buf);
		return;
	}

	while (queued || uxQueueMessagesWaiting(q)) {
		uint16_t frame_len = 0;
		uint16_t aligned_len = 0;
		uint16_t offset = SDIO_HDR_SIZE;
		TickType_t wait = queued ? portMAX_DELAY : 0;

		/* Peek, don't dequeue: inspect the front and leave it queued if it
		 * belongs in the next aggregate. Replaces the old dequeue+requeue,
		 * where a full queue made the requeue fail and leaked the frame. */
		if (!xQueuePeek(q, &buf, wait))
			break;

		if (!buf.payload || !buf.payload_len ||
		    buf.payload_len + offset > sdio_rx_buf_size) {
			/* drop invalid frame (receive can't fail; guard catches a
			 * future invariant break loudly) */
			if (!xQueueReceive(q, &buf, 0)) {
				ESP_LOGE(TAG, "peek/receive desync");
				break;
			}
			if (queued)
				queued--;
			free_tx_buf(&buf);
			continue;
		}

		frame_len = buf.payload_len + offset;
		aligned_len = (frame_len + 3) & ~3;
		flush_after_pkt = buf.payload_len <= SDIO_TX_LATENCY_BYPASS_SIZE;

		/* this frame starts the next aggregate (small/latency or won't fit):
		 * leave it queued and flush what we have */
		if (aggr_len && (flush_after_pkt ||
				 aggr_len + aligned_len > sdio_rx_buf_size))
			break;

		/* commit: dequeue the peeked frame and pack it (same guard) */
		if (!xQueueReceive(q, &buf, 0)) {
			ESP_LOGE(TAG, "peek/receive desync");
			break;
		}
		if (queued)
			queued--;

		aggr_len += build_frame(aggr_buf + aggr_len, &buf);
		free_tx_buf(&buf);
		if (flush_after_pkt)
			break;
	}

	if (aggr_len && sdio_slave_transmit(aggr_buf, aggr_len) != ESP_OK)
		ESP_LOGE(TAG, "aggregate transmit failed");
}

static void send_task(void *arg)
{
	for (;;) {
		bool worked = false;

		/* highest priority first: PRIO_Q_SERIAL(0) -> BT(1) -> OTHERS(2) */
		for (int p = 0; p < MAX_PRIORITY_QUEUES; p++) {
			uint16_t waiting = uxQueueMessagesWaiting(to_host_queue[p]);

			if (waiting) {
				process_tx_queue(to_host_queue[p], waiting, tx_aggr_buf);
				worked = true;
			}
		}

		if (!worked)
			vTaskDelay(1);
	}
}

#elif TX_MODE == TX_MODE_PACKET
#error "Not the current run"
static void send_task(void *arg)
{
	interface_buffer_handle_t buf;

	for (;;) {
		uint16_t total;

		if (xQueueReceive(to_host_queue, &buf, portMAX_DELAY) != pdTRUE)
			continue;
		if (!datapath || !buf.payload || !buf.payload_len) { free_tx_buf(&buf); continue; }
		total = SDIO_HDR_SIZE + buf.payload_len;
		build_frame(tx_pkt_buf, &buf);
		if (sdio_slave_transmit(tx_pkt_buf, total) != ESP_OK)
			ESP_LOGE(TAG, "packet transmit failed");
		free_tx_buf(&buf);
	}
}

#elif TX_MODE == TX_MODE_STREAM
/* STREAM: no software aggregation. Each frame is copied into its own DMA buffer
 * and handed to the IDF HW send-queue (sdio_slave_send_queue); the counting
 * semaphore tx_stream_sem bounds in-flight buffers to SDIO_STREAM_QUEUE_SIZE.
 * Completed buffers are reclaimed (freed + sem returned) as they finish. Drained
 * per-priority (SERIAL -> BT -> OTHERS), matching the SW_AGGR path. */
static void reclaim_finished(void)
{
	uint8_t *done;

	while (sdio_slave_send_get_finished((void **)&done, 0) == ESP_OK && done) {
		hosted_mempool_free(buf_mp_tx_g, done);
		xSemaphoreGive(tx_stream_sem);
	}
}

static void process_tx_stream(QueueHandle_t q, uint16_t queued)
{
	interface_buffer_handle_t buf = {0};

	while (queued--) {
		uint16_t frame_len, aligned;
		uint8_t *sendbuf;

		if (!xQueueReceive(q, &buf, 0))
			break;
		if (!datapath || !buf.payload || !buf.payload_len ||
		    buf.payload_len + SDIO_HDR_SIZE > sdio_rx_buf_size) {
			free_tx_buf(&buf);
			continue;
		}

		frame_len = SDIO_HDR_SIZE + buf.payload_len;
		aligned   = (frame_len + 3) & ~3;

		/* Acquire an in-flight slot. If all SDIO_STREAM_QUEUE_SIZE are busy, block
		 * by reclaiming a finished buffer here - never a bare blocking take: the
		 * only thing that returns a token is reclaim, so a blind take deadlocks
		 * once the queue fills (light traffic never fills it, sustained load does). */
		while (xSemaphoreTake(tx_stream_sem, 0) != pdTRUE) {
			uint8_t *done = NULL;
			if (sdio_slave_send_get_finished((void **)&done, portMAX_DELAY) == ESP_OK && done) {
				hosted_mempool_free(buf_mp_tx_g, done);
				xSemaphoreGive(tx_stream_sem);
			}
		}
		sendbuf = hosted_mempool_alloc(buf_mp_tx_g, aligned, 0);
		if (!sendbuf) {
			xSemaphoreGive(tx_stream_sem);
			free_tx_buf(&buf);
			continue;
		}
		build_frame(sendbuf, &buf);     /* header+payload, zero-padded to aligned */
		free_tx_buf(&buf);
		if (sdio_slave_send_queue(sendbuf, aligned, sendbuf, portMAX_DELAY) != ESP_OK) {
			hosted_mempool_free(buf_mp_tx_g, sendbuf);
			xSemaphoreGive(tx_stream_sem);
		} else {
			stream_tx_acc += aligned;
		}
		reclaim_finished();
	}
}

static void send_task(void *arg)
{
	for (;;) {
		bool worked = false;

		/* highest priority first: PRIO_Q_SERIAL(0) -> BT(1) -> OTHERS(2) */
		for (int p = 0; p < MAX_PRIORITY_QUEUES; p++) {
			uint16_t waiting = uxQueueMessagesWaiting(to_host_queue[p]);

			if (waiting) {
				process_tx_stream(to_host_queue[p], waiting);
				worked = true;
			}
		}

		reclaim_finished();
		if (!worked) {
			vTaskDelay(1);
		} else if (stream_tx_acc >= STREAM_YIELD_BYTES) {
			/* Sustained load: send_task (prio>idle) would otherwise never block
			 * (HW queue drained as fast as filled) -> idle starves -> 5s task WDT.
			 * Periodic 1-tick yield lets idle run; ~2% throughput cost. */
			stream_tx_acc = 0;
			vTaskDelay(1);
		}
	}
}

#endif

/* if_ops->write: enqueue only. Ownership of buf_handle transfers to the queue;
 * the send_task frees it after packing. */
static int32_t sdio_write(interface_handle_t *handle, interface_buffer_handle_t *buf_handle)
{
	uint8_t prio;

	if (!buf_handle || !buf_handle->payload || !buf_handle->payload_len)
		return ESP_FAIL;

	/* if_ops->write drops the producer's queue_type hint, so derive the lane
	 * from if_type (mirrors the host kmod): serial/control and BT get their own
	 * queues, everything else (wifi data, raw-TP) is OTHERS. */
	prio = (buf_handle->if_type == ESP_SERIAL_IF) ? PRIO_Q_SERIAL :
	       (buf_handle->if_type == ESP_HCI_IF)    ? PRIO_Q_BT : PRIO_Q_OTHERS;

	if (xQueueSend(to_host_queue[prio], buf_handle, portMAX_DELAY) != pdTRUE) {
		free_tx_buf(buf_handle);
		return ESP_FAIL;
	}
	return buf_handle->payload_len;
}

/* ===================== H2E receive (de-aggregating) ===================== */
static int sdio_read(interface_handle_t *if_handle, interface_buffer_handle_t *buf_handle)
{
	static uint8_t *blk_base = NULL;
	static sdio_slave_buf_handle_t blk_handle = NULL;
	static uint16_t blk_total = 0;
	static uint16_t blk_pos = 0;

	esp_err_t ret;
	struct esp_payload_header *header;
	uint16_t len, offset, frame_len, aligned_len;
	size_t sdio_read_len = 0;
	bool last_frame = false;
#if CONFIG_ESP_SDIO_CHECKSUM
	uint16_t rx_checksum, checksum;
#endif

	if (!if_handle || !buf_handle || if_handle->state != ACTIVE)
		return ESP_FAIL;

	if (!blk_base) {
		ret = sdio_slave_recv(&blk_handle, &blk_base, &sdio_read_len, portMAX_DELAY);
		if (ret) {
			blk_base = NULL;
			return ESP_FAIL;
		}
		blk_total = sdio_read_len & 0xFFFF;
		blk_pos = 0;
	}

	header = (struct esp_payload_header *)(blk_base + blk_pos);
	UPDATE_HEADER_RX_PKT_NO(header);

	len = le16toh(header->len);
	offset = le16toh(header->offset);
	frame_len = len + offset;
	aligned_len = (frame_len + 3) & ~3;

	if (!len || offset < SDIO_HDR_SIZE || (uint32_t)blk_pos + frame_len > blk_total) {
		ESP_LOGE(TAG, "Drop invalid rx frame: len=%u offset=%u pos=%u total=%u",
				len, offset, blk_pos, blk_total);
		sdio_read_done(blk_handle);
		blk_base = NULL; blk_handle = NULL; blk_total = 0; blk_pos = 0;
		return ESP_FAIL;
	}

#if CONFIG_ESP_SDIO_CHECKSUM
	rx_checksum = le16toh(header->checksum);
	header->checksum = 0;
	checksum = compute_checksum((uint8_t *)header, frame_len);
	if (checksum != rx_checksum) {
		ESP_LOGE(TAG, "sdio rx checksum mismatch, drop block");
		sdio_read_done(blk_handle);
		blk_base = NULL; blk_handle = NULL; blk_total = 0; blk_pos = 0;
		return ESP_FAIL;
	}
#endif

	if ((uint32_t)blk_pos + aligned_len + SDIO_HDR_SIZE > blk_total) {
		last_frame = true;
	} else {
		struct esp_payload_header *nh =
			(struct esp_payload_header *)(blk_base + blk_pos + aligned_len);
		if (le16toh(nh->len) == 0)
			last_frame = true;
	}

	buf_handle->payload = blk_base + blk_pos;
	buf_handle->payload_len = frame_len;
	buf_handle->if_type = header->if_type;
	buf_handle->if_num = header->if_num;

	if (last_frame) {
		buf_handle->sdio_buf_handle = blk_handle;
		buf_handle->free_buf_handle = sdio_read_done;
		blk_base = NULL; blk_handle = NULL; blk_total = 0; blk_pos = 0;
	} else {
		buf_handle->free_buf_handle = NULL;
		buf_handle->priv_buffer_handle = NULL;
		blk_pos += aligned_len;
	}

	return frame_len;
}

/* ===================== startup handshake ===================== */

/* ESP_PRIV_RX_BUF_CONFIG: slave advertises its datapath buffer sizing so the
 * host can configure its RX window without hard-coded caps.  Per direction:
 *   e2h (slave→host): TX_MODE + pool size (STREAM differs from SW_AGGR/PACKET)
 *   h2e (host→slave): always SW_AGGR + slave recv-buffer size
 * Sizes in 512-byte units (ESP_PRIV_BUF_BLOCK) to fit a uint8_t field. */
static uint8_t *tlv_append_rx_buf_config(uint8_t *pos, uint16_t *len)
{
	struct esp_priv_rx_buf_config cfg = {0};
	cfg.transport = ESP_PRIV_TPORT_SDIO;
	cfg.u.sdio.e2h_mode = TX_MODE;
#if TX_MODE == TX_MODE_STREAM
	cfg.u.sdio.e2h_bufsz_512B = (SDIO_STREAM_QUEUE_SIZE * SDIO_TX_BUFFER_SIZE) / ESP_PRIV_BUF_BLOCK;
#else
	cfg.u.sdio.e2h_bufsz_512B = sdio_rx_buf_size / ESP_PRIV_BUF_BLOCK;
#endif
	cfg.u.sdio.h2e_mode = ESP_PRIV_TXMODE_SW_AGGR;
	cfg.u.sdio.h2e_bufsz_512B = sdio_rx_buf_size / ESP_PRIV_BUF_BLOCK;
	*pos++ = ESP_PRIV_RX_BUF_CONFIG; *pos++ = sizeof(cfg);
	memcpy(pos, &cfg, sizeof(cfg)); pos += sizeof(cfg);
	*len += 2 + sizeof(cfg);
	return pos;
}

/* ESP_PRIV_CUSTOM_STR: firmware build timestamp for support/debug logs.
 * Unique per binary; host logs it in dmesg for quick version confirmation. */
static uint8_t *tlv_append_custom_str(uint8_t *pos, uint16_t *len)
{
	const char *ts = __DATE__ " " __TIME__;
	uint8_t n = (uint8_t)strlen(ts);
	*pos++ = ESP_PRIV_CUSTOM_STR; *pos++ = n;
	memcpy(pos, ts, n); pos += n;
	*len += 2 + n;
	return pos;
}

void generate_startup_event(uint8_t cap)
{
	struct esp_payload_header *header;
	struct esp_priv_event *event;
	uint8_t *pos, *payload;
	uint16_t len = 0;
	uint8_t raw_tp_cap = debug_get_raw_tp_conf();
	struct fw_version fw_ver = { 0 };

	payload = heap_caps_malloc(512, MALLOC_CAP_DMA);
	assert(payload);
	memset(payload, 0, 512);

	header = (struct esp_payload_header *)payload;
	header->if_type = ESP_PRIV_IF;
	header->if_num = 0;
	header->offset = htole16(SDIO_HDR_SIZE);
	header->priv_pkt_type = ESP_PACKET_TYPE_EVENT;
	UPDATE_HEADER_TX_PKT_NO(header);

	event = (struct esp_priv_event *)(payload + SDIO_HDR_SIZE);
	event->event_type = ESP_PRIV_EVENT_INIT;
	pos = event->event_data;

	*pos++ = ESP_PRIV_FIRMWARE_CHIP_ID;   *pos++ = LENGTH_1_BYTE; *pos++ = CONFIG_IDF_FIRMWARE_CHIP_ID; len += 3;
	*pos++ = ESP_PRIV_CAPABILITY;         *pos++ = LENGTH_1_BYTE; *pos++ = cap;        len += 3;
	*pos++ = ESP_PRIV_TEST_RAW_TP;        *pos++ = LENGTH_1_BYTE; *pos++ = raw_tp_cap; len += 3;

	pos = tlv_append_rx_buf_config(pos, &len);
	pos = tlv_append_custom_str(pos, &len);

	strlcpy(fw_ver.project_name, PROJECT_NAME, sizeof(fw_ver.project_name));
	fw_ver.major1 = PROJECT_VERSION_MAJOR_1;
	fw_ver.major2 = PROJECT_VERSION_MAJOR_2;
	fw_ver.minor  = PROJECT_VERSION_MINOR;
	fw_ver.revision_patch_1 = PROJECT_REVISION_PATCH_1;
	fw_ver.revision_patch_2 = PROJECT_REVISION_PATCH_2;
	*pos++ = ESP_PRIV_FW_DATA; *pos++ = sizeof(fw_ver);
	memcpy(pos, &fw_ver, sizeof(fw_ver)); pos += sizeof(fw_ver); len += 2 + sizeof(fw_ver);

	event->event_len = len;
	len += 2;
	header->len = htole16(len);
#if CONFIG_ESP_SDIO_CHECKSUM
	header->checksum = htole16(compute_checksum(payload, len + SDIO_HDR_SIZE));
#endif

	if (sdio_slave_transmit(payload, len + SDIO_HDR_SIZE) != ESP_OK)
		ESP_LOGE(TAG, "startup event transmit failed");
	heap_caps_free(payload);
}

/* ===================== lifecycle ===================== */
IRAM_ATTR static void event_cb(uint8_t val)
{
	if (val == ESP_RESET) {
		sdio_reset(&if_handle_g);
		return;
	}
	if (context.event_handler)
		context.event_handler(val);
}

interface_context_t *interface_insert_driver(int (*event_handler)(uint8_t val))
{
	ESP_LOGI(TAG, "Using SDIO interface (TX_MODE=%d)", TX_MODE);
	memset(&context, 0, sizeof(context));
	context.type = SDIO;
	context.if_ops = &if_ops;
	context.event_handler = event_handler;
	return &context;
}

int interface_remove_driver(void)
{
	memset(&context, 0, sizeof(context));
	return 0;
}

static void set_intr_ena(void)
{
	sdio_slave_set_host_intena(SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET |
			SDIO_SLAVE_HOSTINT_BIT0 | SDIO_SLAVE_HOSTINT_BIT1 |
			SDIO_SLAVE_HOSTINT_BIT2 | SDIO_SLAVE_HOSTINT_BIT3 |
			SDIO_SLAVE_HOSTINT_BIT4 | SDIO_SLAVE_HOSTINT_BIT5 |
			SDIO_SLAVE_HOSTINT_BIT6 | SDIO_SLAVE_HOSTINT_BIT7);
}

static interface_handle_t *sdio_init(void)
{
	esp_err_t ret;
	sdio_slave_buf_handle_t handle;

	sdio_rx_buf_size = sdio_hw_max_rx_buf_size();
	ESP_LOGI(TAG, "rx_buf_size=%"PRIu32, sdio_rx_buf_size);

	for (int i = 0; i < SDIO_RX_BUFFER_NUM; i++) {
		sdio_slave_rx_buffer[i] = heap_caps_malloc(sdio_rx_buf_size, MALLOC_CAP_DMA);
		assert(sdio_slave_rx_buffer[i]);
		memset(sdio_slave_rx_buffer[i], 0, sdio_rx_buf_size);
	}

	sdio_slave_config_t config = {
#if TX_MODE == TX_MODE_PACKET
		.sending_mode    = SDIO_SLAVE_SEND_PACKET,
#else
		.sending_mode    = SDIO_SLAVE_SEND_STREAM,
#endif
		.send_queue_size  = SDIO_DRIVER_TX_QUEUE_SIZE,
		.recv_buffer_size = sdio_rx_buf_size,
		.event_cb         = event_cb,
		/* .flags and .timing left at default (0) on C5 */
	};

	ret = sdio_slave_initialize(&config);
	if (ret != ESP_OK)
		return NULL;

	for (int i = 0; i < SDIO_RX_BUFFER_NUM; i++) {
		handle = sdio_slave_recv_register_buf(sdio_slave_rx_buffer[i]);
		assert(handle != NULL);
		ret = sdio_slave_recv_load_buf(handle);
		if (ret != ESP_OK) {
			sdio_slave_deinit();
			return NULL;
		}
	}

	set_intr_ena();

	ret = sdio_slave_start();
	if (ret != ESP_OK) {
		sdio_slave_deinit();
		return NULL;
	}

	for (int p = 0; p < MAX_PRIORITY_QUEUES; p++) {
		to_host_queue[p] = xQueueCreate(to_host_q_depth[p], sizeof(interface_buffer_handle_t));
		assert(to_host_queue[p]);
	}
#if TX_MODE == TX_MODE_SW_AGGR
	tx_aggr_buf = heap_caps_malloc(sdio_rx_buf_size, MALLOC_CAP_DMA);
	assert(tx_aggr_buf);
#elif TX_MODE == TX_MODE_PACKET
	tx_pkt_buf = heap_caps_malloc(sdio_rx_buf_size, MALLOC_CAP_DMA);
	assert(tx_pkt_buf);
#elif TX_MODE == TX_MODE_STREAM
	tx_stream_sem = xSemaphoreCreateCounting(SDIO_STREAM_QUEUE_SIZE, SDIO_STREAM_QUEUE_SIZE);
	assert(tx_stream_sem);
	/* Pre-allocated DMA TX pool (needs CONFIG_ESP_CACHE_MALLOC=y to actually pool;
	 * otherwise hosted_mempool falls back to per-call malloc and STREAM WDT-hangs
	 * under load). Sized > in-flight depth so alloc never fails once the sem is held. */
	buf_mp_tx_g = hosted_mempool_create(NULL, 0, SDIO_TX_MEMPOOL_BLOCKS, SDIO_TX_BUFFER_SIZE);
	assert(buf_mp_tx_g);
#endif
	assert(xTaskCreate(send_task, "sdio_send",
			CONFIG_ESP_DEFAULT_TASK_STACK_SIZE, NULL,
			CONFIG_ESP_HOSTED_TASK_PRIORITY_DEFAULT, NULL) == pdTRUE);

	memset(&if_handle_g, 0, sizeof(if_handle_g));
	if_handle_g.state = INIT;
	return &if_handle_g;
}

static esp_err_t sdio_reset(interface_handle_t *handle)
{
	esp_err_t ret;

	sdio_slave_stop();
	ret = sdio_slave_reset();
	if (ret != ESP_OK)
		return ret;

	set_intr_ena();

	ret = sdio_slave_start();
	if (ret != ESP_OK)
		return ret;

	while (1) {
		void *finished = NULL;

		if (sdio_slave_send_get_finished(&finished, 0) != ESP_OK)
			break;
#if TX_MODE == TX_MODE_STREAM
		if (finished) {
			hosted_mempool_free(buf_mp_tx_g, finished);
			xSemaphoreGive(tx_stream_sem);
		}
#endif
	}
	return ESP_OK;
}

static void sdio_deinit(interface_handle_t *handle)
{
	sdio_slave_stop();
	sdio_slave_reset();

	for (int i = 0; i < SDIO_RX_BUFFER_NUM; i++) {
		heap_caps_free(sdio_slave_rx_buffer[i]);
		sdio_slave_rx_buffer[i] = NULL;
	}
#if TX_MODE == TX_MODE_SW_AGGR
	heap_caps_free(tx_aggr_buf); tx_aggr_buf = NULL;
#elif TX_MODE == TX_MODE_PACKET
	heap_caps_free(tx_pkt_buf); tx_pkt_buf = NULL;
#elif TX_MODE == TX_MODE_STREAM
	hosted_mempool_destroy(buf_mp_tx_g); buf_mp_tx_g = NULL;
#endif
}
