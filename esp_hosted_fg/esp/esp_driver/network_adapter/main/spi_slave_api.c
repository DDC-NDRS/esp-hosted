// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2021 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sdkconfig.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <endian.h>
#include "soc/gpio_reg.h"
#include "esp_log.h"
#include "interface.h"
#include "adapter.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "mempool.h"
#include "stats.h"
#include "esp_timer.h"
#include "esp_fw_version.h"

// de-assert HS signal on CS, instead of at end of transaction
#if defined(CONFIG_ESP_SPI_DEASSERT_HS_ON_CS)
#define HS_DEASSERT_ON_CS (1)
#else
#define HS_DEASSERT_ON_CS (0)
#endif

static const char TAG[] = "SPI_DRIVER";
/* SPI settings */
#define SPI_BITS_PER_WORD          8
#define ESP_SPI_MODE               CONFIG_ESP_SPI_MODE
#define GPIO_MOSI                  CONFIG_ESP_SPI_GPIO_MOSI
#define GPIO_MISO                  CONFIG_ESP_SPI_GPIO_MISO
#define GPIO_SCLK                  CONFIG_ESP_SPI_GPIO_CLK
#define GPIO_CS                    CONFIG_ESP_SPI_GPIO_CS
#define GPIO_DATA_READY            CONFIG_ESP_SPI_GPIO_DATA_READY
#define GPIO_HANDSHAKE             CONFIG_ESP_SPI_GPIO_HANDSHAKE

#define ESP_SPI_CONTROLLER         CONFIG_ESP_SPI_CONTROLLER

/* SPI-DMA settings */
#define SPI_DMA_ALIGNMENT_BYTES    4
#define SPI_DMA_ALIGNMENT_MASK     (SPI_DMA_ALIGNMENT_BYTES-1)
#define IS_SPI_DMA_ALIGNED(VAL)    (!((VAL)& SPI_DMA_ALIGNMENT_MASK))
#define MAKE_SPI_DMA_ALIGNED(VAL)  (VAL += SPI_DMA_ALIGNMENT_BYTES - \
				((VAL)& SPI_DMA_ALIGNMENT_MASK))

#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2)
    #define DMA_CHAN               ESP_SPI_CONTROLLER
#else
    #define DMA_CHAN               SPI_DMA_CH_AUTO
#endif

#if ESP_SPI_MODE==0
#  error "SPI mode 0 at SLAVE is NOT supported"
#endif
/* SPI internal configs */
#define SPI_BUFFER_SIZE            MAX_TRANSPORT_BUF_SIZE

#define GPIO_MASK_DATA_READY (1ULL << GPIO_DATA_READY)
#define GPIO_MASK_HANDSHAKE (1ULL << GPIO_HANDSHAKE)

#if HS_DEASSERT_ON_CS
#define H_CS_INTR_TO_CLEAR_HS                        GPIO_INTR_ANYEDGE
#else
#define H_CS_INTR_TO_CLEAR_HS                        GPIO_INTR_NEGEDGE
#endif

/* Max SPI slave CLK in IO_MUX tested in IDF:
 * ESP32: 10MHz
 * ESP32-C2/C3/S2/S3: 40MHz
 * ESP32-C6: 26MHz
 */

#define H_HS_PULL_REGISTER                         GPIO_PULLDOWN_ONLY
#define H_DR_PULL_REGISTER                         GPIO_PULLDOWN_ONLY

#define SPI_DRIVER_QUEUE_SIZE      3

#ifdef CONFIG_ESP_ENABLE_TX_PRIORITY_QUEUES
    #define SPI_TX_WIFI_QUEUE_SIZE     CONFIG_ESP_TX_WIFI_Q_SIZE
    #define SPI_TX_BT_QUEUE_SIZE       CONFIG_ESP_TX_BT_Q_SIZE
    #define SPI_TX_SERIAL_QUEUE_SIZE   CONFIG_ESP_TX_SERIAL_Q_SIZE
    #define SPI_TX_TOTAL_QUEUE_SIZE (SPI_TX_WIFI_QUEUE_SIZE+SPI_TX_BT_QUEUE_SIZE+SPI_TX_SERIAL_QUEUE_SIZE)
#else
    #define SPI_TX_QUEUE_SIZE          CONFIG_ESP_TX_Q_SIZE
    #define SPI_TX_TOTAL_QUEUE_SIZE    SPI_TX_QUEUE_SIZE
#endif

#ifdef CONFIG_ESP_ENABLE_RX_PRIORITY_QUEUES
    #define SPI_RX_WIFI_QUEUE_SIZE     CONFIG_ESP_RX_WIFI_Q_SIZE
    #define SPI_RX_BT_QUEUE_SIZE       CONFIG_ESP_RX_BT_Q_SIZE
    #define SPI_RX_SERIAL_QUEUE_SIZE   CONFIG_ESP_RX_SERIAL_Q_SIZE
    #define SPI_RX_TOTAL_QUEUE_SIZE (SPI_RX_WIFI_QUEUE_SIZE+SPI_RX_BT_QUEUE_SIZE+SPI_RX_SERIAL_QUEUE_SIZE)
#else
    #define SPI_RX_QUEUE_SIZE          CONFIG_ESP_RX_Q_SIZE
    #define SPI_RX_TOTAL_QUEUE_SIZE    SPI_RX_QUEUE_SIZE
#endif

static interface_context_t context;
static interface_handle_t if_handle_g;

#ifdef CONFIG_ESP_ENABLE_TX_PRIORITY_QUEUES
  static QueueHandle_t spi_tx_queue[MAX_PRIORITY_QUEUES];
  static SemaphoreHandle_t spi_tx_sem;
#else
  static QueueHandle_t spi_tx_queue;
#endif

#ifdef CONFIG_ESP_ENABLE_RX_PRIORITY_QUEUES
  static QueueHandle_t spi_rx_queue[MAX_PRIORITY_QUEUES];
  static SemaphoreHandle_t spi_rx_sem;
#else
  static QueueHandle_t spi_rx_queue;
#endif

#if HS_DEASSERT_ON_CS
static SemaphoreHandle_t wait_cs_deassert_sem;
#endif
static interface_handle_t * esp_spi_init(void);
static int32_t esp_spi_write(interface_handle_t *handle,
				interface_buffer_handle_t *buf_handle);
static int esp_spi_read(interface_handle_t *if_handle, interface_buffer_handle_t * buf_handle);
static esp_err_t esp_spi_reset(interface_handle_t *handle);
static void esp_spi_deinit(interface_handle_t *handle);
static void esp_spi_read_done(void *handle);
static void queue_next_transaction(void);

if_ops_t if_ops = {
	.init = esp_spi_init,
	.write = esp_spi_write,
	.read = esp_spi_read,
	.reset = esp_spi_reset,
	.deinit = esp_spi_deinit,
};

static struct hosted_mempool * buf_mp_tx_g;
static struct hosted_mempool * buf_mp_rx_g;
static struct hosted_mempool * trans_mp_g;

/* Full size dummy buffer for no-data transactions */
static DRAM_ATTR uint8_t dummy_buffer[SPI_BUFFER_SIZE] __attribute__((aligned(4)));

static inline void spi_mempool_create()
{
#ifdef CONFIG_ESP_CACHE_MALLOC
	/* Create separate pools for TX and RX with optimized sizes */
	buf_mp_tx_g = hosted_mempool_create(NULL, 0,
			(SPI_TX_TOTAL_QUEUE_SIZE + SPI_DRIVER_QUEUE_SIZE + 1), SPI_BUFFER_SIZE);

	buf_mp_rx_g = hosted_mempool_create(NULL, 0,
			(SPI_RX_TOTAL_QUEUE_SIZE + SPI_DRIVER_QUEUE_SIZE + SPI_DRIVER_QUEUE_SIZE), SPI_BUFFER_SIZE);

	trans_mp_g = hosted_mempool_create(NULL, 0,
			SPI_DRIVER_QUEUE_SIZE, sizeof(spi_slave_transaction_t));


	assert(buf_mp_tx_g);
	assert(buf_mp_rx_g);
	assert(trans_mp_g);
#else
	ESP_LOGI(TAG, "Using dynamic heap for mem alloc");
#endif
}

static inline void spi_mempool_destroy()
{
#ifdef CONFIG_ESP_CACHE_MALLOC
	hosted_mempool_destroy(buf_mp_tx_g);
	if (buf_mp_tx_g!=buf_mp_rx_g) {
		hosted_mempool_destroy(buf_mp_rx_g);
	}
	hosted_mempool_destroy(trans_mp_g);
#endif
}

static inline void *spi_buffer_tx_alloc(uint need_memset)
{
#ifdef CONFIG_ESP_CACHE_MALLOC
	return hosted_mempool_alloc(buf_mp_tx_g, SPI_BUFFER_SIZE, need_memset);
#else
	void *buf = MEM_ALLOC(SPI_BUFFER_SIZE);
	if (buf && need_memset) {
		memset(buf, 0, SPI_BUFFER_SIZE);
	}
	return buf;
#endif
}

static inline void *spi_buffer_rx_alloc(uint need_memset)
{
#ifdef CONFIG_ESP_CACHE_MALLOC
	return hosted_mempool_alloc(buf_mp_rx_g, SPI_BUFFER_SIZE, need_memset);
#else
	void *buf = MEM_ALLOC(SPI_BUFFER_SIZE);
	if (buf && need_memset) {
		memset(buf, 0, SPI_BUFFER_SIZE);
	}
	return buf;
#endif
}

static inline spi_slave_transaction_t *spi_trans_alloc(uint need_memset)
{
#ifdef CONFIG_ESP_CACHE_MALLOC
	return hosted_mempool_alloc(trans_mp_g, sizeof(spi_slave_transaction_t), need_memset);
#else
	spi_slave_transaction_t *trans = MEM_ALLOC(sizeof(spi_slave_transaction_t));
	if (trans && need_memset) {
		memset(trans, 0, sizeof(spi_slave_transaction_t));
	}
	return trans;
#endif
}

static inline void spi_buffer_tx_free(void *buf)
{
#ifdef CONFIG_ESP_CACHE_MALLOC
	hosted_mempool_free(buf_mp_tx_g, buf);
#else
	FREE(buf);
#endif
}

static inline void spi_buffer_rx_free(void *buf)
{
#ifdef CONFIG_ESP_CACHE_MALLOC
	hosted_mempool_free(buf_mp_rx_g, buf);
#else
	FREE(buf);
#endif
}

static inline void spi_trans_free(spi_slave_transaction_t *trans)
{
#ifdef CONFIG_ESP_CACHE_MALLOC
	hosted_mempool_free(trans_mp_g, trans);
#else
	FREE(trans);
#endif
}

#define set_handshake_gpio()     gpio_set_level(GPIO_HANDSHAKE, 1);
#define reset_handshake_gpio()   gpio_set_level(GPIO_HANDSHAKE, 0);
#define set_dataready_gpio()     gpio_set_level(GPIO_DATA_READY, 1);
#define reset_dataready_gpio()   gpio_set_level(GPIO_DATA_READY, 0);

interface_context_t *interface_insert_driver(int (*event_handler)(uint8_t val))
{
	ESP_LOGI(TAG, "Using SPI interface");
	memset(&context, 0, sizeof(context));

	context.type = SPI;
	context.if_ops = &if_ops;
	context.event_handler = event_handler;

	return &context;
}

int interface_remove_driver()
{
	memset(&context, 0, sizeof(context));
	return 0;
}

void generate_startup_event(uint8_t cap)
{
	struct esp_payload_header *header = NULL;
	interface_buffer_handle_t buf_handle = {0};
	struct esp_priv_event *event = NULL;
	uint8_t *pos = NULL;
	uint16_t len = 0;
	uint8_t raw_tp_cap = 0;
	uint32_t total_len = 0;
	struct fw_version fw_ver = { 0 };

	buf_handle.payload = spi_buffer_tx_alloc(MEMSET_REQUIRED);

	raw_tp_cap = debug_get_raw_tp_conf();

	assert(buf_handle.payload);
	header = (struct esp_payload_header *) buf_handle.payload;

	header->if_type = ESP_PRIV_IF;
	header->if_num = 0;
	header->offset = htole16(sizeof(struct esp_payload_header));
	header->priv_pkt_type = ESP_PACKET_TYPE_EVENT;

	/* Populate event data */
	event = (struct esp_priv_event *) (buf_handle.payload + sizeof(struct esp_payload_header));

	event->event_type = ESP_PRIV_EVENT_INIT;

	/* Populate TLVs for event */
	pos = event->event_data;

	/* TLVs start */

	/* TLV - Board type */
	ESP_LOGI(TAG, "Slave chip Id[%x]", ESP_PRIV_FIRMWARE_CHIP_ID);
	*pos = ESP_PRIV_FIRMWARE_CHIP_ID;   pos++;len++;
	*pos = LENGTH_1_BYTE;               pos++;len++;
	*pos = CONFIG_IDF_FIRMWARE_CHIP_ID; pos++;len++;

	/* TLV - Capability */
	*pos = ESP_PRIV_CAPABILITY;         pos++;len++;
	*pos = LENGTH_1_BYTE;               pos++;len++;
	*pos = cap;                         pos++;len++;

	*pos = ESP_PRIV_TEST_RAW_TP;        pos++;len++;
	*pos = LENGTH_1_BYTE;               pos++;len++;
	*pos = raw_tp_cap;                  pos++;len++;

	/* fill structure with fw info */
	strlcpy(fw_ver.project_name, PROJECT_NAME, sizeof(fw_ver.project_name));
	fw_ver.major1 = PROJECT_VERSION_MAJOR_1;
	fw_ver.major2 = PROJECT_VERSION_MAJOR_2;
	fw_ver.minor  = PROJECT_VERSION_MINOR;
	fw_ver.revision_patch_1 = PROJECT_REVISION_PATCH_1;
	fw_ver.revision_patch_2 = PROJECT_REVISION_PATCH_2;

	/* TLV - Firmware Version */
	*pos = ESP_PRIV_FW_DATA;            pos++;len++;
	*pos = sizeof(fw_ver);              pos++;len++;
	memcpy(pos, &fw_ver, sizeof(fw_ver));
	pos += sizeof(fw_ver);
	len += sizeof(fw_ver);

	/* TLVs end */

	event->event_len = len;

	/* payload len = Event len + sizeof(event type) + sizeof(event len) */
	len += 2;
	header->len = htole16(len);

	total_len = len + sizeof(struct esp_payload_header);

	if (!IS_SPI_DMA_ALIGNED(total_len)) {
		MAKE_SPI_DMA_ALIGNED(total_len);
	}

	buf_handle.payload_len = total_len;

#if CONFIG_ESP_SPI_CHECKSUM
	header->checksum = htole16(compute_checksum(buf_handle.payload, len + sizeof(struct esp_payload_header)));
#endif

#ifdef CONFIG_ESP_ENABLE_TX_PRIORITY_QUEUES
	xQueueSend(spi_tx_queue[PRIO_Q_OTHERS], &buf_handle, portMAX_DELAY);
	xSemaphoreGive(spi_tx_sem);
#else
	xQueueSend(spi_tx_queue, &buf_handle, portMAX_DELAY);
#endif

	set_dataready_gpio();
	/* process first data packet here to start transactions */
	queue_next_transaction();
}


/* Invoked after transaction is queued and ready for pickup by master */
static void IRAM_ATTR spi_post_setup_cb(spi_slave_transaction_t *trans)
{
	/* ESP peripheral ready for spi transaction. Set hadnshake line high. */
	set_handshake_gpio();
}

/* Invoked after transaction is sent/received.
 * Use this to set the handshake line low */
static void IRAM_ATTR spi_post_trans_cb(spi_slave_transaction_t *trans)
{
#if !HS_DEASSERT_ON_CS
	/* Clear handshake line */
	reset_handshake_gpio();
#endif
}

static uint8_t * get_next_tx_buffer(uint32_t *len)
{
	interface_buffer_handle_t buf_handle = {0};
	esp_err_t ret = ESP_OK;


	#ifdef CONFIG_ESP_ENABLE_TX_PRIORITY_QUEUES
	ret = xSemaphoreTake(spi_tx_sem, 0);
	if (pdTRUE == ret) {

		if (pdFALSE == xQueueReceive(spi_tx_queue[PRIO_Q_SERIAL], &buf_handle, 0))
			if (pdFALSE == xQueueReceive(spi_tx_queue[PRIO_Q_BT], &buf_handle, 0))
				if (pdFALSE == xQueueReceive(spi_tx_queue[PRIO_Q_OTHERS], &buf_handle, 0))
					ret = pdFALSE;
	}
	#else
	ret = xQueueReceive(spi_tx_queue, &buf_handle, 0);
	#endif

	if (ret == pdTRUE && buf_handle.payload) {
		struct esp_payload_header *header = (struct esp_payload_header *)buf_handle.payload;
		ESP_LOGD(TAG, "[TX] Real data queued - if_type: %d, len: %d",
				 header->if_type, le16toh(header->len));
		if (len) {
#if ESP_PKT_STATS
			if (buf_handle.if_type == ESP_SERIAL_IF)
				pkt_stats.serial_tx_total++;
#endif
			*len = buf_handle.payload_len;
		}
		return buf_handle.payload;
	}

	/* No real data, using dummy buffer */
	ESP_LOGV(TAG, "[TX] No data - using dummy buffer");
	reset_dataready_gpio();
	return dummy_buffer;
}

static int process_spi_rx(interface_buffer_handle_t *buf_handle)
{
	struct esp_payload_header *header;

	if (!buf_handle || !buf_handle->payload) {
		ESP_LOGE(TAG, "Invalid RX buffer");
		return -1;
	}

	header = (struct esp_payload_header *) buf_handle->payload;

	/* Log packet info */
	ESP_LOGV(TAG, "[RX] if_type: %d, len: %d, offset: %d",
			 header->if_type, le16toh(header->len), le16toh(header->offset));

	UPDATE_HEADER_RX_PKT_NO(header);

	ESP_HEXLOGV("spi_rx:", header, 16, 16);

	uint16_t len = le16toh(header->len);
	uint16_t offset = le16toh(header->offset);
	uint8_t flags = header->flags;

	if (!len) {
		ESP_LOGV(TAG, "Rx pkt len:0, drop");
		return -1;
	}

	if (!offset) {
		ESP_LOGD(TAG, "Rx pkt offset:0, drop");
		return -1;
	}

	if ((len+offset) > SPI_BUFFER_SIZE) {
		ESP_LOGE(TAG, "rx_pkt len+offset[%u]>max[%u], dropping it", len+offset, SPI_BUFFER_SIZE);
		return -1;
	}

	ESP_LOGV(TAG, "RX: len=%u offset=%u flags=0x%x payload_addr=%p",
		len, offset, flags, buf_handle->payload);

	if (flags & FLAG_POWER_SAVE_STARTED) {
		ESP_LOGI(TAG, "Host informed starting to power sleep");
		if (context.event_handler) {
			context.event_handler(ESP_POWER_SAVE_ON);
		}
	} else if (flags & FLAG_POWER_SAVE_STOPPED) {
		ESP_LOGI(TAG, "Host informed that it waken up");
		if (context.event_handler) {
			context.event_handler(ESP_POWER_SAVE_OFF);
		}
	}

#if CONFIG_ESP_SPI_CHECKSUM
	uint16_t rx_checksum = le16toh(header->checksum);
	header->checksum = 0;
	uint16_t checksum = compute_checksum(buf_handle->payload, (len + offset));

	if (checksum != rx_checksum) {
		ESP_LOGE(TAG, "%s: cal_chksum[%u] != exp_chksum[%u], drop len[%u] offset[%u]",
				__func__, checksum, rx_checksum, len, offset);
		return -1;
	}
#endif

	/* Buffer is valid */
	buf_handle->if_type = header->if_type;
	buf_handle->if_num = header->if_num;
	buf_handle->free_buf_handle = esp_spi_read_done;
	buf_handle->payload_len = len + offset;
	buf_handle->priv_buffer_handle = buf_handle->payload;

#if ESP_PKT_STATS
	if (buf_handle->if_type == ESP_STA_IF)
		pkt_stats.hs_bus_sta_in++;
#endif
#ifdef CONFIG_ESP_ENABLE_RX_PRIORITY_QUEUES
	if (header->if_type == ESP_SERIAL_IF) {
		xQueueSend(spi_rx_queue[PRIO_Q_SERIAL], buf_handle, portMAX_DELAY);
	} else if (header->if_type == ESP_HCI_IF) {
		xQueueSend(spi_rx_queue[PRIO_Q_BT], buf_handle, portMAX_DELAY);
	} else {
		xQueueSend(spi_rx_queue[PRIO_Q_OTHERS], buf_handle, portMAX_DELAY);
	}

	xSemaphoreGive(spi_rx_sem);
#else
	xQueueSend(spi_rx_queue, buf_handle, portMAX_DELAY);
#endif

	return 0;
}

static void queue_next_transaction(void)
{
	spi_slave_transaction_t *spi_trans = NULL;
	uint32_t len = 0;
	uint8_t *tx_buffer = get_next_tx_buffer(&len);
	if (!tx_buffer) {
		/* Queue next transaction failed */
		ESP_LOGE(TAG , "Failed to queue new transaction\r\n");
		return;
	}

	spi_trans = spi_trans_alloc(MEMSET_REQUIRED);
	assert(spi_trans);

	/* Use RX mempool instead of direct heap allocation */
	uint8_t *rx_buffer = spi_buffer_rx_alloc(MEMSET_REQUIRED);
	assert(rx_buffer);

	spi_trans->rx_buffer = rx_buffer;
	spi_trans->tx_buffer = tx_buffer;
	spi_trans->length = SPI_BUFFER_SIZE * SPI_BITS_PER_WORD;

	spi_slave_queue_trans(ESP_SPI_CONTROLLER, spi_trans, portMAX_DELAY);
}

static void spi_transaction_post_process_task(void* pvParameters)
{
	spi_slave_transaction_t *spi_trans = NULL;
	esp_err_t ret = ESP_OK;
	interface_buffer_handle_t rx_buf_handle;

	for (;;) {
		memset(&rx_buf_handle, 0, sizeof(rx_buf_handle));
		/* Wait for transaction completion */
		ESP_ERROR_CHECK(spi_slave_get_trans_result(ESP_SPI_CONTROLLER, &spi_trans, portMAX_DELAY));

#if HS_DEASSERT_ON_CS
		/* Wait until CS has been deasserted before we queue a new transaction.
		 *
		 * Some MCUs delay deasserting CS at the end of a transaction.
		 * If we queue a new transaction without waiting for CS to deassert,
		 * the slave SPI can start (since CS is still asserted), and data is lost
		 * as host is not expecting any data.
		 */
		xSemaphoreTake(wait_cs_deassert_sem, portMAX_DELAY);
#endif
		/* Queue new transaction to get ready as soon as possible */
		queue_next_transaction();

		/* Process received data */
		if (spi_trans->rx_buffer) {
			rx_buf_handle.payload = spi_trans->rx_buffer;
			ret = process_spi_rx(&rx_buf_handle);
		}

		ESP_HEXLOGV("spi_tx:", (uint8_t*)spi_trans->tx_buffer, 16, 16);
		/* Free buffers */
		if (spi_trans->tx_buffer != dummy_buffer) {
			spi_buffer_tx_free((void *)spi_trans->tx_buffer);
		}

#if ESP_PKT_STATS
		struct esp_payload_header *header =
			(struct esp_payload_header *)spi_trans->tx_buffer;
		if (header->if_type == ESP_STA_IF)
			pkt_stats.sta_sh_out++;
#endif
		if (ret != ESP_OK && spi_trans->rx_buffer) {
			spi_buffer_rx_free((void *)spi_trans->rx_buffer);
		}

		spi_trans_free(spi_trans);
	}
}

static void IRAM_ATTR gpio_disable_hs_isr_handler(void* arg)
{
#if HS_DEASSERT_ON_CS
	int level = gpio_get_level(GPIO_CS);
	if (level == 0) {
		/* CS is asserted, disable HS */
		reset_handshake_gpio();
	} else {
		/* Last transaction complete, populate next one */
		if (wait_cs_deassert_sem)
			xSemaphoreGive(wait_cs_deassert_sem);
	}
#else
	reset_handshake_gpio();
#endif
}

static void register_hs_disable_pin(uint32_t gpio_num)
{
	if (gpio_num != -1) {
		gpio_reset_pin(gpio_num);

		gpio_config_t slave_disable_hs_pin_conf={
			.intr_type=GPIO_INTR_DISABLE,
			.mode=GPIO_MODE_INPUT,
			.pin_bit_mask=(1ULL<<gpio_num)
		};
		slave_disable_hs_pin_conf.pull_up_en = 1;
		gpio_config(&slave_disable_hs_pin_conf);
		gpio_set_intr_type(gpio_num, H_CS_INTR_TO_CLEAR_HS);
		gpio_install_isr_service(0);
		gpio_isr_handler_add(gpio_num, gpio_disable_hs_isr_handler, NULL);
	}
}

static interface_handle_t * esp_spi_init(void)
{
	esp_err_t ret = ESP_OK;

	struct esp_payload_header *header = NULL;
	/* Configuration for the SPI bus */
	spi_bus_config_t buscfg={
		.mosi_io_num=GPIO_MOSI,
		.miso_io_num=GPIO_MISO,
		.sclk_io_num=GPIO_SCLK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = SPI_BUFFER_SIZE,
#if 0
		/*
		 * Moving ESP32 SPI slave interrupts in flash, Keeping it in IRAM gives crash,
		 * While performing flash erase operation.
		 */
		.intr_flags=ESP_INTR_FLAG_IRAM
#endif
	};

	/* Configuration for the SPI slave interface */
	spi_slave_interface_config_t slvcfg={
		.mode=ESP_SPI_MODE,
		.spics_io_num=GPIO_CS,
		.queue_size=SPI_DRIVER_QUEUE_SIZE,
		.flags=0,
		.post_setup_cb=spi_post_setup_cb,
		.post_trans_cb=spi_post_trans_cb
	};

	/* Configuration for the handshake line */
	gpio_config_t io_conf={
		.intr_type=GPIO_INTR_DISABLE,
		.mode=GPIO_MODE_OUTPUT,
		.pin_bit_mask=GPIO_MASK_HANDSHAKE
	};

	/* Configuration for data_ready line */
	gpio_config_t io_data_ready_conf={
		.intr_type=GPIO_INTR_DISABLE,
		.mode=GPIO_MODE_OUTPUT,
		.pin_bit_mask=GPIO_MASK_DATA_READY
	};

	spi_mempool_create();

	/* Configure handshake and data_ready lines as output */
	gpio_config(&io_conf);
	gpio_config(&io_data_ready_conf);
	reset_handshake_gpio();
	reset_dataready_gpio();

	header = (struct esp_payload_header *) dummy_buffer;
	memset(dummy_buffer, 0, sizeof(struct esp_payload_header));

	/* Populate header to indicate it as a dummy buffer */
	header->if_type = ESP_MAX_IF;
	header->if_num = 0xF;
	header->len = 0;


	/* Enable pull-ups on SPI lines
	 * so that no rogue pulses when no master is connected
	 */
	gpio_set_pull_mode(CONFIG_ESP_SPI_GPIO_HANDSHAKE, H_HS_PULL_REGISTER);
	gpio_set_pull_mode(CONFIG_ESP_SPI_GPIO_DATA_READY, H_DR_PULL_REGISTER);
	gpio_set_pull_mode(GPIO_MOSI, GPIO_PULLUP_ONLY);
	gpio_set_pull_mode(GPIO_SCLK, GPIO_PULLUP_ONLY);
	gpio_set_pull_mode(GPIO_CS, GPIO_PULLUP_ONLY);

	ESP_LOGI(TAG, "SPI Ctrl:%u mode: %u, Freq:ConfigAtHost\nGPIOs: MOSI: %u, MISO: %u, CS: %u, CLK: %u HS: %u DR: %u\n",
			ESP_SPI_CONTROLLER, slvcfg.mode,
			GPIO_MOSI, GPIO_MISO, GPIO_CS, GPIO_SCLK,
			GPIO_HANDSHAKE, GPIO_DATA_READY);

#ifdef CONFIG_ESP_ENABLE_TX_PRIORITY_QUEUES
	ESP_LOGI(TAG, "TX Queues :Wifi[%u]+bt[%u]+serial[%u] = %u",
			SPI_TX_WIFI_QUEUE_SIZE, SPI_TX_BT_QUEUE_SIZE, SPI_TX_SERIAL_QUEUE_SIZE,
			SPI_TX_TOTAL_QUEUE_SIZE);
#else
	ESP_LOGI(TAG, "TX Queues:%u", SPI_TX_TOTAL_QUEUE_SIZE);
#endif

#ifdef CONFIG_ESP_ENABLE_RX_PRIORITY_QUEUES
	ESP_LOGI(TAG, "RX Queues :Wifi[%u]+bt[%u]+serial[%u] = %u",
			SPI_RX_WIFI_QUEUE_SIZE, SPI_RX_BT_QUEUE_SIZE, SPI_RX_SERIAL_QUEUE_SIZE,
			SPI_RX_TOTAL_QUEUE_SIZE);
#else
	ESP_LOGI(TAG, "RX Queues:%u", SPI_RX_TOTAL_QUEUE_SIZE);
#endif
	register_hs_disable_pin(GPIO_CS);

	/* Initialize SPI slave interface */
	ret=spi_slave_initialize(ESP_SPI_CONTROLLER, &buscfg, &slvcfg, DMA_CHAN);
	assert(ret==ESP_OK);

	//gpio_set_drive_capability(CONFIG_ESP_SPI_GPIO_HANDSHAKE, GPIO_DRIVE_CAP_3);
	//gpio_set_drive_capability(CONFIG_ESP_SPI_GPIO_DATA_READY, GPIO_DRIVE_CAP_3);
	gpio_set_drive_capability(GPIO_SCLK, GPIO_DRIVE_CAP_3);
	gpio_set_drive_capability(GPIO_MISO, GPIO_DRIVE_CAP_3);
	gpio_set_pull_mode(GPIO_MISO, GPIO_PULLDOWN_ONLY);


	memset(&if_handle_g, 0, sizeof(if_handle_g));
	if_handle_g.state = INIT;

#if HS_DEASSERT_ON_CS
	wait_cs_deassert_sem = xSemaphoreCreateBinary();
	assert(wait_cs_deassert_sem!= NULL);
	/* Clear the semaphore */
	xSemaphoreTake(wait_cs_deassert_sem, 0);
#endif

#ifdef CONFIG_ESP_ENABLE_TX_PRIORITY_QUEUES
	spi_tx_sem = xSemaphoreCreateCounting(SPI_TX_TOTAL_QUEUE_SIZE, 0);
	assert(spi_tx_sem);

	spi_tx_queue[PRIO_Q_OTHERS] = xQueueCreate(SPI_TX_WIFI_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
	assert(spi_tx_queue[PRIO_Q_OTHERS]);
	spi_tx_queue[PRIO_Q_BT] = xQueueCreate(SPI_TX_BT_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
	assert(spi_tx_queue[PRIO_Q_BT]);
	spi_tx_queue[PRIO_Q_SERIAL] = xQueueCreate(SPI_TX_SERIAL_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
	assert(spi_tx_queue[PRIO_Q_SERIAL]);
#else
	spi_tx_queue = xQueueCreate(SPI_TX_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
	assert(spi_tx_queue);
#endif

#ifdef CONFIG_ESP_ENABLE_RX_PRIORITY_QUEUES
	spi_rx_sem = xSemaphoreCreateCounting(SPI_RX_TOTAL_QUEUE_SIZE, 0);
	assert(spi_rx_sem);

	spi_rx_queue[PRIO_Q_OTHERS] = xQueueCreate(SPI_RX_WIFI_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
	assert(spi_rx_queue[PRIO_Q_OTHERS]);
	spi_rx_queue[PRIO_Q_BT] = xQueueCreate(SPI_RX_BT_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
	assert(spi_rx_queue[PRIO_Q_BT]);
	spi_rx_queue[PRIO_Q_SERIAL] = xQueueCreate(SPI_RX_SERIAL_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
	assert(spi_rx_queue[PRIO_Q_SERIAL]);
#else
	spi_rx_queue = xQueueCreate(SPI_RX_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
	assert(spi_rx_queue);
#endif


	assert(xTaskCreate(spi_transaction_post_process_task , "spi_post_process_task" ,
			CONFIG_ESP_DEFAULT_TASK_STACK_SIZE, NULL,
			CONFIG_ESP_HOSTED_TASK_PRIORITY_DEFAULT, NULL) == pdTRUE);

	usleep(500);

	return &if_handle_g;
}

static int32_t esp_spi_write(interface_handle_t *handle, interface_buffer_handle_t *buf_handle)
{
	int32_t total_len = 0;
	struct esp_payload_header *header;
	interface_buffer_handle_t tx_buf_handle = {0};

	/* Basic validation */
	if (!handle || !buf_handle || !buf_handle->payload) {
		ESP_LOGE(TAG, "Invalid args - handle:%p buf:%p payload:%p",
				handle, buf_handle, buf_handle ? buf_handle->payload : NULL);
		return ESP_FAIL;
	}

	/* Length validation */
	if (!buf_handle->payload_len || buf_handle->payload_len > (SPI_BUFFER_SIZE-sizeof(struct esp_payload_header))) {
		ESP_LOGE(TAG, "Invalid payload length:%d", buf_handle->payload_len);
		return ESP_FAIL;
	}

	/* Calculate total length */
	total_len = buf_handle->payload_len + sizeof(struct esp_payload_header);

	/* DMA alignment check */
	if (!IS_SPI_DMA_ALIGNED(total_len)) {
		MAKE_SPI_DMA_ALIGNED(total_len);
	}

	if (total_len > SPI_BUFFER_SIZE) {
		ESP_LOGE(TAG, "Total length %" PRId32 " exceeds max %d", total_len, SPI_BUFFER_SIZE);
		return ESP_FAIL;
	}

	/* Allocate and validate TX buffer */
	tx_buf_handle.payload = spi_buffer_tx_alloc(MEMSET_NOT_REQUIRED);
	if (!tx_buf_handle.payload) {
		ESP_LOGE(TAG, "TX buffer allocation failed");
		return ESP_FAIL;
	}

	/* Setup header */
	header = (struct esp_payload_header *)tx_buf_handle.payload;
	memset(header, 0, sizeof(struct esp_payload_header));

	header->if_type = buf_handle->if_type;
	header->if_num = buf_handle->if_num;
	header->len = htole16(buf_handle->payload_len);
	header->offset = htole16(sizeof(struct esp_payload_header));
	header->seq_num = htole16(buf_handle->seq_num);
	header->flags = buf_handle->flag;

	/* Copy payload data */
	memcpy(tx_buf_handle.payload + sizeof(struct esp_payload_header),
			buf_handle->payload, buf_handle->payload_len);

	tx_buf_handle.if_type = buf_handle->if_type;
	tx_buf_handle.if_num = buf_handle->if_num;
	tx_buf_handle.payload_len = total_len;

#if CONFIG_ESP_SPI_CHECKSUM
	/* Calculate checksum with header checksum field zeroed */
	header->checksum = 0;
	uint16_t checksum = compute_checksum(tx_buf_handle.payload,
			sizeof(struct esp_payload_header)+buf_handle->payload_len);
	header->checksum = htole16(checksum);
#endif

	ESP_LOGV(TAG, "[TX] Packet - type:%u len:%" PRIu16 " total:% " PRId32,
			header->if_type, buf_handle->payload_len, total_len);

#ifdef CONFIG_ESP_ENABLE_TX_PRIORITY_QUEUES
	if (header->if_type == ESP_SERIAL_IF)
		xQueueSend(spi_tx_queue[PRIO_Q_SERIAL], &tx_buf_handle, portMAX_DELAY);
	else if (header->if_type == ESP_HCI_IF)
		xQueueSend(spi_tx_queue[PRIO_Q_BT], &tx_buf_handle, portMAX_DELAY);
	else
		xQueueSend(spi_tx_queue[PRIO_Q_OTHERS], &tx_buf_handle, portMAX_DELAY);

	xSemaphoreGive(spi_tx_sem);
#else
	xQueueSend(spi_tx_queue, &tx_buf_handle, portMAX_DELAY);
#endif

	set_dataready_gpio();

	return tx_buf_handle.payload_len;
}

static void IRAM_ATTR esp_spi_read_done(void *handle)
{
	spi_buffer_rx_free(handle);
}

static int esp_spi_read(interface_handle_t *if_handle, interface_buffer_handle_t *buf_handle)
{
	if (!if_handle) {
		ESP_LOGE(TAG, "Invalid arguments to esp_spi_read\n");
		return ESP_FAIL;
	}

#ifdef CONFIG_ESP_ENABLE_RX_PRIORITY_QUEUES
	xSemaphoreTake(spi_rx_sem, portMAX_DELAY);

	if (pdFALSE == xQueueReceive(spi_rx_queue[PRIO_Q_SERIAL], buf_handle, 0))
		if (pdFALSE == xQueueReceive(spi_rx_queue[PRIO_Q_BT], buf_handle, 0))
			if (pdFALSE == xQueueReceive(spi_rx_queue[PRIO_Q_OTHERS], buf_handle, 0)) {
				ESP_LOGI(TAG, "%s No element in rx queue", __func__);
		return ESP_FAIL;
	}
#else
	xQueueReceive(spi_rx_queue, buf_handle, portMAX_DELAY);
#endif

	return buf_handle->payload_len;
}

static esp_err_t esp_spi_reset(interface_handle_t *handle)
{
	esp_err_t ret = ESP_OK;
	ret = spi_slave_free(ESP_SPI_CONTROLLER);
	if (ESP_OK != ret) {
		ESP_LOGE(TAG, "spi slave bus free failed\n");
	}
	return ret;
}

static void esp_spi_deinit(interface_handle_t *handle)
{
	esp_err_t ret = ESP_OK;

	spi_mempool_destroy();

	ret = spi_slave_free(ESP_SPI_CONTROLLER);
	if (ESP_OK != ret) {
		ESP_LOGE(TAG, "spi slave bus free failed\n");
		return;
	}

	ret = spi_bus_free(ESP_SPI_CONTROLLER);
	if (ESP_OK != ret) {
		ESP_LOGE(TAG, "spi all bus free failed\n");
		return;
	}
}
