// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2022 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef __NETWORK_ADAPTER_PRIV__H
#define __NETWORK_ADAPTER_PRIV__H

#include "adapter.h"

typedef struct {
    interface_context_t *context;
} adapter;

struct wow_config {
    uint8_t any;
    uint8_t disconnect;
    uint8_t magic_pkt;
    uint8_t four_way_handshake;
    uint8_t eap_identity_req;
};

#endif
