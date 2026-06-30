/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "sys/queue.h"
#include "soc/soc.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <unistd.h>
#include <inttypes.h>
#ifndef CONFIG_IDF_TARGET_ARCH_RISCV
#include "xtensa/core-macros.h"
#endif
#include "esp_private/wifi.h"
#include "interface.h"
#include "esp_wpa.h"
#include "esp_hosted_coprocessor.h"
#include "driver/gpio.h"

#include "freertos/task.h"
#include "freertos/queue.h"

// enable only if BT component enabled and soc supports BT
#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_SOC_BT_SUPPORTED)
#include "esp_bt.h"
#ifdef CONFIG_BT_HCI_UART_NO
#include "driver/uart.h"
#endif
#endif

#include "endian.h"

#include <protocomm.h>
#include "protocomm_pserial.h"
#include "slave_control.h"
#include "slave_bt.h"
#include "stats.h"
#include "esp_fw_version.h"
#include "esp_hosted_cli.h"
#ifdef ESP_HOSTED_COPROCESSOR_EXAMPLE_HTTP_CLIENT
#include "example_http_client.h"
#endif
#include "esp_wifi.h"

#if CONFIG_NETWORK_SPLIT_ENABLED
	#include "host_power_save.h"
	#include "esp_hosted_config.pb-c.h"


	volatile uint8_t station_got_ip = 0;

	#include "wifi_cmd.h"

	/* Perform DHCP at slave & send IP info at host */
	#define H_SLAVE_LWIP_DHCP_AT_SLAVE       1

#endif
#include "nw_split_router.h"

#ifdef CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_PEER_DATA
#include "peer_data_example.h"
#endif

static const char TAG[] = "fg_slave";


#define UNKNOWN_CTRL_MSG_ID              0

#define TO_HOST_QUEUE_SIZE               10

#define ETH_DATA_LEN                     1500
#define MAX_WIFI_STA_TX_RETRY            8
#define WIFI_TX_RETRY_DELAY_MS           1



volatile uint8_t datapath = 0;
volatile uint8_t station_connected = 0;
volatile uint8_t softap_started = 0;

interface_context_t *if_context = NULL;
interface_handle_t *if_handle = NULL;

esp_netif_t *slave_sta_netif = NULL;

static protocomm_t *pc_pserial;
SemaphoreHandle_t host_reset_sem;

static struct rx_data {
	uint8_t valid;
	uint16_t cur_seq_no;
	int len;
	uint8_t *data;
} r;

/* Add at the top with other static variables */
#if H_HOST_PS_ALLOWED
#define MAX_DHCP_DNS_RETRIES 10
static int dhcp_dns_retry_count = 0;
static TimerHandle_t delayed_dhcp_dns_timer = NULL;
#endif

static void print_firmware_version()
{
	ESP_LOGI(TAG, "*********************************************************************");
	ESP_LOGI(TAG, "                ESP-Hosted Firmware version :: %s-%d.%d.%d.%d.%d",
			PROJECT_NAME, PROJECT_VERSION_MAJOR_1, PROJECT_VERSION_MAJOR_2, PROJECT_VERSION_MINOR, PROJECT_REVISION_PATCH_1, PROJECT_REVISION_PATCH_2);
#if CONFIG_ESP_SPI_HOST_INTERFACE
  #if BLUETOOTH_UART
	ESP_LOGI(TAG, "                Transport used :: SPI + UART                    ");
  #else
	ESP_LOGI(TAG, "                Transport used :: SPI only                      ");
  #endif
#else
  #if BLUETOOTH_UART
	ESP_LOGI(TAG, "                Transport used :: SDIO + UART                   ");
  #else
	ESP_LOGI(TAG, "                Transport used :: SDIO only                     ");
  #endif
#endif
	ESP_LOGI(TAG, "*********************************************************************");
}

static uint8_t get_capabilities(void)
{
	uint8_t cap = 0;

	ESP_LOGI(TAG, "Supported features are:");
#if CONFIG_ESP_SPI_HOST_INTERFACE
	ESP_LOGI(TAG, "- WLAN over SPI");
	cap |= ESP_WLAN_SPI_SUPPORT;
#else
	ESP_LOGI(TAG, "- WLAN over SDIO");
	cap |= ESP_WLAN_SDIO_SUPPORT;
#endif

#if CONFIG_ESP_SPI_CHECKSUM || CONFIG_ESP_SDIO_CHECKSUM
	cap |= ESP_CHECKSUM_ENABLED;
#endif

#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_SOC_BT_SUPPORTED)
	cap |= get_bluetooth_capabilities();
#endif
	ESP_LOGI(TAG, "capabilities: 0x%x", cap);

	return cap;
}

static inline esp_err_t populate_buff_handle(interface_buffer_handle_t *buf_handle,
		uint8_t if_type,
		uint8_t *buf,
		uint16_t len,
		void (*free_buf_func)(void *data),
		void *free_buf_handle,
		uint8_t flag,
		uint8_t if_num,
		uint16_t seq_num)
{
	buf_handle->if_type = if_type;
	buf_handle->payload = buf;
	buf_handle->payload_len = len;
	buf_handle->priv_buffer_handle = free_buf_handle;
	buf_handle->free_buf_handle = free_buf_func;
	buf_handle->flag = flag;
	buf_handle->if_num = if_num;
	buf_handle->seq_num = seq_num;

	return ESP_OK;
}

#define populate_wifi_buffer_handle(Buf_hdL, TypE, BuF, LeN) \
	populate_buff_handle(Buf_hdL, TypE, BuF, LeN, esp_wifi_internal_free_rx_buffer, eb, 0, 0, 0);


esp_err_t wlan_ap_rx_callback(void *buffer, uint16_t len, void *eb)
{
	interface_buffer_handle_t buf_handle = {0};

	if (!buffer || !eb || !datapath) {
		if (eb) {
			esp_wifi_internal_free_rx_buffer(eb);
		}
		return ESP_OK;
	}
	ESP_HEXLOGV("AP_Get", buffer, len, 32);

	populate_wifi_buffer_handle(&buf_handle, ESP_AP_IF, buffer, len);

	if (send_to_host_queue(&buf_handle, PRIO_Q_OTHERS))
		goto DONE;

	return ESP_OK;

DONE:
	esp_wifi_internal_free_rx_buffer(eb);
	return ESP_OK;
}

/* This function would check the incoming packet from AP
 * to send it to local lwip or host lwip depending upon the
 * destination port used in the packet
 */
esp_err_t wlan_sta_rx_callback(void *buffer, uint16_t len, void *eb)
{
	interface_buffer_handle_t buf_handle = {0};
	hosted_l2_bridge bridge_to_use = HOST_LWIP_BRIDGE;

	if (!buffer || !eb) {
		if (eb) {
			ESP_LOGD(TAG, "drop wifi packet. datapath: %u", datapath);
			esp_wifi_internal_free_rx_buffer(eb);
		}
		return ESP_OK;
	}

	ESP_HEXLOGV("STA_Get", buffer, len, 64);

#if ESP_PKT_STATS
	pkt_stats.sta_lwip_in++;
#endif

#ifdef CONFIG_NETWORK_SPLIT_ENABLED
	/* Filter and route the packet based on destination port */
	bridge_to_use = filter_and_route_packet(buffer, len);
#else
	/* default as co-processor mode */
	bridge_to_use = HOST_LWIP_BRIDGE;
#endif

	switch (bridge_to_use) {
		case HOST_LWIP_BRIDGE:
			if (!datapath) {
				ESP_LOGV(TAG, "datapath closed, drop packet");
				goto DONE;
			}
			/* Send to Host */
			ESP_LOGV(TAG, "host packet");
			populate_wifi_buffer_handle(&buf_handle, ESP_STA_IF, buffer, len);

			if (unlikely(send_to_host_queue(&buf_handle, PRIO_Q_OTHERS)))
				goto DONE;

#if ESP_PKT_STATS
			pkt_stats.sta_sh_in++;
			pkt_stats.sta_host_lwip_out++;
#endif
			break;

		case SLAVE_LWIP_BRIDGE:
			/* Send to local LWIP */
			ESP_LOGV(TAG, "slave packet");
			if (!slave_sta_netif) {
				ESP_LOGW(TAG, "slave_sta_netif not init, drop slave packet");
				goto DONE;
			}
			esp_netif_receive(slave_sta_netif, buffer, len, eb);
#if ESP_PKT_STATS
			pkt_stats.sta_slave_lwip_out++;
#endif
			break;

		case BOTH_LWIP_BRIDGE:
			ESP_LOGV(TAG, "slave & host packet");

			/* Allocate host copy only when datapath is open. */
			void *copy_buff = NULL;
			if (datapath) {
				copy_buff = malloc(len);
				if (copy_buff)
					memcpy(copy_buff, buffer, len);
				else
					ESP_LOGW(TAG, "no mem for host copy, slave-only this packet");
			}

			/* slave netif takes ownership of eb */
			if (!slave_sta_netif) {
				ESP_LOGW(TAG, "slave_sta_netif not init, drop slave part");
				esp_wifi_internal_free_rx_buffer(eb);
			} else {
				esp_netif_receive(slave_sta_netif, buffer, len, eb);
			}

			if (copy_buff) {
				populate_buff_handle(&buf_handle, ESP_STA_IF, copy_buff, len, free, copy_buff, 0, 0, 0);
				if (unlikely(send_to_host_queue(&buf_handle, PRIO_Q_OTHERS))) {
					free(copy_buff);
					return ESP_OK;
				}

			#if ESP_PKT_STATS
				pkt_stats.sta_sh_in++;
				pkt_stats.sta_both_lwip_out++;
			#endif
			} else {
			#if ESP_PKT_STATS
				pkt_stats.sta_slave_lwip_out++;
			#endif
			}

			break;

		default:
			ESP_LOGV(TAG, "Packet filtering failed, drop packet");
			goto DONE;
	}

	return ESP_OK;

DONE:
	esp_wifi_internal_free_rx_buffer(eb);
	return ESP_OK;
}

/* TX ownership now lives in transport write(). */

static void parse_protobuf_req(void)
{
	protocomm_pserial_data_ready(pc_pserial, r.data,
		r.len, UNKNOWN_CTRL_MSG_ID);
}

esp_err_t send_event_to_host(int event_id)
{
	return protocomm_pserial_data_ready(pc_pserial, NULL, 0, event_id);
}

esp_err_t send_event_data_to_host(int event_id, void *data, int size)
{
	return protocomm_pserial_data_ready(pc_pserial, data, size, event_id);
}

static void process_serial_rx_pkt(uint8_t *buf)
{
	struct esp_payload_header *header = NULL;
	uint16_t payload_len = 0;
	uint8_t *payload = NULL;
	uint8_t *new_data = NULL;

	header = (struct esp_payload_header *) buf;
	payload_len = le16toh(header->len);
	payload = buf + le16toh(header->offset);

	ESP_HEXLOGV("serial_rx", payload, payload_len, 32);

	while (r.valid)
	{
		ESP_LOGI(TAG,"More segment: %u curr seq: %u header seq: %u\n",
			header->flags & MORE_FRAGMENT, r.cur_seq_no, header->seq_num);
		vTaskDelay(10);
	}

	if (!r.len) {
		/* New Buffer */
		r.cur_seq_no = le16toh(header->seq_num);
	}

	if (header->seq_num != r.cur_seq_no) {
		/* Sequence number mismatch */
		r.valid = 1;
		ESP_LOGV(TAG, "Final Frag: r.valid=1");
		parse_protobuf_req();
		return;
	}

	new_data = realloc(r.data, r.len + payload_len);
	if (!new_data) {
		ESP_LOGE(TAG, "serial rx realloc failed (need %d bytes), dropping",
			r.len + payload_len);
		free(r.data);
		r.data = NULL;
		r.len = 0;
		r.cur_seq_no = 0;
		return;
	}
	r.data = new_data;
	memcpy(r.data + r.len, payload, payload_len);
	r.len += payload_len;

	if (!(header->flags & MORE_FRAGMENT)) {
		/* Received complete buffer */
		r.valid = 1;
		ESP_LOGV(TAG, "no frag case: r.valid=1");
		parse_protobuf_req();
	}
}



static void process_priv_pkt(uint8_t *payload, uint16_t payload_len)
{
	struct esp_priv_event *event;

	if (!payload || !payload_len)
		return;

	event = (struct esp_priv_event *) payload;

	if (event->event_type == ESP_PRIV_EVENT_INIT) {
		ESP_HEXLOGD("init_config", event->event_data, event->event_len, 32);
	} else {
		ESP_LOGW(TAG, "Drop unknown event\n\r");
	}
}

/* Host->slave private command. */
static void process_priv_command(uint8_t *payload, uint16_t payload_len)
{
	if (!payload || !payload_len)
		return;

#if TEST_RAW_TP
	process_raw_tp_cmd(payload[0]);
#else
	ESP_LOGW(TAG, "Priv command %u ignored (raw-tp not built in)", payload[0]);
#endif
}

/* Retry transient WiFi TX buffer exhaustion. */
static int wifi_tx_with_retry(wifi_interface_t wifi_if, uint8_t *payload,
		uint16_t payload_len)
{
	int ret = ESP_OK;
	int retry = MAX_WIFI_STA_TX_RETRY;

	do {
		ret = esp_wifi_internal_tx(wifi_if, payload, payload_len);
		if (ret != ESP_ERR_NO_MEM)
			break;
#if ESP_PKT_STATS
		pkt_stats.wifi_tx_retries++;
#endif
		vTaskDelay(pdMS_TO_TICKS(WIFI_TX_RETRY_DELAY_MS));
	} while (--retry);

	return ret;
}

static void process_rx_pkt(interface_buffer_handle_t *buf_handle)
{

	struct esp_payload_header *header = NULL;
	uint8_t *payload = NULL;
	uint16_t payload_len = 0;
	int ret = 0;

	header = (struct esp_payload_header *) buf_handle->payload;
	payload = buf_handle->payload + le16toh(header->offset);
	payload_len = le16toh(header->len);

	ESP_HEXLOGV("bus_RX", payload, payload_len, 16);


	if (buf_handle->if_type == ESP_STA_IF && station_connected) {
		/* Forward data to wlan driver */
		ret = wifi_tx_with_retry(WIFI_IF_STA, payload, payload_len);

#if ESP_PKT_STATS
		if (ret)
			pkt_stats.hs_bus_sta_fail++;
		else
			pkt_stats.hs_bus_sta_out++;
#endif
	} else if (buf_handle->if_type == ESP_AP_IF && softap_started) {
		/* Forward data to wlan driver */
		ret = wifi_tx_with_retry(WIFI_IF_AP, payload, payload_len);
#if ESP_PKT_STATS
		if (ret)
			pkt_stats.hs_bus_ap_fail++;
		else
			pkt_stats.hs_bus_ap_out++;
#endif
		ESP_HEXLOGV("AP_Put", payload, payload_len, 32);
	} else if (buf_handle->if_type == ESP_SERIAL_IF) {
#if ESP_PKT_STATS
		pkt_stats.serial_rx++;
#endif
		process_serial_rx_pkt(buf_handle->payload);
	} else if (buf_handle->if_type == ESP_PRIV_IF) {
		if (header->priv_pkt_type == ESP_PACKET_TYPE_COMMAND)
			process_priv_command(payload, payload_len);
		else
			process_priv_pkt(payload, payload_len);
	}
#if defined(CONFIG_BT_ENABLED) && BLUETOOTH_HCI
	else if (buf_handle->if_type == ESP_HCI_IF) {
		process_hci_rx_pkt(payload, payload_len);
	}
#endif
#if TEST_RAW_TP
	else if (buf_handle->if_type == ESP_TEST_IF) {
		debug_update_raw_tp_rx_count(payload_len);
	}
#endif

	/* Free buffer handle */
	if (buf_handle->free_buf_handle && buf_handle->priv_buffer_handle) {
		buf_handle->free_buf_handle(buf_handle->priv_buffer_handle);
		buf_handle->priv_buffer_handle = NULL;
	}

}

/* Get data from host */
static void recv_task(void* pvParameters)
{
	interface_buffer_handle_t buf_handle = {0};

	for (;;) {

		if (!datapath) {
			/* Datapath is not enabled by host yet*/
			vTaskDelay(pdMS_TO_TICKS(1));
			continue;
		}

		/* receive data from transport layer */
		if (if_context && if_context->if_ops && if_context->if_ops->read) {
			int len = if_context->if_ops->read(if_handle, &buf_handle);
			if (len <= 0) {
				vTaskDelay(2);
				continue;
			}
		}

		process_rx_pkt(&buf_handle);
	}
}

static ssize_t serial_read_data(uint8_t *data, ssize_t len)
{
	len = min(len, r.len);
	if (r.valid) {
		memcpy(data, r.data, len);
		free(r.data);
		r.data = NULL;
		r.valid = 0;
		r.len = 0;
		r.cur_seq_no = 0;
	} else {
		ESP_LOGI(TAG,"No data to be read, len %d", len);
	}
	return len;
}

int send_to_host_queue(interface_buffer_handle_t *buf_handle, uint8_t queue_type)
{
	/* write() takes ownership of buf_handle and frees it, so return OK once it
	 * is reached (caller must not double-free); FAIL only when no transport.
	 * queue_type is unused - the driver derives the lane from if_type. */
	if (!if_context || !if_context->if_ops || !if_context->if_ops->write)
		return ESP_FAIL;
	if_context->if_ops->write(if_handle, buf_handle);
	return ESP_OK;
}

static esp_err_t serial_write_data(uint8_t* data, ssize_t len)
{
	uint8_t *pos = data;
	int32_t left_len = len;
	int32_t frag_len = 0;
	static uint16_t seq_num = 0;

	do {
		interface_buffer_handle_t buf_handle = {0};

		seq_num++;

		buf_handle.if_type = ESP_SERIAL_IF;
		buf_handle.if_num = 0;
		buf_handle.seq_num = seq_num;

		if (left_len > ETH_DATA_LEN) {
			frag_len = ETH_DATA_LEN;
			buf_handle.flag = MORE_FRAGMENT;
		} else {
			frag_len = left_len;
			buf_handle.flag = 0;
			buf_handle.priv_buffer_handle = data;
			buf_handle.free_buf_handle = free;
		}

		buf_handle.payload = pos;
		buf_handle.payload_len = frag_len;

		if (send_to_host_queue(&buf_handle, PRIO_Q_SERIAL)) {
			if (data) {
				free(data);
				data = NULL;
			}
			return ESP_FAIL;
		}

		ESP_HEXLOGV("serial_tx_create", data, frag_len, 32);

		left_len -= frag_len;
		pos += frag_len;
	} while(left_len);

	return ESP_OK;
}

int event_handler(uint8_t val)
{
	switch(val) {
		case ESP_OPEN_DATA_PATH:
			if (if_handle) {
				if_handle->state = ACTIVE;
				datapath = 1;
				ESP_EARLY_LOGI(TAG, "Start Data Path");
				if (host_reset_sem) {
					xSemaphoreGive(host_reset_sem);
				}
			} else {
				ESP_EARLY_LOGI(TAG, "Failed to Start Data Path");
			}
			break;

		case ESP_CLOSE_DATA_PATH:
			datapath = 0;
			if (if_handle) {
				ESP_EARLY_LOGI(TAG, "Stop Data Path");
				if_handle->state = DEACTIVE;
			} else {
				ESP_EARLY_LOGI(TAG, "Failed to Stop Data Path");
			}
			break;

		case ESP_POWER_SAVE_ON:
			host_power_save_alert(ESP_POWER_SAVE_ON);
			if_handle->state = ACTIVE;
			break;

		case ESP_POWER_SAVE_OFF:
			if_handle->state = ACTIVE;
			if (host_reset_sem) {
				xSemaphoreGive(host_reset_sem);
			}
			host_power_save_alert(ESP_POWER_SAVE_OFF);
			break;
	}
	return 0;
}

#if defined(CONFIG_ESP_GPIO_SLAVE_RESET) && (CONFIG_ESP_GPIO_SLAVE_RESET != -1)
static void IRAM_ATTR gpio_resetpin_isr_handler(void* arg)
{

	ESP_EARLY_LOGI(TAG, "*********");
	if (CONFIG_ESP_GPIO_SLAVE_RESET == -1) {
		ESP_EARLY_LOGI(TAG, "%s: using EN pin for slave reset", __func__);
		return;
	}

	static uint32_t lasthandshaketime_us;
	uint32_t currtime_us = esp_timer_get_time();

	if (gpio_get_level(CONFIG_ESP_GPIO_SLAVE_RESET) == 0) {
		lasthandshaketime_us = currtime_us;
	} else {
		uint32_t diff = currtime_us - lasthandshaketime_us;
		ESP_EARLY_LOGI(TAG, "%s Diff: %u", __func__, diff);
		if (diff < 500) {
			return; //ignore everything < half ms after an earlier irq
		} else {
			ESP_EARLY_LOGI(TAG, "Host triggered slave reset");
			esp_restart();
		}
	}
}

static void register_reset_pin(uint32_t gpio_num)
{
	if (gpio_num != -1) {
		ESP_LOGI(TAG, "Using GPIO [%lu] as slave reset pin", gpio_num);
		gpio_reset_pin(gpio_num);

		gpio_config_t slave_reset_pin_conf={
			.intr_type=GPIO_INTR_DISABLE,
			.mode=GPIO_MODE_INPUT,
			.pull_up_en=1,
			.pin_bit_mask=(1<<gpio_num)
		};

		gpio_config(&slave_reset_pin_conf);
		gpio_set_intr_type(gpio_num, GPIO_INTR_ANYEDGE);
		gpio_install_isr_service(0);
		gpio_isr_handler_add(gpio_num, gpio_resetpin_isr_handler, NULL);
	}
}
#endif
#ifdef CONFIG_NETWORK_SPLIT_ENABLED
void create_slave_sta_netif(uint8_t dhcp_at_slave)
{
	/* Create "almost" default station, but with un-flagged DHCP client */
	esp_netif_inherent_config_t netif_cfg;
	memcpy(&netif_cfg, ESP_NETIF_BASE_DEFAULT_WIFI_STA, sizeof(netif_cfg));

	if (!dhcp_at_slave)
		netif_cfg.flags &= ~ESP_NETIF_DHCP_CLIENT;

	esp_netif_config_t cfg_sta = {
		.base = &netif_cfg,
		.stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA,
	};
	esp_netif_t *netif_sta = esp_netif_new(&cfg_sta);
	assert(netif_sta);

	ESP_ERROR_CHECK(esp_netif_attach_wifi_station(netif_sta));
	ESP_ERROR_CHECK(esp_wifi_set_default_wifi_sta_handlers());

	if (!dhcp_at_slave) {
		ESP_ERROR_CHECK(esp_netif_dhcpc_stop(netif_sta));
		ESP_LOGI(TAG, "No DHCP at slave");
	} else {
		ESP_LOGI(TAG, "DHCP at slave");
		ESP_ERROR_CHECK(esp_netif_dhcpc_start(netif_sta));
	}

	slave_sta_netif = netif_sta;
}
#endif

/* Inits WiFi + STA mode + start at boot in all modes (so host RPCs don't hit
 * WIFI_NOT_INIT); the fallback auto-connect inside is gated to network-split. */

#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_WPA3_SAE_PWE_HUNT_AND_PECK
  #define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
  #define EXAMPLE_H2E_IDENTIFIER ""
#elif CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT
  #define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
  #define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_WPA3_SAE_PWE_BOTH
  #define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
  #define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif

#if CONFIG_ESP_WIFI_AUTH_OPEN
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

static int __attribute__((unused)) fallback_to_sdkconfig_wifi_config(void)
{
	wifi_config_t wifi_config = {
		.sta = {
			.ssid = EXAMPLE_ESP_WIFI_SSID,
			.password = EXAMPLE_ESP_WIFI_PASS,
			/* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (password len => 8).
			 * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
			 * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
			 * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
			 */
			.threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
			.sae_pwe_h2e = ESP_WIFI_SAE_MODE,
			.sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
			.scan_method = WIFI_ALL_CHANNEL_SCAN,
			.sort_method = WIFI_CONNECT_AP_BY_SIGNAL,

		},
	};

	ESP_ERROR_CHECK(esp_hosted_set_sta_config(WIFI_IF_STA, &wifi_config) );

	return ESP_OK;
}

static bool __attribute__((unused)) wifi_is_provisioned(void)
{
	wifi_config_t wifi_cfg = {0};

	if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK) {
		ESP_LOGI(TAG, "Wifi get config failed");
		return false;
	}

	ESP_LOGI(TAG, "SSID: %s", wifi_cfg.sta.ssid);

	if (strlen((const char *) wifi_cfg.sta.ssid)) {
		ESP_LOGI(TAG, "Wifi provisioned");
		return true;
	}
	ESP_LOGI(TAG, "Wifi not provisioned, Fallback to example config");

	return false;
}

static int connect_sta(void)
{
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

	ESP_ERROR_CHECK(esp_hosted_wifi_init(&cfg));

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );

#if CONFIG_WIFI_CMD_DEFAULT_COUNTRY_CN
	/* Only set country once during first initialize wifi */
	static bool country_code_has_set = false;
	if (country_code_has_set == false) {
		wifi_country_t country = {
			.cc = "CN",
			.schan = 1,
			.nchan = 13,
			.policy = 0
		};
		esp_wifi_set_country(&country);
		country_code_has_set = true;
	}
#endif

#ifdef CONFIG_NETWORK_SPLIT_ENABLED
	if (! wifi_is_provisioned()) {
		fallback_to_sdkconfig_wifi_config();
	}
#endif

	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

	ESP_ERROR_CHECK(esp_wifi_start() );

#if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
#if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_MU_STATS
	esp_wifi_enable_rx_statistics(true, true);
#else
	esp_wifi_enable_rx_statistics(true, false);
#endif
#endif

#if CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS
	esp_wifi_enable_tx_statistics(ESP_WIFI_ACI_BE, true);
#endif

	return ESP_OK;
}

#if H_HOST_PS_ALLOWED

/* Update timer callback function */
static void delayed_dhcp_dns_timer_cb(TimerHandle_t xTimer)
{
	/* Check if host has fetched IP or max retries reached */
	if (has_host_fetched_auto_ip() || dhcp_dns_retry_count >= MAX_DHCP_DNS_RETRIES) {
		/* Stop retrying */
		if (delayed_dhcp_dns_timer) {
			xTimerDelete(delayed_dhcp_dns_timer, 0);
			delayed_dhcp_dns_timer = NULL;
		}
		dhcp_dns_retry_count = 0;
		return;
	}

	send_dhcp_dns_info_to_host(0, 0);
	dhcp_dns_retry_count++;

}

/* Function to schedule delayed DHCP/DNS info send */
static void schedule_delayed_dhcp_dns_info(void)
{
	const TickType_t delay_ticks = pdMS_TO_TICKS(500); /* 500ms delay */

	/* Delete existing timer if any */
	if (delayed_dhcp_dns_timer) {
		xTimerDelete(delayed_dhcp_dns_timer, 0);
		delayed_dhcp_dns_timer = NULL;
	}

	/* Create and start one-shot timer */
	delayed_dhcp_dns_timer = xTimerCreate("DhcpDns",
			delay_ticks,
			pdFALSE,  /* One-shot timer */
			0,
			delayed_dhcp_dns_timer_cb);

	if (delayed_dhcp_dns_timer) {
		if (xTimerStart(delayed_dhcp_dns_timer, 0) != pdPASS) {
			ESP_LOGE(TAG, "Failed to start delayed DHCP/DNS timer");
			xTimerDelete(delayed_dhcp_dns_timer, 0);
			delayed_dhcp_dns_timer = NULL;
		}
	} else {
		ESP_LOGE(TAG, "Failed to create delayed DHCP/DNS timer");
	}
}
#endif
/* Update host wakeup callback */
void host_wakeup_callback(void)
{
	/* Handle immediate wakeup tasks */
#if H_HOST_PS_ALLOWED
	/* Reset retry count on new wakeup */
	dhcp_dns_retry_count = 0;

	/* Schedule delayed DHCP/DNS info send */
	if (station_connected) {
		schedule_delayed_dhcp_dns_info();
	}
#endif
}

static void host_reset_task(void* pvParameters)
{
	uint8_t capa = 0;

	ESP_LOGI(TAG, "host reset handler task started");

	while (1) {

		if (host_reset_sem) {
			xSemaphoreTake(host_reset_sem, portMAX_DELAY);
		} else {
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}

		capa = get_capabilities();
		/* send capabilities to host */
		ESP_LOGI(TAG,"Send slave up event");
		generate_startup_event(capa);
		send_event_to_host(CTRL_MSG_ID__Event_ESPInit);

#ifdef CONFIG_NETWORK_SPLIT_ENABLED
		ESP_LOGI(TAG,"--- Wait for IP ---");
		while (!station_got_ip) {
			vTaskDelay(pdMS_TO_TICKS(50));
		}
		send_dhcp_dns_info_to_host(1, 1);
#endif
	}
}


esp_err_t esp_hosted_coprocessor_init(void)
{
	assert(host_reset_sem = xSemaphoreCreateBinary());

	print_firmware_version();

#if CONFIG_NETWORK_SPLIT_ENABLED
	ESP_ERROR_CHECK(esp_netif_init());
#endif
	ESP_ERROR_CHECK(esp_event_loop_create_default());

#if defined(CONFIG_ESP_GPIO_SLAVE_RESET) && (CONFIG_ESP_GPIO_SLAVE_RESET != -1)
	register_reset_pin(CONFIG_ESP_GPIO_SLAVE_RESET);
#endif


#ifdef CONFIG_ESP_HOSTED_HOST_RESERVED_PORTS_CONFIGURED
	ESP_LOGI(TAG, "Configuring host static port forwarding rules from slave kconfig");
	configure_host_static_port_forwarding_rules(CONFIG_ESP_HOSTED_HOST_RESERVED_TCP_SRC_PORTS,
												CONFIG_ESP_HOSTED_HOST_RESERVED_TCP_DEST_PORTS,
												CONFIG_ESP_HOSTED_HOST_RESERVED_UDP_SRC_PORTS,
												CONFIG_ESP_HOSTED_HOST_RESERVED_UDP_DEST_PORTS);
#endif


	host_power_save_init(host_wakeup_callback);

#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_SOC_BT_SUPPORTED)
	initialise_bluetooth();
#endif

	pc_pserial = protocomm_new();
	if (pc_pserial == NULL) {
		ESP_LOGE(TAG,"Failed to allocate memory for new instance of protocomm ");
		return ESP_FAIL;
	}

	/* Endpoint for control command responses */
	if (protocomm_add_endpoint(pc_pserial, CTRL_EP_NAME_RESP,
				data_transfer_handler, NULL) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to add endpoint");
		return ESP_FAIL;
	}

	/* Endpoint for control notifications for events subscribed by user */
	if (protocomm_add_endpoint(pc_pserial, CTRL_EP_NAME_EVENT,
				ctrl_notify_handler, NULL) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to add endpoint");
		return ESP_FAIL;
	}

	protocomm_pserial_start(pc_pserial, serial_write_data, serial_read_data);

	if_context = interface_insert_driver(event_handler);

#if CONFIG_ESP_SPI_HOST_INTERFACE
	datapath = 1;
	if (host_reset_sem)
		xSemaphoreGive(host_reset_sem);
#endif

	if (!if_context || !if_context->if_ops) {
		ESP_LOGE(TAG, "Failed to insert driver\n");
		return ESP_FAIL;
	}

	if_handle = if_context->if_ops->init();

	if (!if_handle) {
		ESP_LOGE(TAG, "Failed to initialize driver\n");
		return ESP_FAIL;
	}


	assert(xTaskCreate(recv_task , "recv_task" ,
			CONFIG_ESP_DEFAULT_TASK_STACK_SIZE, NULL ,
			CONFIG_ESP_HOSTED_TASK_PRIORITY_DEFAULT, NULL) == pdTRUE);
	create_debugging_tasks();

#ifdef H_ESP_HOSTED_CLI_ENABLED
	esp_hosted_cli_start();
#endif

#ifdef CONFIG_NETWORK_SPLIT_ENABLED

	create_slave_sta_netif(H_SLAVE_LWIP_DHCP_AT_SLAVE);

	ESP_LOGI(TAG, "Default LWIP post filtering packets to send: %s",
#if defined(CONFIG_ESP_DEFAULT_LWIP_SLAVE)
			"slave. Host need to use **static netif** only"
#elif defined(CONFIG_ESP_DEFAULT_LWIP_HOST)
			"host"
#elif defined(CONFIG_ESP_DEFAULT_LWIP_BOTH)
			"host+slave"
#endif
			);
#endif

	connect_sta();

	ESP_LOGI(TAG, "bus tx locked on slave boot-up");

	while(!datapath) {
		vTaskDelay(10);
	}
	ESP_LOGI(TAG, "bus tx unlocked");

	assert(xTaskCreate(host_reset_task, "host_reset_task" ,
			CONFIG_ESP_DEFAULT_TASK_STACK_SIZE, NULL ,
			CONFIG_ESP_HOSTED_TASK_PRIORITY_DEFAULT, NULL) == pdTRUE);


  #ifdef CONFIG_NETWORK_SPLIT_ENABLED
	while (!station_got_ip)
		sleep(1);
  #endif

	/* Register how you are going to handle the user defined RPC requests */
#ifdef CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_PEER_DATA
	peer_data_example_register();
#endif

	return ESP_OK;
}

void app_main(void)
{
	/* Initialize NVS */
	esp_err_t ret = nvs_flash_init();

	if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
	    ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK( ret );

	esp_hosted_coprocessor_init();
#ifdef CONFIG_NETWORK_SPLIT_ENABLED

#ifdef ESP_HOSTED_COPROCESSOR_EXAMPLE_HTTP_CLIENT
	example_http_client_start();
#endif

#endif

}
