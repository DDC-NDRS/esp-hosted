// SPDX-License-Identifier: GPL-2.0-only
/*
 * Espressif Systems Wireless LAN device driver
 *
 * Copyright (C) 2015-2025 Espressif Systems (Shanghai) PTE LTD
 *
 * This software file (the "File") is distributed by Espressif Systems (Shanghai)
 * PTE LTD under the terms of the GNU General Public License Version 2, June 1991
 * (the "License").  You may use, redistribute and/or modify this File in
 * accordance with the terms and conditions of the License, a copy of which
 * is available by writing to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA or on the
 * worldwide web at http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt.
 *
 * THE FILE IS DISTRIBUTED AS-IS, WITHOUT WARRANTY OF ANY KIND, AND THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE
 * ARE EXPRESSLY DISCLAIMED.  The License provides additional details about
 * this warranty disclaimer.
 */

#ifndef __ESP_HOSTED_HOST_EXT_PEER_DATA_TRANSFER_H__
#define __ESP_HOSTED_HOST_EXT_PEER_DATA_TRANSFER_H__

#include <stdint.h>
#include <stddef.h>
#include "ctrl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register (or unregister) a callback for a custom peer message id.
 *
 * Passing callback == NULL unregisters a previously registered callback
 * for msg_id_exp.
 *
 * @return SUCCESS, FAILURE, or CALLBACK_NOT_REGISTERED.
 */
int esp_hosted_register_custom_callback(uint32_t msg_id_exp,
        void (*callback)(uint32_t msg_id_recvd, const uint8_t *data_recvd,
                         size_t data_len_recvd, void *local_context),
        void *local_context);

/**
 * @brief Dispatch an incoming CTRL_EVENT_CUSTOM_RPC_UNSERIALISED_MSG event
 *        to the matching registered callback.
 */
int esp_hosted_peer_data_handle_event(ctrl_cmd_t *app_event);

/**
 * @brief Send a custom (unserialised) peer data message to the slave.
 */
int esp_hosted_send_custom_data(uint32_t msg_id_to_send,
        const uint8_t *data_to_send, size_t data_len_to_send);

#ifdef __cplusplus
}
#endif

#endif /* __ESP_HOSTED_HOST_EXT_PEER_DATA_TRANSFER_H__ */
