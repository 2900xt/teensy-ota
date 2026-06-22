/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * OTA commit: apply a staged firmware update into slot A, inside the bootloader,
 * before slot selection. The running application never writes flash; it only
 * stages a hex file on the SD card and sets the ota_pending flag. On the next
 * reset the bootloader mounts the SD, reads /ota/pending.txt, and programs slot A
 * from the staged Intel-HEX.
 *
 * Safety ordering: the staged file's outer CRC (recorded in pending.txt) is
 * verified BEFORE slot A is erased, so a corrupt transfer never touches flash.
 * Only then do we erase + program + re-verify the stamped image CRC. A failure
 * after the erase leaves slot A CRC-invalid, which the boot path handles by
 * falling back to GOLDEN.
 *
 * On a successful commit a row is appended to the SD commit history
 * (OTA_COMMITS_CSV_PATH): timestamp,path,crc32,len. The boot path uses that
 * history to revert a misbehaving slot-A image to the *previous* flashed image
 * (ota_revert_to_previous) before resorting to GOLDEN.
 *
 * Lives in the bootloader project (not the shared library src/) so the SD library
 * is never pulled into consuming application builds.
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
// boot_target / last_commit_result. On OTA_COMMIT_OK the committed image is also
// appended to the SD commit history (OTA_COMMITS_CSV_PATH).
ota_commit_result_t ota_commit_pending(Stream& log);

// Revert slot A to the previous flashed image: read the commit history, take the
// row before the most recent (the last good firmware), and re-program slot A from
// its staged hex exactly as a commit would (outer-CRC pre-check, erase, program,
// verify). Streams progress to `log`. Returns OTA_COMMIT_OK on a verified slot A,
// OTA_COMMIT_SD_ERR if there is no previous image on record or its hex is missing,
// otherwise the failing stage. Does NOT touch the history (a revert is not a new
// commit) or the boot-control state; the caller owns slot selection and GOLDEN
// fallback.
ota_commit_result_t ota_revert_to_previous(Stream& log);

#endif // TEENSY_OTA_COMMIT_H
