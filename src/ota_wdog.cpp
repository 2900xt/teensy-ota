/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * WDOG1 arm/feed for the rollback watchdog (see ota_wdog.h).
 */
#include "ota_wdog.h"

#include <Arduino.h> // WDOG1_WCR / WDOG1_WSR / WDOG1_WMCR, WDOG_WCR_* (imxrt.h)

void ota_wdog_arm(uint32_t timeout_ms) {
      // WCR.WT is an 8-bit field counting half-seconds, where a stored value N
      // means a timeout of (N + 1) * 0.5 s. So 0 -> 0.5 s and 255 -> 128 s. Round
      // the request up so we never arm a shorter window than asked, then clamp to
      // the representable range.
      uint32_t half_seconds = (timeout_ms + 499u) / 500u;
      if (half_seconds < 1u) half_seconds = 1u;
      if (half_seconds > 128u) half_seconds = 128u;
      const uint16_t wt = static_cast<uint16_t>(half_seconds - 1u);

      // WDOG1 is clocked through CCM_CCGR3; the Teensy core never enables this gate,
      // and with it off the WCR writes below land on a dead peripheral whose counter
      // never runs. Turn it on before configuring (harmless if already on).
      CCM_CCGR3 |= CCM_CCGR3_WDOG1(CCM_CCGR_ON);

      // Disable the separate WDOG1 power-down counter (WMCR.PDE). It is enabled by
      // reset default and is write-once within the first 16 s after power-up; clear
      // it here (the bootloader runs immediately at boot) so only the timeout we
      // configure governs resets.
      WDOG1_WMCR = 0;

      // SRS and WDA must stay high so an expiry drives an internal system reset
      // rather than asserting the external WDOG_B reset pin. WDE enables the dog
      // and is write-once until the next reset.
      WDOG1_WCR = static_cast<uint16_t>(WDOG_WCR_WT(wt) | WDOG_WCR_WDE | WDOG_WCR_SRS | WDOG_WCR_WDA);
}

void ota_wdog_feed(void) {
      // The reload requires the two service words back-to-back with no other WSR
      // write in between; mask interrupts across the pair so an ISR that also fed
      // the dog cannot interleave. Restore the prior interrupt state rather than
      // forcing them on, so this is safe to call from any context.
      uint32_t primask;
      __asm__ volatile("mrs %0, primask" : "=r"(primask));
      __disable_irq();
      WDOG1_WSR = 0x5555;
      WDOG1_WSR = 0xAAAA;
      if (!primask) __enable_irq();
}
