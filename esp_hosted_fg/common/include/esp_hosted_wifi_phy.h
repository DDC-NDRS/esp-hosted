/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 */

/* Wi-Fi PHY protocol bitmap shared between host (which has no ESP-IDF headers)
 * and slave, so both agree on the values carried in ConnectAP.protocol. The bit
 * values mirror esp_wifi's WIFI_PROTOCOL_* exactly. The Python control app keeps
 * an identical mirror in py_parse/process.py (PROTOCOL_TABLE) - keep in sync. */

#ifndef __ESP_HOSTED_WIFI_PHY_H__
#define __ESP_HOSTED_WIFI_PHY_H__

/* Protocol bits (mirror of esp_wifi WIFI_PROTOCOL_*) */
#define H_WIFI_PROTOCOL_11B    0x01
#define H_WIFI_PROTOCOL_11G    0x02
#define H_WIFI_PROTOCOL_11N    0x04
#define H_WIFI_PROTOCOL_LR     0x08
#define H_WIFI_PROTOCOL_11A    0x10
#define H_WIFI_PROTOCOL_11AC   0x20
#define H_WIFI_PROTOCOL_11AX   0x40
#define H_WIFI_PROTOCOL_MASK   0x7F  /* all known protocol bits */

/* Band-appropriate cumulative profiles (esp_wifi.h: 2.4G uses b/g, 5G uses a;
 * n/ac/ax are cumulative). HT40 requires an 11n profile WITHOUT 11ax/11ac. */
#define H_PHY_2G_LEGACY  (H_WIFI_PROTOCOL_11B | H_WIFI_PROTOCOL_11G)
#define H_PHY_2G_11N     (H_PHY_2G_LEGACY | H_WIFI_PROTOCOL_11N)
#define H_PHY_2G_11AX    (H_PHY_2G_11N | H_WIFI_PROTOCOL_11AX)
#define H_PHY_2G_LR      (H_WIFI_PROTOCOL_LR)

#define H_PHY_5G_LEGACY  (H_WIFI_PROTOCOL_11A)
#define H_PHY_5G_11N     (H_PHY_5G_LEGACY | H_WIFI_PROTOCOL_11N)
#define H_PHY_5G_11AC    (H_PHY_5G_11N | H_WIFI_PROTOCOL_11AC)
#define H_PHY_5G_11AX    (H_PHY_5G_11AC | H_WIFI_PROTOCOL_11AX)

/* PHY bandwidth (mirror of esp_wifi WIFI_BW_HT20/HT40) */
#define H_WIFI_BW_UNSET  0
#define H_WIFI_BW_HT20   1
#define H_WIFI_BW_HT40   2

#endif /* __ESP_HOSTED_WIFI_PHY_H__ */
