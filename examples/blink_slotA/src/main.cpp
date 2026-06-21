/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Minimal teensy-ota example app: a double-blink heartbeat that also exercises
 * the M3 rollback safety net.
 *
 * Built three ways (see platformio.ini): standalone (teensy41), slot A
 * (teensy41_slotA, the OTA-bootable image), and GOLDEN (teensy41_slotB). The
 * slot builds carry an app_header at the slot base — emitted by the library
 * when TEENSY_OTA_SLOT_BUILD is defined — so the resident bootloader can
 * validate the image and branch into it.
 *
 * M3 participation (only meaningful in a slot build, where the bootloader armed
 * the rollback watchdog before jumping here):
 *   - ota_boot_keepalive() every loop feeds that watchdog so a healthy app is not
 *     mistaken for a hang;
 *   - after running stably for OTA_HEALTHY_AFTER_MS we call ota_mark_healthy(),
 *     which clears the boot-attempt counter so this image is no longer a rollback
 *     candidate. An app that crashes or hangs before that point is rolled back to
 *     GOLDEN after OTA_BOOT_MAX_ATTEMPTS boots.
 *
 * Two compile-time demos drive the bench test in OTA_PLAN.md (M3). Define one in
 * the build to prove rollback, then rebuild GOLDEN/slot A as usual:
 *   -D OTA_DEMO_FORCE_FAULT  faults on every boot before marking healthy
 *                            (exercises the crash path: fault -> reboot -> rollback)
 *   -D OTA_DEMO_FORCE_HANG   spins forever without feeding the watchdog
 *                            (exercises the hang path: watchdog reset -> rollback)
 */
#include <Arduino.h>

#include "core_pins.h"
#include "ota_boot.h"

// How long the image must run before it declares itself healthy.
static const uint32_t OTA_HEALTHY_AFTER_MS = 3000;

static uint32_t g_start_ms = 0;
static bool g_marked_healthy = false;

void setup() {
      pinMode(LED_BUILTIN, OUTPUT);
      g_start_ms = millis();
}

void loop() {
      // Feed the rollback watchdog the bootloader armed across the handoff. WDOG1
      // cannot be disabled once armed, so this is required for the whole run, even
      // after the image is marked healthy. (No-op in a standalone build.)
      ota_boot_keepalive();

#ifdef OTA_DEMO_FORCE_HANG
      // Bench demo: never feed again, never mark healthy. The watchdog expires and
      // resets the chip; the un-cleared attempt counter rolls back to GOLDEN.
      while (true) {
            delay(500);
            Serial.println("HANGING");
            Serial.flush();
      }
#endif

#ifdef OTA_DEMO_FORCE_FAULT
      // Bench demo: dereference a null function pointer to take a hard fault before
      // marking healthy. The core fault handler stores CrashReport and reboots; the
      // un-cleared attempt counter rolls back to GOLDEN after OTA_BOOT_MAX_ATTEMPTS.
      if (millis() - g_start_ms > 500) {
            reinterpret_cast<void (*)(void)>(0)();
      }
#endif

      // Once we have run stably for a while, declare the image healthy so the
      // bootloader stops counting it toward rollback.
      if (!g_marked_healthy && (millis() - g_start_ms) >= OTA_HEALTHY_AFTER_MS) {
            ota_mark_healthy();
            g_marked_healthy = true;
      }

      // Distinct double-blink: the app is running (vs. the bootloader's 3 blinks).
      digitalWrite(LED_BUILTIN, HIGH);
      delay(80);
      digitalWrite(LED_BUILTIN, LOW);
      delay(120);
      digitalWrite(LED_BUILTIN, HIGH);
      delay(80);
      digitalWrite(LED_BUILTIN, LOW);
      delay(720);
}
