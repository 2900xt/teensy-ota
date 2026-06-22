/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Persistence for the boot-control struct (see ota_boot_state.h). Backed by the
 * Teensy emulated EEPROM, with a self-CRC so a torn write is detected on load.
 */
#include "ota_boot_state.h"

#include <EEPROM.h>
#include <stddef.h>

#include "ota_crc32.h"

// Layout guarantees the bootloader and app both depend on. If any of these fire,
// the struct is being interpreted differently than it is written and the CRC /
// EEPROM image would silently disagree.
static_assert(sizeof(ota_boot_state_t) == 16, "ota_boot_state_t must be 16 bytes");
static_assert(offsetof(ota_boot_state_t, crc32) == 12, "crc32 must be at offset 12");

void ota_boot_state_default(ota_boot_state_t* s) {
      s->magic = OTA_BOOT_STATE_MAGIC;
      s->version = OTA_BOOT_STATE_VERSION;
      s->boot_target = OTA_BOOT_TARGET_A;
      s->slotA_attempts = 0;
      s->slotA_healthy = 0;
      s->ota_pending = 0;
      s->last_commit_result = OTA_COMMIT_NONE;
      s->slotA_reverted = 0;
      s->crc32 = 0; // stamped by ota_boot_state_save()
}

uint32_t ota_boot_state_crc(const ota_boot_state_t* s) {
      return ota_crc32(s, offsetof(ota_boot_state_t, crc32));
}

int ota_boot_state_load(ota_boot_state_t* s) {
      EEPROM.get(OTA_BOOT_STATE_EEPROM_ADDR, *s);
      if (s->magic != OTA_BOOT_STATE_MAGIC || s->version != OTA_BOOT_STATE_VERSION) {
            ota_boot_state_default(s);
            return 0;
      }
      if (ota_boot_state_crc(s) != s->crc32) {
            ota_boot_state_default(s);
            return 0;
      }
      return 1;
}

void ota_boot_state_save(ota_boot_state_t* s) {
      s->crc32 = ota_boot_state_crc(s);
      EEPROM.put(OTA_BOOT_STATE_EEPROM_ADDR, *s);
}
