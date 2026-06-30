// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD

#ifndef _esp_api__h_
#define _esp_api__h_

#include "esp.h"

int esp_add_card(struct esp_adapter *adapter);
int esp_remove_card(struct esp_adapter *adapter);
void esp_process_new_packet_intr(struct esp_adapter *adapter);
struct esp_adapter * esp_get_adapter(void);
/* Consumes @skb: 0 = queued, any nonzero = dropped and freed here (NOT
 * "busy, retry"). Callers must neither free nor retry on nonzero. */
int esp_send_packet(struct esp_adapter *adapter, struct sk_buff *skb);
u8 esp_is_bt_supported_over_sdio(u32 cap);
int esp_is_tx_queue_paused(void);
void esp_tx_pause(void);
void esp_tx_resume(void);
int process_init_event(u8 *evt_buf, u8 len);
void process_capabilities(u8 cap);
void process_test_capabilities(u8 cap);
int is_host_sleeping(void);

#endif
