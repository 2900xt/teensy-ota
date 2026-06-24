/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Bootloader configuration loaded from the SD card. Knobs that were previously
 * compile-time constants (rollback attempts, watchdog timeout, default boot
 * target, serial/log verbosity) are read from `/ota/config.txt` at the top of
 * every boot, so they can be retuned in the field without reflashing.
 *
 * The config lives in the same `/ota/` folder the app uses for staged images and
 * the commit history. On the first boot of a fresh SD card (no `/ota/` folder, or
 * no `config.txt`) the bootloader creates the folder and writes a default preset
 * file, then runs with those defaults — so a blank card is self-provisioning.
 *
 * Format: one `key=value` per line, `#` comments and blank lines ignored, keys
 * case-sensitive. Unknown keys are skipped; an out-of-range or malformed value
 * falls back to that field's compiled default rather than failing the boot.
 */
#ifndef TEENSY_OTA_CONFIG_H
#define TEENSY_OTA_CONFIG_H

#include <stdint.h>

#include "ota_boot_state.h" // ota_boot_target_t

// On-SD config file, alongside pending.txt / commits.csv in the /ota/ folder.
#define OTA_CONFIG_DIR "/ota"
#define OTA_CONFIG_TXT_PATH "/ota/config.txt"

// Compiled fallbacks, used when the file is absent or a field is invalid. The
// rollback/watchdog defaults mirror the historical compile-time constants.
#define OTA_CONFIG_DEFAULT_MAX_ATTEMPTS 3u
#define OTA_CONFIG_DEFAULT_WDOG_TIMEOUT_MS 8000u
#define OTA_CONFIG_DEFAULT_BOOT_TARGET OTA_BOOT_TARGET_A
#define OTA_CONFIG_DEFAULT_ROLLBACK 1u
#define OTA_CONFIG_DEFAULT_BAUD 115200u
#define OTA_CONFIG_DEFAULT_VERBOSE 1u

typedef struct {
      uint8_t max_attempts;     // failed slot-A boots before rollback (1..255)
      uint32_t wdog_timeout_ms; // rollback watchdog window (500..128000)
      uint8_t boot_target;      // preferred target when state is fresh (ota_boot_target_t)
      uint8_t rollback_enabled; // 1: attempt-counted rollback; 0: boot preferred target directly
      uint32_t serial_baud;     // baud for the bootloader serial log
      uint8_t verbose;          // 1: print the full boot log; 0: quiet (key lines only)
} ota_config_t;

#ifdef __cplusplus

#include <Stream.h>

// Fill `c` with the compiled defaults (see the OTA_CONFIG_DEFAULT_* macros).
void ota_config_default(ota_config_t* c);

// Mount the SD, ensure `/ota/` exists, and load `/ota/config.txt` into `c`. If the
// folder or file is missing it is created with the default preset (and a one-line
// note is logged). On any SD failure `c` is left at the compiled defaults. Invalid
// individual fields fall back to their defaults. Logs a one-line effective summary.
void ota_config_load(ota_config_t* c, Stream& log);

#endif // __cplusplus

#endif // TEENSY_OTA_CONFIG_H
