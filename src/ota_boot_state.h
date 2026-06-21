/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Boot-control state (M3): a small, CRC-protected struct that persists across
 * resets and the bootloader->app handoff. It is the shared contract that lets the
 * bootloader implement attempt-counted rollback and the running app signal that a
 * freshly-booted slot-A image is healthy.
 *
 * Storage: the Teensy emulated EEPROM (top flash sectors, reserved by the core via
 * FLASH_SECTORS=63 and excluded from the ota_flash writable window). Both the
 * bootloader and the application are ordinary Teensy/Arduino images, so both link
 * the core EEPROM emulation and see the *same* physical backing store — a value
 * the bootloader writes just before jumping is read back by the app, and survives
 * a reset. Writes go through EEPROM.put(), which only rewrites bytes that actually
 * change, so steady-state operation costs no flash wear.
 *
 * Integrity: the struct carries its own CRC32 (over every byte before the crc32
 * field) so a torn write — e.g. power loss mid-update — is detected on load and
 * falls back to a safe default rather than acting on garbage.
 *
 * This header is the cross-component contract (like app_header.h); keep the layout
 * and the enum values stable, and bump OTA_BOOT_STATE_VERSION on any change.
 */
#ifndef TEENSY_OTA_BOOT_STATE_H
#define TEENSY_OTA_BOOT_STATE_H

#include <stdint.h>

// 'OTBS' (OTa Boot State), little-endian.
#define OTA_BOOT_STATE_MAGIC 0x4F544253u
#define OTA_BOOT_STATE_VERSION 1u

// Byte offset of the boot-control struct within the emulated EEPROM. Kept at 0;
// nothing else in this project uses EEPROM, and the struct is only 16 bytes.
#define OTA_BOOT_STATE_EEPROM_ADDR 0

// Number of consecutive slot-A boot attempts that may fail to reach
// ota_mark_healthy() before the bootloader gives up on slot A and falls back to
// the immutable GOLDEN image. The bootloader increments the counter before each
// jump into slot A and the app clears it once stable, so a crash/hang loop trips
// rollback after this many tries.
#define OTA_BOOT_MAX_ATTEMPTS 3u

// Which image the bootloader should run. GOLDEN is sticky once selected by a
// rollback and is only cleared when a new OTA image is committed (M4).
typedef enum {
      OTA_BOOT_TARGET_A = 0,      // slot A, the OTA-updatable image
      OTA_BOOT_TARGET_GOLDEN = 1, // slot B, the immutable factory recovery image
} ota_boot_target_t;

// Outcome of the most recent bootloader commit attempt (M4 populates these; M3
// only ever stores OTA_COMMIT_NONE, but the field is carried now so the struct
// layout is stable across milestones).
typedef enum {
      OTA_COMMIT_NONE = 0,
      OTA_COMMIT_OK = 1,
      OTA_COMMIT_PARSE_ERR = 2,
      OTA_COMMIT_FLASH_ERR = 3,
      OTA_COMMIT_VERIFY_ERR = 4,
      OTA_COMMIT_SD_ERR = 5,
} ota_commit_result_t;

// 16 bytes, no padding: magic(4) version(2) then six u8 fields (offsets 6..11)
// then crc32(4) at offset 12. The CRC covers bytes [0, 12). See the static_asserts
// in ota_boot_state.cpp.
typedef struct {
      uint32_t magic;             // OTA_BOOT_STATE_MAGIC when valid
      uint16_t version;           // OTA_BOOT_STATE_VERSION
      uint8_t boot_target;        // ota_boot_target_t
      uint8_t slotA_attempts;     // ++ by bootloader before jumping to A; cleared by app
      uint8_t slotA_healthy;      // set by app via ota_mark_healthy() once stable
      uint8_t ota_pending;        // an armed update is staged on SD (M4/M6)
      uint8_t last_commit_result; // ota_commit_result_t (M4)
      uint8_t reserved;           // pad to keep crc32 4-byte aligned; must be 0
      uint32_t crc32;             // CRC32 over the preceding 12 bytes
} ota_boot_state_t;

#ifdef __cplusplus
extern "C" {
#endif

// Fill `s` with the safe power-on default: magic/version set, boot_target=A,
// counters cleared, not healthy, nothing pending. The crc32 field is left for
// ota_boot_state_save() to stamp.
void ota_boot_state_default(ota_boot_state_t* s);

// CRC32 over the struct excluding its own crc32 field (the stored integrity value).
uint32_t ota_boot_state_crc(const ota_boot_state_t* s);

// Read the struct from EEPROM and validate magic, version and CRC. On any failure
// `s` is overwritten with the default (see ota_boot_state_default) and false is
// returned; on success `s` holds the persisted state and true is returned.
int ota_boot_state_load(ota_boot_state_t* s);

// Stamp the CRC and persist to EEPROM. EEPROM.put() only rewrites changed bytes,
// so calling this with an unchanged struct is effectively free (no flash wear).
void ota_boot_state_save(ota_boot_state_t* s);

#ifdef __cplusplus
}
#endif

#endif // TEENSY_OTA_BOOT_STATE_H
