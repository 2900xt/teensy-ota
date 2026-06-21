/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Application-facing OTA boot API (see ota_boot.h).
 */
#include "ota_boot.h"

#include "ota_boot_state.h"
#include "ota_wdog.h"

void ota_mark_healthy(void) {
      ota_boot_state_t s;
      ota_boot_state_load(&s);

      // Running from GOLDEN means a rollback is in effect (boot_target is sticky
      // GOLDEN until a new OTA is committed). The recovery image shares this source
      // and may also call ota_mark_healthy(); clearing the counter here would let
      // the bootloader retry a known-bad slot A, so refuse.
      if (s.boot_target != OTA_BOOT_TARGET_A) return;

      // Already healthy and counter clear: nothing to persist (avoids flash wear).
      if (s.slotA_healthy && s.slotA_attempts == 0) return;

      s.slotA_healthy = 1;
      s.slotA_attempts = 0;
      ota_boot_state_save(&s);
}

void ota_boot_keepalive(void) {
      ota_wdog_feed();
}
