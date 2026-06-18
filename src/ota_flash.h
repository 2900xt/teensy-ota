/*
 * Copyright BlueVigil LLC 2026
 * All rights reserved
 *
 * RAM-resident QSPI flash erase/program/verify for Teensy 4.1 (i.MX RT1062 +
 * W25Q64 NOR). This is the low-level layer the OTA bootloader uses to write a
 * staged application image into the app slot.
 *
 * Why this is safe to run while "executing from flash": the Teensy 4 linker
 * scripts copy all `.text` to ITCM and `.rodata`/`.data` to DTCM at startup, so
 * after boot the CPU does not fetch instructions or constants over QSPI XIP. The
 * underlying core primitives (eepromemu_flash_*) are likewise in ITCM, disable
 * IRQs across each program/erase command, purge the D-cache, and spin on FlexSPI
 * registers — never touching the QSPI chip that is mid-erase. The only rule the
 * caller must honor: do not access `.flashmem`/`.progmem` or read the slot being
 * programmed from an ISR during a call (IRQs are masked inside each primitive, so
 * this is handled as long as callers don't re-enable them mid-operation).
 *
 * Geometry (W25Q64JV): 8 MiB, 4 KiB sectors, 32K/64K erase blocks, 256-byte
 * program pages. All addresses are absolute (0x60000000-based).
 */
#ifndef BV_OTA_FLASH_H
#define BV_OTA_FLASH_H

#include <stdint.h>

#include "app_header.h" // BOOTLOADER_BASE / APP_SLOT_A_BASE

#define OTA_FLASH_BASE 0x60000000u
#define OTA_FLASH_SIZE 0x00800000u // 8 MiB
#define OTA_FLASH_SECTOR_SIZE 4096u
#define OTA_FLASH_PAGE_SIZE 256u

// Writable window for OTA. The bootloader (below APP_SLOT_A_BASE) and the Teensy
// core's emulated-EEPROM region (the top 63 sectors, base 0x607C0000) are off
// limits — erasing either would brick boot or corrupt persisted settings.
#define OTA_FLASH_WRITABLE_BASE APP_SLOT_A_BASE // 0x60040000
#define OTA_FLASH_EEPROM_BASE 0x607C0000u
#define OTA_FLASH_WRITABLE_END OTA_FLASH_EEPROM_BASE

#ifdef __cplusplus
extern "C" {
#endif

// Status codes (0 == success).
typedef enum {
      OTA_FLASH_OK = 0,
      OTA_FLASH_ERR_RANGE = 1, // outside the writable window
      OTA_FLASH_ERR_ALIGN = 2, // address/length alignment violated
      OTA_FLASH_ERR_VERIFY = 3, // read-back did not match
} ota_flash_status_t;

// Erase [addr, addr+len). `addr` and `len` must be 4 KiB-aligned. Greedily uses
// the largest aligned erase block (64K/32K/4K) so erasing a multi-MB slot costs
// tens of operations, not thousands.
int ota_flash_erase_range(uint32_t addr, uint32_t len);

// Program [addr, addr+len) from `src`. The target must already be erased (0xFF).
// `addr` and `len` must be 4-byte aligned; writes are split at 256-byte page
// boundaries internally.
int ota_flash_write(uint32_t addr, const void* src, uint32_t len);

// Compare flash [addr, addr+len) against `src`. Returns OTA_FLASH_OK if equal.
int ota_flash_verify(uint32_t addr, const void* src, uint32_t len);

// True if every byte in [addr, addr+len) reads as 0xFF (i.e. erased).
int ota_flash_is_erased(uint32_t addr, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif // BV_OTA_FLASH_H
