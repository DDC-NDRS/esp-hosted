This migration guide documents key changes in ESP-Hosted-FG that users must be aware of when upgrading between versions.

Always keep the **host driver** and **slave firmware** on the **same release** — they are version-checked at runtime, and a mismatched pair is refused. Version string format: `FG-<major1>.<major2>.<minor>.<patch1>.<patch2>` (e.g. `FG-2.0.0.0.0`); `major1` is the breaking wire-protocol number.

#### Index
1. [2.0.0.0.0 - SDIO Frame Aggregation and Larger Transport Buffers](#coloryellow-text20000---sdio-frame-aggregation-and-larger-transport-buffers)

# $${\color{yellow} \text{2.0.0.0.0 - SDIO Frame Aggregation and Larger Transport Buffers}}$$

## Migration needed from versions

| Firmware | Version          | Migration required |
| -------- | ---------------- | ------------------ |
| Host     | < FG-2.0.0.0.0   | ✅                  |
| Slave    | < FG-2.0.0.0.0   | ✅                  |

## Reason for change

`major1` was bumped **1 → 2** because SDIO frame aggregation (packing several frames per transfer / streaming) and larger transport buffers are a **breaking wire-protocol change** versus FG-1.x. A FG-1.x host cannot talk to a FG-2.x slave (or vice versa).

- Version fields: [`common/include/esp_fw_version.h`](../../common/include/esp_fw_version.h)
- Runtime check: [`host/linux/host_driver/esp32/esp_fw_verify.c`](../../host/linux/host_driver/esp32/esp_fw_verify.c)

## Compatibility rules

| Situation | Result |
| --------- | ------ |
| `major1` mismatch (old ↔ new) | **Fatal** — host refuses to attach |
| Full version mismatch, default strict mode | **Fatal** — "Incompatible ESP firmware release" |
| New host + **old** slave (missing `ESP_PRIV_AGGR_CONFIG` TLV) | **Tolerated** — host falls back to the non-aggregated datapath |
| Old host + **new** slave | **Not tolerated** — `major1` mismatch → refuse |

> [!NOTE]
> Tolerance is **one-way**: a newer host can talk to an older slave, but an older host cannot talk to a newer slave. A debug-only `FW_CHECK_OFF` override exists in the host driver but must **not** be used for a release.

## Upgrade procedure — in-field (OTA the slave)

> There is **no** window where an old host keeps working with a new slave; the host swap below is mandatory.

1. **OTA the new slave firmware** while the *old* host is still loaded. The OTA control path (`ota_begin` / `ota_write` / `ota_end`) is version-stable, so an old host can flash a new slave. The slave validates the image and reboots after ~5 s. See [`ota_update.md`](ota_update.md). *(Stop BT/BLE first; do not interrupt OTA.)*
2. The new slave reboots → the old host now **refuses** it (`major1` mismatch). This is expected.
3. `rmmod` the old host driver.
4. Build + `insmod` the **new** host driver (matching the new slave release).
5. **Verify:** `dmesg` shows a single matched `ESP Firmware version: FG-…` line and **no** "Incompatible" line.

## Upgrade procedure — bench / development (USB flash the slave)

Flash the new slave over USB **and** rebuild + `insmod` the new host driver **from the same checkout** in one operation — never pair a new slave with a stale host `.ko` (a version mismatch taints results). Confirm the matched version line as above.

## What a release contains (binary matrix)

A release is **not** a single binary. Slave firmware varies by:

| Axis | Variants | How |
| ---- | -------- | --- |
| **Chip** | ESP32 / S2 / S3 / C2 / C3 / C5 / C6/C61 | per-chip `sdkconfig.defaults.<chip>`, separate build |
| **Transport** | SDIO / SPI (+ UART for BT) | compile-time `CONFIG_`, separate build |
| **BT** | on / off | separate build |

- **Raw-TP is not a binary variant** — it is a runtime / TLV capability toggle.
- **Host driver:** one kernel module per transport, built against the target kernel.

So a full release is one slave binary per **chip × transport × BT** combination, plus the matching host driver.

## Cross-references

- [`ota_update.md`](ota_update.md) — OTA mechanics (begin / write / end APIs)
- [`Getting_started.md`](Getting_started.md) — station / SoftAP operations
- [`SDIO_setup.md`](SDIO_setup.md) / [`SPI_setup.md`](SPI_setup.md) — transport bring-up
- ESP-Hosted-FG [README — Throughput performance](../../README.md#5-throughput-performance) — C5 reference numbers
