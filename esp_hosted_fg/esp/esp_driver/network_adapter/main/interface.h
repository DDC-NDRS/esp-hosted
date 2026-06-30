// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD

#ifndef __TRANSPORT_LAYER_INTERFACE_H
#define __TRANSPORT_LAYER_INTERFACE_H
#include "esp_err.h"
#include "esp_hosted_log.h"

#ifdef CONFIG_ESP_SDIO_HOST_INTERFACE

#if CONFIG_SOC_SDIO_SLAVE_SUPPORTED
	#include "driver/sdio_slave.h"
#else
	#error "SDIO is not supported for this chipset"
#endif

#endif

typedef enum {
	LENGTH_1_BYTE  = 1,
	LENGTH_2_BYTE  = 2,
	LENGTH_3_BYTE  = 3,
	LENGTH_4_BYTE  = 4,
} byte_length;

typedef void *wlan_buf_handle_t;

typedef enum {
	SDIO = 0,
	SPI = 1,
} transport_layer;

typedef enum {
	DEINIT,
	INIT,
	ACTIVE,
	DEACTIVE,
} INTERFACE_STATE;

typedef struct {
	union {
#ifdef CONFIG_ESP_SDIO_HOST_INTERFACE
		sdio_slave_buf_handle_t sdio_buf_handle;
#endif
		wlan_buf_handle_t	wlan_buf_handle;
		void *priv_buffer_handle;
	};
	uint8_t if_type;
	uint8_t if_num;
	uint8_t *payload;
	uint8_t flag;
	uint16_t payload_len;
	uint16_t seq_num;

	void (*free_buf_handle)(void *buf_handle);
} interface_buffer_handle_t;

typedef struct {
	/*
	union {
	} phy_context;
	*/
	INTERFACE_STATE state;
}interface_handle_t;

#if CONFIG_ESP_SPI_HOST_INTERFACE
#define MAX_TRANSPORT_BUF_SIZE 1600
#elif CONFIG_ESP_SDIO_HOST_INTERFACE
/* MAX_TRANSPORT_BUF_SIZE — SDIO recv-buffer size and flow-control credit unit.
 *
 * 15872 is only a fallback. The real size is found at runtime: the slave
 * (sdio_init -> sdio_hw_max_rx_buf_size) reads this chip's DMA-descriptor limit,
 * rounds down to a 512B multiple, allocates that, and tells the host in the boot
 * TLV (ESP_PRIV_RX_BUF_CONFIG: e2h_bufsz_512B / h2e_bufsz_512B). The host uses
 * the advertised size and falls back to its own 15872 only for an old (pre-TLV)
 * slave. So this constant and the host's ESP_RX_BUFFER_SIZE need to match only
 * on that fallback path.
 *
 * How 15872 is derived (the value the auto-detect returns on 14-bit chips):
 *   14-bit descriptor length -> max 16383 B; CMD53 is 512B-aligned ->
 *   floor(16383/512) = 31 blocks -> 31*512 = 15872 (16384 would overflow).
 * Per chip (auto-detected, no manual table):
 *   C5/C6/C61 and other 14-bit chips: 15872
 *   ESP32 classic (12-bit): floor(4095/512)*512 = 3584
 *
 * SPI uses the small per-transfer buffer above (no aggregation/credit). */
#define MAX_TRANSPORT_BUF_SIZE 15872
#endif

#define BSSID_BYTES_SIZE       6

typedef struct {
	interface_handle_t * (*init)(void);
	int32_t (*write)(interface_handle_t *handle, interface_buffer_handle_t *buf_handle);
	int (*read)(interface_handle_t *handle, interface_buffer_handle_t *buf_handle);
	esp_err_t (*reset)(interface_handle_t *handle);
	void (*deinit)(interface_handle_t *handle);
} if_ops_t;

typedef struct {
	transport_layer type;
	void *priv;
	if_ops_t *if_ops;
	int (*event_handler)(uint8_t bitmap);
} interface_context_t;

interface_context_t * interface_insert_driver(int (*callback)(uint8_t val));
int interface_remove_driver();
void generate_startup_event(uint8_t cap);
int send_to_host_queue(interface_buffer_handle_t *buf_handle, uint8_t queue_type);

void send_dhcp_dns_info_to_host(uint8_t network_up, uint8_t send_wifi_connected);

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

#endif
