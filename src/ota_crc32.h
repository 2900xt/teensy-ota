/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * CRC32 for the OTA image-integrity contract (M1). This is the standard
 * IEEE 802.3 / zlib CRC32 (reflected, polynomial 0xEDB88320, init 0xFFFFFFFF,
 * final XOR 0xFFFFFFFF) so the host-side stamping tool (stamp_header.py, which
 * uses Python's zlib.crc32) and the on-device bootloader compute bit-identical
 * values over the same bytes.
 *
 * `ota_app_image_crc()` implements the exact convention agreed in OTA_PLAN.md:
 * the CRC covers [slot_base, slot_base + img_len) with the 4-byte crc32 field of
 * the app_header (at offset offsetof(app_header_t, crc32)) treated as zero. The
 * stamping tool zeroes that field before computing, so the bootloader must do
 * the same when it recomputes — hence this shared helper rather than a raw CRC
 * over the literal flash bytes.
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

#ifdef __cplusplus
}
#endif

#endif // TEENSY_OTA_CRC32_H
