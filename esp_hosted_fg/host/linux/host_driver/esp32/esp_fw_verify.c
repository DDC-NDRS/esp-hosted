// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015-2021 Espressif Systems (Shanghai) PTE LTD
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
#include "esp_utils.h"

#include "esp.h"
#include "esp_fw_verify.h"

#define VERSION_BUFFER_SIZE 50

// default: we do strict check
static fw_check_t fw_check_type = FW_CHECK_STRICT;

int set_fw_check_type(fw_check_t check)
{
	fw_check_type = check;

	return 0;
}

fw_check_t get_fw_check_type(void)
{
	return fw_check_type;
}

int check_esp_version(struct fw_version *ver)
{
	char version_str[VERSION_BUFFER_SIZE] = { 0 };

	snprintf(version_str, VERSION_BUFFER_SIZE, "%s-%u.%u.%u.%u.%u",
			ver->project_name, ver->major1, ver->major2, ver->minor, ver->revision_patch_1, ver->revision_patch_2);

	esp_info("Driver supports Firmware version: %s\n", RELEASE_VERSION);
	esp_info("ESP Firmware version: %s\n", version_str);

	/* A major-version change is a breaking wire-protocol boundary (FG 1->2:
	 * SDIO coalescing + larger transport buffers). Treat it as fatal for any
	 * check mode except an explicit FW_CHECK_OFF override, and tell the user
	 * which side to update. */
	if (fw_check_type != FW_CHECK_OFF && ver->major1 != PROJECT_VERSION_MAJOR_1) {
		if (ver->major1 > PROJECT_VERSION_MAJOR_1)
			esp_err("Incompatible firmware: ESP %s is NEWER than host driver %s "
					"(major version). Update the host driver (esp_hosted) to match.\n",
					version_str, RELEASE_VERSION);
		else
			esp_err("Incompatible firmware: ESP %s is OLDER than host driver %s "
					"(major version). Update/reflash the ESP firmware to match.\n",
					version_str, RELEASE_VERSION);
		return -1;
	}

	if (fw_check_type == FW_CHECK_STRICT) {
		if (strncmp(RELEASE_VERSION, version_str, strlen(version_str)) != 0) {
			esp_err("Incompatible ESP firmware release detected. Please use correct ESP-Hosted branch/compatible release\n");
			return -1;
		}
	}
	return 0;
}

int process_fw_data(struct fw_version *fw_p, int tag_len)
{
	if (tag_len != sizeof(struct fw_version)) {
		esp_err("Length not matching to firmware data size\n");
		return -1;
	}

	return check_esp_version(fw_p);
}
