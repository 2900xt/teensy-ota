/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Implementation of the RAM-resident QSPI flash layer. See ota_flash.h.
 */
#include "ota_flash.h"

#include <Arduino.h>

// Core primitives (cores/teensy4/eeprom.c). They are FASTRUN (ITCM), disable
// IRQs across the program/erase command, purge the D-cache, and wait for the
// chip to go ready before returning. Declared here because the core does not
// expose them in a public header.
extern "C" {
void eepromemu_flash_write(void* addr, const void* data, uint32_t len);
void eepromemu_flash_erase_sector(void* addr);
void eepromemu_flash_erase_32K_block(void* addr);
void eepromemu_flash_erase_64K_block(void* addr);
}

namespace {

// Fully contained in slot A? Rejects len==0 and wraparound. Anything outside
// slot A (bootloader, GOLDEN, EEPROM) is not writable.
bool in_writable_window(uint32_t addr, uint32_t len) {
      if (len == 0) return false;
      if (addr + len < addr) return false; // wraparound
      return addr >= OTA_FLASH_SLOTA_BASE && (addr + len) <= OTA_FLASH_SLOTA_END;
}

} // namespace

// Marked FASTRUN to guarantee ITCM placement even if the linker's catch-all
// `*(.text*)` rule ever changes: this code must not be fetched over QSPI while a
// program/erase is in flight.
FASTRUN int ota_flash_erase_range(uint32_t addr, uint32_t len) {
      if (!in_writable_window(addr, len)) return OTA_FLASH_ERR_RANGE;
      if ((addr % OTA_FLASH_SECTOR_SIZE) != 0 || (len % OTA_FLASH_SECTOR_SIZE) != 0) { return OTA_FLASH_ERR_ALIGN; }

      while (len > 0) {
            uint32_t step;
            if ((addr & 0xFFFFu) == 0 && len >= 0x10000u) {
                  eepromemu_flash_erase_64K_block(reinterpret_cast<void*>(addr));
                  step = 0x10000u;
            } else if ((addr & 0x7FFFu) == 0 && len >= 0x8000u) {
                  eepromemu_flash_erase_32K_block(reinterpret_cast<void*>(addr));
                  step = 0x8000u;
            } else {
                  eepromemu_flash_erase_sector(reinterpret_cast<void*>(addr));
                  step = OTA_FLASH_SECTOR_SIZE;
            }
            addr += step;
            len -= step;
      }
      return OTA_FLASH_OK;
}

FASTRUN int ota_flash_write(uint32_t addr, const void* src, uint32_t len) {
      if (!in_writable_window(addr, len)) return OTA_FLASH_ERR_RANGE;
      if ((addr & 0x3u) != 0 || (len & 0x3u) != 0) return OTA_FLASH_ERR_ALIGN;

      const uint8_t* p = static_cast<const uint8_t*>(src);
      while (len > 0) {
            // A single page-program command must not cross a 256-byte page.
            uint32_t page_room = OTA_FLASH_PAGE_SIZE - (addr & (OTA_FLASH_PAGE_SIZE - 1));
            uint32_t chunk = len < page_room ? len : page_room;
            eepromemu_flash_write(reinterpret_cast<void*>(addr), p, chunk);
            addr += chunk;
            p += chunk;
            len -= chunk;
      }
      return OTA_FLASH_OK;
}
