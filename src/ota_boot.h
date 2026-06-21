/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Application-facing OTA boot API (M3). This is the small surface a slot-A
 * firmware uses to participate in the rollback safety net:
 *
 *   - call ota_boot_keepalive() regularly (e.g. once per loop) to feed the
 *     rollback watchdog the bootloader armed across the handoff; and
 *   - call ota_mark_healthy() once the image has proven itself stable (reached
 *     idle, run N seconds, passed self-checks — the app decides), which clears the
 *     boot-attempt counter so this image is no longer a rollback candidate.
 *
 * A slot-A image that crashes, hangs, or simply never marks itself healthy within
 * OTA_BOOT_MAX_ATTEMPTS boots is rolled back to the immutable GOLDEN image by the
 * bootloader. See ota_boot_state.h (persistence) and ota_wdog.h (watchdog).
 */
#ifndef TEENSY_OTA_BOOT_H
#define TEENSY_OTA_BOOT_H

#ifdef __cplusplus
extern "C" {
#endif

// Mark the currently running slot-A image as healthy: clear the boot-attempt
// counter and set the healthy flag so the bootloader stops counting this image
// toward rollback. Idempotent and wear-friendly (persists only when something
// actually changed). No-op when running from GOLDEN — a sticky rollback must not
// be cleared by the recovery image, only by committing a new OTA update.
void ota_mark_healthy(void);

// Feed the rollback watchdog. Call periodically while running, comfortably within
// the armed timeout (OTA_WDOG_DEFAULT_TIMEOUT_MS). Required even after
// ota_mark_healthy(): WDOG1 cannot be disabled once armed, so the app owns feeding
// it for as long as it runs. Alias for ota_wdog_feed().
void ota_boot_keepalive(void);

#ifdef __cplusplus
}
#endif

#endif // TEENSY_OTA_BOOT_H
