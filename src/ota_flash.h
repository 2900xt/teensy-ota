/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * RAM-resident QSPI flash erase/program for Teensy 4.1 (W25Q64 NOR). The OTA
 * bootloader uses this to write a staged image into slot A. Safe to run while
 * "executing from flash": after boot the CPU runs from ITCM/DTCM, and the core
 * primitives (eepromemu_flash_*) are ITCM-resident, mask IRQs, and purge the
 * D-cache, so nothing fetches over the QSPI chip mid-erase.
 *
 * Geometry: 8 MiB, 4 KiB sectors, 32K/64K erase blocks, 256-byte program pages.
 * Addresses are absolute (0x60000000-based).
 */
#ifndef TEENSY_OTA_FLASH_H
#define TEENSY_OTA_FLASH_H

#include <stdint.h>

#include "app_header.h" // APP_SLOT_A_BASE / APP_SLOT_B_BASE

#define OTA_FLASH_SECTOR_SIZE 4096u
#define OTA_FLASH_PAGE_SIZE 256u

// Writable window for OTA: slot A only. Everything else is off-limits so no
// runtime flash path can reach it — the bootloader (below slot A), GOLDEN slot B
// (the immutable recovery image), or the emulated EEPROM (top sectors).
#define OTA_FLASH_SLOTA_BASE APP_SLOT_A_BASE // 0x60040000
#define OTA_FLASH_SLOTA_END APP_SLOT_B_BASE  // 0x603C0000 (GOLDEN starts here)

#ifdef __cplusplus
extern "C" {
#endif

// Status codes (0 == success).
typedef enum {
      OTA_FLASH_OK = 0,
      OTA_FLASH_ERR_RANGE = 1, // outside the writable window
      OTA_FLASH_ERR_ALIGN = 2, // address/length alignment violated
} ota_flash_status_t;

// Erase [addr, addr+len). `addr` and `len` must be 4 KiB-aligned. Greedily uses
// the largest aligned erase block (64K/32K/4K).
int ota_flash_erase_range(uint32_t addr, uint32_t len);

// Program [addr, addr+len) from `src`. The target must already be erased (0xFF).
// `addr` and `len` must be 4-byte aligned; writes split at 256-byte page bounds.
int ota_flash_write(uint32_t addr, const void* src, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif // TEENSY_OTA_FLASH_H
