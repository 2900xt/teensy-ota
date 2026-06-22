/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * OTA commit (M4): apply a staged firmware update into slot A, inside the
 * bootloader, before slot selection. The running application never writes flash;
 * it only stages a hex file on the SD card and sets the ota_pending flag (M5/M6,
 * or the OTA_DEMO_FORCE_PENDING bench hook). On the next reset the bootloader
 * mounts the SD, reads /ota/pending.txt, and programs slot A from the staged
 * Intel-HEX.
 *
 * Safety ordering: the staged file's outer CRC (recorded in pending.txt) is
 * verified BEFORE slot A is erased, so a corrupt transfer never touches flash
 * and the existing image stays bootable. Only once the file passes its integrity
 * check do we erase + program + re-verify the stamped image CRC. A failure after
 * the erase leaves slot A CRC-invalid, which the normal boot path already handles
 * by falling back to GOLDEN.
 *
 * This lives in the bootloader project (not the shared library src/) so the SD
 * library is never pulled into consuming application builds.
 */
#ifndef TEENSY_OTA_COMMIT_H
#define TEENSY_OTA_COMMIT_H

#include <Arduino.h> // Stream

#include "ota_boot_state.h" // ota_commit_result_t

// The pending-descriptor path/format (OTA_PENDING_TXT_PATH) is the contract with
// the app-side staging API; see ota_update.h.

// Apply the pending SD-staged update into slot A. Streams a human-readable
// progress report to `log`. Returns an ota_commit_result_t: OTA_COMMIT_OK on a
// fully programmed and verified slot A, otherwise the failure stage
// (OTA_COMMIT_SD_ERR / OTA_COMMIT_PARSE_ERR / OTA_COMMIT_FLASH_ERR /
// OTA_COMMIT_VERIFY_ERR). The caller owns clearing ota_pending and updating
// boot_target / last_commit_result.
ota_commit_result_t ota_commit_pending(Stream& log);

#endif // TEENSY_OTA_COMMIT_H
