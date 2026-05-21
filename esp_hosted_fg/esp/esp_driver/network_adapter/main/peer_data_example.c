// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2025 Espressif Systems (Shanghai) PTE LTD

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_err.h"
#include "esp_log.h"
#include "slave_control.h"
#include "peer_data_example.h"

static const char *TAG = "peer_data_example";

/* Peer data transfer test msg IDs — mirror
 * examples/peer_data_transfer/linux_rpc on the eh_cp tree. */
#define MSG_ID_CAT    1   /* host -> CP: small  */
#define MSG_ID_MEOW   2   /* CP -> host: echo small  */
#define MSG_ID_DOG    3   /* host -> CP: medium */
#define MSG_ID_WOOF   4   /* CP -> host: echo medium */
#define MSG_ID_HUMAN  5   /* host -> CP: large  */
#define MSG_ID_HELLO  6   /* CP -> host: echo large  */

static esp_err_t send_echo_event(uint32_t event_id, const void *data, size_t len)
{
	custom_rpc_unserialised_data_t evt = {0};

	evt.custom_msg_id = event_id;
	if (data && len > 0) {
		evt.data = (uint8_t *)malloc(len);
		if (!evt.data) {
			ESP_LOGE(TAG, "alloc %zu bytes for echo event[%" PRIu32 "] failed",
				len, event_id);
			return ESP_FAIL;
		}
		memcpy(evt.data, data, len);
		evt.data_len = len;
		evt.free_func = free;
	}
	return send_custom_rpc_unserialised_event(&evt);
}

/* Runs in the RPC Rx thread — keep it fast, non-blocking. */
static esp_err_t handle_request(const custom_rpc_unserialised_data_t *req,
				custom_rpc_unserialised_data_t *resp_out)
{
	esp_err_t ret = ESP_OK;

	ESP_LOGI(TAG, "req[%" PRIu32 "] len=%u", req->custom_msg_id, req->data_len);

	memset(resp_out, 0, sizeof(*resp_out));
	resp_out->custom_msg_id = req->custom_msg_id;

	switch (req->custom_msg_id) {
	case MSG_ID_CAT:
		ret = send_echo_event(MSG_ID_MEOW,  req->data, req->data_len);
		break;
	case MSG_ID_DOG:
		ret = send_echo_event(MSG_ID_WOOF,  req->data, req->data_len);
		break;
	case MSG_ID_HUMAN:
		ret = send_echo_event(MSG_ID_HELLO, req->data, req->data_len);
		break;
	default:
		ESP_LOGW(TAG, "unhandled req[%" PRIu32 "], acking only",
			req->custom_msg_id);
		break;
	}
	return ret;
}

esp_err_t peer_data_example_register(void)
{
	return register_custom_rpc_unserialised_req_handler(handle_request);
}
