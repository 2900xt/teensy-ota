/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Rollback watchdog (M3) on the i.MX RT1062 WDOG1.
 *
 * Role in the OTA safety net: the bootloader arms WDOG1 immediately before it
 * jumps into a freshly-booted slot-A image. The jump is a branch, not a reset, so
 * the running watchdog spans the handoff. If the new image hangs before it proves
 * itself (never reaching ota_mark_healthy() / never feeding the dog), WDOG1 resets
 * the chip; because the boot-attempt counter was already incremented and never
 * cleared, the bootloader rolls back to GOLDEN after OTA_BOOT_MAX_ATTEMPTS. This
 * catches hangs; outright faults are caught separately by the core fault handler
 * rebooting (and CrashReport).
 *
 * WDOG1's enable bit (WDE) is write-once: once the bootloader sets it, software
 * cannot clear it until the next reset. So a slot-A app that has been armed MUST
 * keep the dog fed for as long as it runs — call ota_wdog_feed() (the app-facing
 * alias is ota_boot_keepalive()) periodically, comfortably inside the timeout.
 * ota_mark_healthy() clears the rollback *counter* but does not — cannot — stop
 * the watchdog; feeding it remains the app's job.
 */
#ifndef TEENSY_OTA_WDOG_H
#define TEENSY_OTA_WDOG_H

#include <stdint.h>

// Default rollback-watchdog timeout the bootloader arms before jumping to slot A.
// Generous relative to Teensy startup (well under a second to reach loop()), so a
// healthy app trivially feeds it in time, while a true hang trips it. WDOG1's
// resolution is 0.5 s; the usable range is 500..128000 ms.
#define OTA_WDOG_DEFAULT_TIMEOUT_MS 8000u

#ifdef __cplusplus
extern "C" {
#endif

// Enable WDOG1 with the given timeout (clamped/rounded up to WDOG1's 0.5 s grid,
// bounded to [500, 128000] ms). Sets WDE, which is write-once until reset — after
// this call the dog cannot be disabled, only fed. Intended to be called once, by
// the bootloader, just before the jump into slot A.
void ota_wdog_arm(uint32_t timeout_ms);

// Refresh WDOG1 (the 0x5555 / 0xAAAA service sequence). No effect if the dog was
// never armed. Safe to call with interrupts enabled — the two-write sequence is
// issued atomically.
void ota_wdog_feed(void);

#ifdef __cplusplus
}
#endif

#endif // TEENSY_OTA_WDOG_H
