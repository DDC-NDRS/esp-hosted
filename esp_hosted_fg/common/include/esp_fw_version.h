// SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

#ifndef __ESP_FW_VERSION__H
#define __ESP_FW_VERSION__H

/* major1 bumped 1->2: SDIO coalescing (streaming/aggregation) + larger transport
 * buffers are a breaking wire-protocol change vs FG-1.x. The host version check
 * uses this to flag old<->new mismatches. */
#define PROJECT_NAME              "FG"
#define PROJECT_VERSION_MAJOR_1   2
#define PROJECT_VERSION_MAJOR_2   0
#define PROJECT_VERSION_MINOR     0
#define PROJECT_REVISION_PATCH_1  0
#define PROJECT_REVISION_PATCH_2  0

#endif
