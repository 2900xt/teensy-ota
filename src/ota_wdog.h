/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Rollback watchdog on the i.MX RT1062 WDOG1.
 *
 * The bootloader arms WDOG1 just before it jumps (branches, not resets) into a
 * freshly-booted slot-A image, so the running watchdog spans the handoff. If the
 * new image hangs before proving itself (never feeding the dog / never reaching
 * ota_mark_healthy()), WDOG1 resets the chip; the attempt counter was already
 * incremented, so after OTA_BOOT_MAX_ATTEMPTS the bootloader rolls back to GOLDEN.
 * (Outright faults are caught separately by the core fault handler + CrashReport.)
 *
 * WDE is write-once: once armed, software cannot disable the dog until reset, so
 * an armed slot-A app MUST keep feeding it (ota_wdog_feed(), aliased as
 * ota_boot_keepalive()) for as long as it runs. ota_mark_healthy() clears the
 * rollback counter but cannot stop the watchdog.
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
