/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Shared contract between the resident bootloader and an application image
 * (any Teensy firmware that builds a slot-A image).
 *
 * The application places one app_header_t at the very base of its flash slot
 * (section ".appheader", forced to the slot ORIGIN by ld/app_slotA.ld). The
 * bootloader reads it at a fixed address (APP_SLOT_A_BASE) to decide whether a
 * valid image is present and where to begin execution.
 *
 * `magic`/`entry` drive the boot handoff; `img_len`/`crc32` are stamped post-build
 * by stamp_header.py and verified by the bootloader before every jump (M1, see
 * ota_crc32). `version` is carried for the later rollback/slot-selection logic.
 */
#ifndef TEENSY_OTA_APP_HEADER_H
#define TEENSY_OTA_APP_HEADER_H

#include <stdint.h>

// 'OTAP' (OTA aPplication), little-endian.
#define APP_HEADER_MAGIC 0x4F544150u

// Flash map (Teensy 4.1, 8 MB QSPI mapped at 0x60000000).
//   bootloader : 0x60000000 .. 0x6003FFFF  (256 KiB, owns the FCB/IVT boot header)
//   app slot A : 0x60040000 .. 0x603BFFFF  (3584 KiB, the OTA-updatable application)
//   GOLDEN B   : 0x603C0000 .. 0x6073FFFF  (3584 KiB, immutable factory recovery image)
// Slots A and B are the same size and link the SAME firmware; the bootloader boots
// slot A when it is valid and falls back to GOLDEN slot B otherwise.
// The default `teensy41` env still links the app at 0x60000000 for standalone
// USB flashing / recovery / manufacturing; the `teensy41_slotA` / `teensy41_slotB`
// envs use the slot layout below.
#define BOOTLOADER_BASE 0x60000000u
#define APP_SLOT_A_BASE 0x60040000u
#define APP_SLOT_B_BASE 0x603C0000u // GOLDEN: immutable factory image, never written at runtime
#define APP_SLOT_SIZE 0x00380000u   // 3584 KiB; both slots are identical in size

typedef struct {
      uint32_t magic; // APP_HEADER_MAGIC when a valid image is present
      uint32_t entry; // address of the app's ResetHandler (Thumb bit set)
      uint32_t img_len; // image length in bytes (0 until OTA fills it)
      uint32_t crc32; // CRC32 over the image (0 until OTA fills it)
      uint32_t version; // monotonic firmware version
} app_header_t;

#endif // TEENSY_OTA_APP_HEADER_H
