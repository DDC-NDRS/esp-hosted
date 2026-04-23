// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2025 Espressif Systems (Shanghai) PTE LTD
//
// Peer data transfer example on the slave side. Mirrors the
// examples/peer_data_transfer/linux_rpc app on the eh_cp tree:
// host requests CAT(1)/DOG(3)/HUMAN(5) payloads and the slave
// echoes each back as an event with msg_id MEOW(2)/WOOF(4)/HELLO(6).

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the peer-data request handler with slave_control. */
esp_err_t peer_data_example_register(void);

#ifdef __cplusplus
}
#endif
