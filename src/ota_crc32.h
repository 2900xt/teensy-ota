/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * CRC32 for the OTA image-integrity contract. Standard IEEE 802.3 / zlib CRC32
 * (reflected, poly 0xEDB88320, init/xor 0xFFFFFFFF) so the host stamper
 * (stamp_header.py, Python's zlib.crc32) and the device compute identical values.
 *
 * `ota_app_image_crc()` covers [slot_base, slot_base + img_len) with the
 * app_header's 4-byte crc32 field treated as zero — matching what the stamper
 * does before computing — so the bootloader recomputes the same value.
 */
#ifndef TEENSY_OTA_CRC32_H
#define TEENSY_OTA_CRC32_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One-shot CRC32 over [data, data+len). Equivalent to Python zlib.crc32(data).
uint32_t ota_crc32(const void* data, uint32_t len);

// Incremental CRC32. Seed the first call with OTA_CRC32_INIT; pass the returned
// value back in for each subsequent chunk; finalize with ota_crc32_final().
#define OTA_CRC32_INIT 0xFFFFFFFFu
uint32_t ota_crc32_update(uint32_t crc, const void* data, uint32_t len);
static inline uint32_t ota_crc32_final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

// CRC32 over the application image in [slot_base, slot_base+img_len), with the
// app_header's crc32 field treated as zero (the stamping convention). Reads the
// region directly from memory-mapped flash; `img_len` must already be bounded by
// the caller to the slot size before calling.
uint32_t ota_app_image_crc(uint32_t slot_base, uint32_t img_len);

// True (1) iff the slot at `slot_base` holds a bootable image: valid magic, an
// entry pointing inside the slot, an in-range img_len, and a stamped CRC that
// matches. The single source of truth for "is this slot good", shared by the
// boot path and the commit re-verify.
int ota_slot_bootable(uint32_t slot_base);

#ifdef __cplusplus
}
#endif

#endif // TEENSY_OTA_CRC32_H
