/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Minimal teensy-ota example app: a double-blink heartbeat.
 *
 * Built three ways (see platformio.ini): standalone (teensy41), slot A
 * (teensy41_slotA, the OTA-bootable image), and GOLDEN (teensy41_slotB). The
 * slot builds carry an app_header at the slot base — emitted by the library
 * when TEENSY_OTA_SLOT_BUILD is defined — so the resident bootloader can
 * validate the image and branch into it.
 *
 * The double-blink is deliberately different from the bootloader's 3 fast
 * blinks, so the LED alone tells you whether the handoff succeeded.
 */
#include <Arduino.h>

void setup() {
      pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
      // distinct double-blink: the app is running (vs. the bootloader's 3 blinks)
      digitalWrite(LED_BUILTIN, HIGH);
      delay(80);
      digitalWrite(LED_BUILTIN, LOW);
      delay(120);
      digitalWrite(LED_BUILTIN, HIGH);
      delay(80);
      digitalWrite(LED_BUILTIN, LOW);
      delay(720);
}
