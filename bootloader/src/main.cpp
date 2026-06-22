/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Resident bootloader. The ROM boots this image at 0x60000000; it applies any
 * pending SD-staged update, then boots slot A (the OTA-updatable image) when it
 * is the chosen target and holds a valid, CRC-verified image, otherwise falls
 * back to the immutable GOLDEN image in slot B. Attempt-counted rollback (a
 * slot-A image that crashes/hangs without marking itself healthy) reverts to
 * GOLDEN. Application-agnostic: it boots whichever firmware is linked into the
 * slots, so the same binary serves any firmware that builds `*_slotA`/`*_slotB`.
 */
#include <Arduino.h>
#include <limits.h>

#include "app_header.h"
#include "ota_boot_state.h"
#include "ota_commit.h"
#include "ota_crc32.h"
#include "ota_wdog.h"

namespace {

void blink(int n, int on_ms, int off_ms) {
      for (int i = 0; i < n; i++) {
            digitalWrite(LED_BUILTIN, HIGH);
            delay(on_ms);
            digitalWrite(LED_BUILTIN, LOW);
            delay(off_ms);
      }
}

// Hand over a quiet machine and branch to the app's ResetHandler. The app's
// startup reconfigures FlexRAM, sets its own stack, re-copies ITCM/DATA, and
// installs its own vector table, so we only have to stop what we started.
[[noreturn]] void jump_to_app(uint32_t slot_base) {
      const app_header_t* hdr = reinterpret_cast<const app_header_t*>(slot_base);
      const uint32_t entry = hdr->entry; // ResetHandler, Thumb bit already set

      __disable_irq();

      // Stop SysTick so it can't fire before the app installs its vectors.
      volatile uint32_t* syst = reinterpret_cast<volatile uint32_t*>(0xE000E010);
      syst[0] = 0; // SYST_CSR
      syst[2] = 0; // SYST_CVR (clear current value)

      // Disable and clear all NVIC interrupts (i.MX RT1062 has 160 IRQs -> 5 regs).
      volatile uint32_t* nvic_icer = reinterpret_cast<volatile uint32_t*>(0xE000E180);
      volatile uint32_t* nvic_icpr = reinterpret_cast<volatile uint32_t*>(0xE000E280);
      for (int i = 0; i < 5; i++) {
            nvic_icer[i] = 0xFFFFFFFFu;
            nvic_icpr[i] = 0xFFFFFFFFu;
      }

      // Disable the MPU. The bootloader's configure_cache() left it enabled with
      // ITCM mapped READ-ONLY and a null-trap region at 0x0. The app's first
      // startup step copies its code INTO ITCM (memory_copy to _stext == 0x0),
      // which a normal reset allows because the MPU isn't configured until later.
      // Without this, that copy faults (DACCVIOL @ 0x0). The app's own
      // configure_cache() re-enables and reconfigures the MPU after the copy.
      SCB_MPU_CTRL = 0;

      asm volatile("dsb" ::: "memory");
      asm volatile("isb" ::: "memory");

      // Teensy startup relies on PRIMASK being clear (it never calls
      // __enable_irq itself), so re-enable before the branch. Safe now: every
      // peripheral IRQ is masked and SysTick is stopped, so nothing is pending.
      __enable_irq();

      reinterpret_cast<void (*)(void)>(entry)();
      while (true) {} // unreachable
}

// Validate a slot via the shared check (magic, entry, img_len, stamped CRC) and
// log a one-line verdict under `name`. Used for both slot A and GOLDEN slot B.
bool slot_bootable(uint32_t slot_base, const char* name) {
      const app_header_t* hdr = reinterpret_cast<const app_header_t*>(slot_base);
      const bool ok = ota_slot_bootable(slot_base);
      Serial.printf("%s @ %08lX magic=%08lX ver=%lu -> %s\n\r", name,
                    static_cast<unsigned long>(slot_base),
                    static_cast<unsigned long>(hdr->magic),
                    static_cast<unsigned long>(hdr->version), ok ? "bootable" : "invalid");
      return ok;
}

} // namespace

void setup() {
      pinMode(LED_BUILTIN, OUTPUT);
      Serial.begin(115200);
      const uint32_t t0 = millis();
      while (!Serial && (millis() - t0) < 1500) {}

      Serial.println("=== teensy-ota bootloader (0x60000000) ===");

      // If the app faulted last boot, Teensy stashed a crash record that survives
      // the reset. Decode it so we can see what killed the handed-off app.
      if (CrashReport) {
            Serial.println("---- last fault (previous boot) ----");
            Serial.print(CrashReport);
            Serial.println("------------------------------------");
      }

      blink(3, 80, 80);

      // Reset-source decode: WDOG1_WRSR latches why the chip last reset. TOUT means
      // the rollback watchdog fired (a hung slot-A app). Enable the clock gate so
      // the register reads on a cold boot that never armed the dog.
      CCM_CCGR3 |= CCM_CCGR3_WDOG1(CCM_CCGR_ON);
      const uint16_t wrsr = WDOG1_WRSR;
      Serial.printf("reset source (WDOG1_WRSR=%04X): %s%s%s\n\r", wrsr,
                    (wrsr & WDOG_WRSR_POR) ? "power-on " : "",
                    (wrsr & WDOG_WRSR_TOUT) ? "WATCHDOG-TIMEOUT " : "",
                    (wrsr & WDOG_WRSR_SFTW) ? "software " : "");

      // Boot-control state: persisted in emulated EEPROM, drives attempt-counted
      // rollback. Defaults (target=A, attempts=0) if blank/torn.
      ota_boot_state_t st;
      ota_boot_state_load(&st);
      Serial.printf(
          "boot-control: target=%s attempts=%u/%u healthy=%u pending=%u last_commit=%u reverted=%u\n\r",
          st.boot_target == OTA_BOOT_TARGET_GOLDEN ? "GOLDEN" : "A", st.slotA_attempts,
          OTA_BOOT_MAX_ATTEMPTS, st.slotA_healthy, st.ota_pending, st.last_commit_result,
          st.slotA_reverted);

      // Apply a pending SD-staged update before slot selection. The app only
      // stages a hex on the SD and sets ota_pending; the bootloader is the only
      // thing that writes slot A. The commit verifies the staged file's outer CRC
      // before erasing, so a corrupt transfer leaves slot A intact; a failure
      // after erase leaves slot A CRC-invalid and is caught by the GOLDEN fallback.
      if (st.ota_pending) {
            const ota_commit_result_t r = ota_commit_pending(Serial);
            st.ota_pending = 0; // always clear: never re-run a commit (boot-loop)
            st.last_commit_result = static_cast<uint8_t>(r);
            if (r == OTA_COMMIT_OK) {
                  // New image committed: clear any sticky GOLDEN and the attempt
                  // counter so the fresh slot A gets a normal rollback-protected run.
                  st.slotA_attempts = 0;
                  st.slotA_healthy = 0;
                  st.slotA_reverted = 0; // a new image re-arms one revert step
                  st.boot_target = OTA_BOOT_TARGET_A;
                  Serial.println("commit OK; boot_target=A, attempts reset");
            } else {
                  // Failed commit: fall back to GOLDEN this boot.
                  st.boot_target = OTA_BOOT_TARGET_GOLDEN;
                  Serial.printf("commit failed (result=%u); falling back to GOLDEN\n\r", r);
            }
            ota_boot_state_save(&st);
      }

      const bool a_ok = slot_bootable(APP_SLOT_A_BASE, "slot A");

      // Boot slot A only if it is the target, has attempts left, and is valid.
      // Arm rollback before the jump: bump the persisted attempt counter (so a
      // crash/hang that never reaches ota_mark_healthy() leaves it incremented)
      // and start the watchdog, which spans the branch and resets a hung app.
      if (st.boot_target == OTA_BOOT_TARGET_A && st.slotA_attempts < OTA_BOOT_MAX_ATTEMPTS && a_ok) {
            st.slotA_attempts++;
            ota_boot_state_save(&st);
            ota_wdog_arm(OTA_WDOG_DEFAULT_TIMEOUT_MS);
            Serial.printf("booting slot A (attempt %u/%u), watchdog armed %u ms; jumping to %08lX...\n\r",
                          st.slotA_attempts, OTA_BOOT_MAX_ATTEMPTS, OTA_WDOG_DEFAULT_TIMEOUT_MS,
                          static_cast<unsigned long>(APP_SLOT_A_BASE));
            Serial.flush();
            delay(50);
            jump_to_app(APP_SLOT_A_BASE);
      }

      // Slot A burned through its attempts without marking healthy. First try to
      // revert to the PREVIOUS flashed image (re-program slot A from the SD commit
      // history) and give that a fresh rollback-protected run; only if that revert
      // is unavailable or itself fails do we fall back to the GOLDEN failsafe. The
      // slotA_reverted flag makes this one-shot: a recovery image that ALSO fails
      // its attempts goes straight to GOLDEN instead of reverting again.
      if (st.boot_target == OTA_BOOT_TARGET_A && st.slotA_attempts >= OTA_BOOT_MAX_ATTEMPTS) {
            if (!st.slotA_reverted) {
                  Serial.printf("slot A reached %u attempts; reverting to previous image\n\r",
                                OTA_BOOT_MAX_ATTEMPTS);
                  const ota_commit_result_t rr = ota_revert_to_previous(Serial);
                  if (rr == OTA_COMMIT_OK) {
                        // Re-arm slot A for the restored image and reboot so the
                        // normal boot path arms the watchdog and jumps into it.
                        st.slotA_reverted = 1;
                        st.slotA_attempts = 0;
                        st.slotA_healthy = 0;
                        st.boot_target = OTA_BOOT_TARGET_A;
                        ota_boot_state_save(&st);
                        Serial.println("revert OK; rebooting into restored slot A...");
                        Serial.flush();
                        delay(50);
                        SCB_AIRCR = 0x05FA0004u; // VECTKEY | SYSRESETREQ
                        while (true) {}
                  }
                  Serial.printf("revert failed (result=%u); falling back to GOLDEN\n\r", rr);
            } else {
                  Serial.println("recovery image also failed its attempts; falling back to GOLDEN");
            }
            // Make GOLDEN sticky so we stop retrying (a new OTA commit clears it).
            st.boot_target = OTA_BOOT_TARGET_GOLDEN;
            ota_boot_state_save(&st);
      } else if (st.boot_target == OTA_BOOT_TARGET_GOLDEN) {
            Serial.println("boot_target is GOLDEN (sticky rollback); booting recovery image");
      } else {
            Serial.println("slot A not bootable; falling back to GOLDEN slot B...");
      }

      // GOLDEN is the safety net: directly bootable, never attempt-counted, never
      // watchdog-armed (there is nothing further to roll back to).
      if (slot_bootable(APP_SLOT_B_BASE, "GOLDEN")) {
            Serial.printf("jumping to GOLDEN %08lX...\n\r",
                          static_cast<unsigned long>(APP_SLOT_B_BASE));
            Serial.flush();
            delay(50);
            jump_to_app(APP_SLOT_B_BASE);
      }

      Serial.println("no bootable image (slot A unavailable and GOLDEN invalid); staying in bootloader");
      return;
}

void loop() {
      // Only reached when no valid app is present. light up the LED as an error.
      blink(1, INT_MAX, 1000);
}
