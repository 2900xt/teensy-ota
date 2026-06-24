/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Application-facing OTA staging API. A running slot-A app uses this to hand a
 * firmware image to the bootloader: it points at a stamped slot-A Intel-HEX on
 * the SD card, and the bootloader programs it into slot A on the next reset (see
 * bootloader/src/ota_commit.*). The app itself never writes the app flash slot.
 *
 * `ota_arm_update` records the file's path, CRC32, length and a caller-supplied
 * timestamp in the pending descriptor `/ota/pending.txt` and sets the `ota_pending`
 * flag (ota_boot_state.h); the bootloader re-verifies the file against that
 * CRC/length before erasing slot A, and on a successful commit logs the timestamp
 * and image in the commit history (`/ota/commits.csv`). `ota_inspect_file` is a
 * no-flash dry run that reports what an image looks like (CRC/length, header,
 * address range) so a tool/UI can vet it before arming.
 *
 * The pending-descriptor path/format and the commit-history CSV are cross-component
 * contracts with the bootloader commit path; keep them in sync.
 */
#ifndef TEENSY_OTA_UPDATE_H
#define TEENSY_OTA_UPDATE_H

#include <stdint.h>

// The only folder the bootloader will flash an Intel-HEX from. Every staged image
// (a pending commit and every revert from the commit history) MUST live under this
// directory; the bootloader rejects any other path before it touches flash. The
// bootloader creates the folder on boot, so a staged hex belongs at e.g.
// `/ota/hex/app.hex`. Trailing slash included so it composes as a path prefix.
#define OTA_HEX_DIR "/ota/hex"
#define OTA_HEX_DIR_PREFIX "/ota/hex/"

// Pending-update descriptor on the SD card. Four positional lines:
//   line 1: path to the staged slot-A Intel-HEX, under OTA_HEX_DIR (e.g. /ota/hex/app.hex)
//   line 2: CRC32 of that file's raw bytes (hex)
//   line 3: length of that file in bytes (decimal)
//   line 4: caller-supplied commit timestamp (decimal; Unix epoch seconds, 0 if
//           unknown) — logged into the commit history on a successful commit.
#define OTA_PENDING_TXT_PATH "/ota/pending.txt"

// Commit history on the SD card. A header line followed by one row per image the
// bootloader has successfully committed, oldest first:
//   timestamp,path,crc32,len
// (timestamp decimal, path as staged, crc32 8-hex, len decimal). The bootloader
// appends a row after each verified commit, and on an attempt-rollback re-flashes
// the *previous* row's image (the last good firmware) instead of GOLDEN.
#define OTA_COMMITS_CSV_PATH "/ota/commits.csv"
#define OTA_COMMITS_CSV_HEADER "timestamp,path,crc32,len"

typedef enum {
      OTA_ARM_OK = 0,
      OTA_ARM_NOT_FOUND = 1, // the staged file does not exist on the SD card
      OTA_ARM_SD_ERR = 2,    // SD mount, read, or pending.txt write failed
      OTA_ARM_BAD_PATH = 3,  // the staged file is not under OTA_HEX_DIR (/ota/hex/)
} ota_arm_result_t;

// Result of a no-flash inspect of a staged hex (ota_inspect_file).
typedef struct {
      uint32_t file_len;    // raw .hex file size in bytes
      uint32_t file_crc32;  // CRC32 over the raw .hex file bytes
      uint8_t parse_ok;     // every line decoded and an EOF record was seen
      uint8_t has_header;   // a valid app_header (magic) sits at the slot-A base
      uint8_t in_slot_a;    // every data record lies inside slot A
      uint32_t img_len;     // app_header.img_len  (valid iff has_header)
      uint32_t img_crc32;   // app_header.crc32 (stamped) (valid iff has_header)
      uint32_t version;     // app_header.version (valid iff has_header)
      uint32_t min_addr;    // lowest data address seen (valid iff parse saw data)
      uint32_t max_addr;    // highest data address seen, inclusive
} ota_file_info_t;

#ifdef __cplusplus
extern "C" {
#endif

// Stage the Intel-HEX at `sd_path` for commit into slot A on the next reset:
// compute its CRC32/length, write /ota/pending.txt (recording `unix_time` as the
// commit timestamp), and set the ota_pending flag. Pass 0 for unix_time if the
// caller has no wall clock. Does NOT reboot. Returns OTA_ARM_OK on success.
ota_arm_result_t ota_arm_update(const char* sd_path, uint32_t unix_time);

// Stage `sd_path` (as ota_arm_update) and, on success, immediately reset the chip
// so the bootloader commits it. Returns only on failure (with the arm result).
ota_arm_result_t ota_arm_update_and_reboot(const char* sd_path, uint32_t unix_time);

// Clear any pending update (the ota_pending flag). The /ota/pending.txt file and
// the staged hex are left in place.
void ota_disarm_update(void);

// No-flash dry run: read `sd_path` and fill `out`. Returns 0 if the file could be
// read (inspect `out` for validity), non-zero if the SD/file could not be opened.
int ota_inspect_file(const char* sd_path, ota_file_info_t* out);

// Software reset via the Cortex-M AIRCR (SYSRESETREQ). Does not return.
void ota_reboot(void);

#ifdef __cplusplus
}
#endif

#endif // TEENSY_OTA_UPDATE_H
