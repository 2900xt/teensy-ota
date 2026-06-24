/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * OTA commit — see ota_commit.h. Mounts the SD card, reads the pending
 * descriptor, verifies the staged hex's outer CRC, then erases and programs
 * slot A from the Intel-HEX and re-verifies the stamped image CRC.
 */
#include "ota_commit.h"

#include <SD.h>
#include <stdlib.h>
#include <string.h>

#include "app_header.h"
#include "ota_crc32.h"
#include "ota_flash.h"
#include "ota_hex.h"
#include "ota_sd_line.h"
#include "ota_update.h" // OTA_PENDING_TXT_PATH

namespace {

constexpr uint8_t ERASED = 0xFF; // NOR erased state; gap/tail fill for a sector.

// One flash sector assembled in RAM before being programmed. File-scope static
// (no heap): also doubles as the scratch buffer for the outer-CRC pre-pass.
uint8_t g_sector[OTA_FLASH_SECTOR_SIZE];
char g_line[600]; // one hex text line (longest record + CR/LF)

// Streams bytes (in non-descending address order) into 4 KiB sector buffers and
// programs each sector to flash exactly once. The whole slot is pre-erased, so
// sectors that are skipped entirely simply stay 0xFF, and the partial tail of
// the final sector is programmed as 0xFF (a no-op over erased flash).
class SectorWriter {
 public:
      explicit SectorWriter(Stream& log) : log_(log) {}

      // 0 ok, OTA_FLASH_* on a flash error, -1 on a backwards/overlapping write.
      int put(uint32_t addr, uint8_t b) {
            const uint32_t base = addr & ~(OTA_FLASH_SECTOR_SIZE - 1);
            if (!have_ || base != cur_base_) {
                  if (have_) {
                        if (base < cur_base_) return -1; // not ascending
                        const int e = flush();
                        if (e != OTA_FLASH_OK) return e;
                  }
                  memset(g_sector, ERASED, sizeof(g_sector));
                  cur_base_ = base;
                  have_ = true;
            }
            g_sector[addr - cur_base_] = b;
            return 0;
      }

      // Program the final assembled sector. 0 ok, OTA_FLASH_* otherwise.
      int finish() { return have_ ? flush() : OTA_FLASH_OK; }

 private:
      int flush() {
            const int e = ota_flash_write(cur_base_, g_sector, sizeof(g_sector));
            if (e != OTA_FLASH_OK) {
                  log_.printf("  commit: flash write @ %08lX failed (%d)\n\r",
                              static_cast<unsigned long>(cur_base_), e);
            }
            return e;
      }

      Stream& log_;
      uint32_t cur_base_ = 0;
      bool have_ = false;
};

// Re-verify slot A exactly as the boot path does (shared check: magic, entry,
// img_len, stamped CRC). Returns true iff the freshly programmed image is bootable.
bool verify_slot_a(Stream& log) {
      if (!ota_slot_bootable(APP_SLOT_A_BASE)) {
            log.println("  commit: verify failed (slot A not bootable)");
            return false;
      }
      const app_header_t* hdr = reinterpret_cast<const app_header_t*>(APP_SLOT_A_BASE);
      log.printf("  commit: verify OK (img_len=%lu crc=%08lX ver=%lu)\n\r",
                 static_cast<unsigned long>(hdr->img_len), static_cast<unsigned long>(hdr->crc32),
                 static_cast<unsigned long>(hdr->version));
      return true;
}

// Program slot A from the staged Intel-HEX at `path`, whose raw bytes must match
// `want_crc`/`want_len`. Assumes the SD is already mounted. The outer CRC is
// verified BEFORE slot A is erased, so a corrupt/missing file never touches flash.
// Returns OTA_COMMIT_OK on a programmed and re-verified slot A. Shared by the
// pending-commit and the revert-to-previous paths.
ota_commit_result_t commit_hex_file(const char* path, uint32_t want_crc, uint32_t want_len,
                                    Stream& log) {
      // 0. Source-folder lock: only flash images that live under OTA_HEX_DIR. This
      //    is the authoritative gate — both the pending commit and every revert
      //    flow through here, so nothing reaches flash from outside /ota/hex/. FAT
      //    is case-insensitive, so the prefix match is too.
      if (strncasecmp(path, OTA_HEX_DIR_PREFIX, sizeof(OTA_HEX_DIR_PREFIX) - 1) != 0) {
            log.printf("  commit: staged hex %s is not under %s; refusing\n\r", path,
                       OTA_HEX_DIR_PREFIX);
            return OTA_COMMIT_SD_ERR;
      }

      // 1. Outer integrity check on the staged file, BEFORE touching flash: if the
      //    transfer is corrupt we abort here and the current slot A survives.
      File hex = SD.open(path, FILE_READ);
      if (!hex) {
            log.printf("  commit: cannot open staged hex %s\n\r", path);
            return OTA_COMMIT_SD_ERR;
      }
      {
            uint32_t crc = OTA_CRC32_INIT;
            uint32_t count = 0;
            for (;;) {
                  const int r = hex.read(g_sector, sizeof(g_sector));
                  if (r <= 0) break;
                  crc = ota_crc32_update(crc, g_sector, static_cast<uint32_t>(r));
                  count += static_cast<uint32_t>(r);
            }
            crc = ota_crc32_final(crc);
            if (count != want_len || crc != want_crc) {
                  log.printf("  commit: staged file mismatch (crc %08lX len %lu)\n\r",
                             static_cast<unsigned long>(crc), static_cast<unsigned long>(count));
                  hex.close();
                  return OTA_COMMIT_SD_ERR;
            }
      }
      log.println("  commit: staged file CRC OK; erasing slot A");

      // 2. Erase the whole slot A span (the only OTA-writable app region).
      if (ota_flash_erase_range(APP_SLOT_A_BASE, APP_SLOT_SIZE) != OTA_FLASH_OK) {
            log.println("  commit: slot A erase failed");
            hex.close();
            return OTA_COMMIT_FLASH_ERR;
      }

      // 3. Stream-parse the hex and program slot A. Only records whose every byte
      //    falls strictly inside slot A are accepted; anything else aborts.
      if (!hex.seek(0)) {
            log.println("  commit: cannot rewind staged hex");
            hex.close();
            return OTA_COMMIT_SD_ERR;
      }
      OtaFileReader hr(hex);
      SectorWriter writer(log);
      ota_hex_t hx;
      ota_hex_init(&hx);
      uint8_t rec[OTA_HEX_MAX_DATA];
      bool saw_eof = false;
      ota_commit_result_t result = OTA_COMMIT_OK;

      for (;;) {
            const int ll = hr.line(g_line, sizeof(g_line));
            if (ll == -1) break; // EOF
            if (ll == -2) {
                  log.println("  commit: hex line too long");
                  result = OTA_COMMIT_PARSE_ERR;
                  break;
            }
            if (ll == 0) continue; // blank line

            uint8_t type = 0, len = 0;
            uint32_t addr = 0;
            if (ota_hex_parse_line(&hx, g_line, &type, &addr, rec, &len) != OTA_HEX_OK) {
                  log.println("  commit: bad hex record");
                  result = OTA_COMMIT_PARSE_ERR;
                  break;
            }

            if (type == OTA_HEX_REC_DATA) {
                  if (addr < APP_SLOT_A_BASE || (addr + len) > APP_SLOT_B_BASE) {
                        log.printf("  commit: record @ %08lX out of slot A; aborting\n\r",
                                   static_cast<unsigned long>(addr));
                        result = OTA_COMMIT_PARSE_ERR;
                        break;
                  }
                  for (uint8_t i = 0; i < len; i++) {
                        const int e = writer.put(addr + i, rec[i]);
                        if (e == -1) {
                              log.println("  commit: non-ascending hex record; aborting");
                              result = OTA_COMMIT_PARSE_ERR;
                        } else if (e != OTA_FLASH_OK) {
                              result = OTA_COMMIT_FLASH_ERR;
                        }
                        if (result != OTA_COMMIT_OK) break;
                  }
                  if (result != OTA_COMMIT_OK) break;
            } else if (type == OTA_HEX_REC_EOF) {
                  saw_eof = true;
                  break;
            }
            // Other record types (segment addressing) are unused by Teensy builds.
      }
      hex.close();

      if (result != OTA_COMMIT_OK) return result;
      if (!saw_eof) {
            log.println("  commit: hex ended without EOF record");
            return OTA_COMMIT_PARSE_ERR;
      }

      // 4. Flush the final sector, then re-verify the programmed image.
      if (writer.finish() != OTA_FLASH_OK) return OTA_COMMIT_FLASH_ERR;
      if (!verify_slot_a(log)) return OTA_COMMIT_VERIFY_ERR;
      return OTA_COMMIT_OK;
}

// Append one image to the SD commit history (timestamp,path,crc32,len), writing
// the CSV header first if the file does not yet exist. Best-effort: a logging
// failure does not fail the commit, but it does cost the next revert a step of
// history. SD must already be mounted.
void append_commit_history(uint32_t ts, const char* path, uint32_t crc, uint32_t len,
                           Stream& log) {
      const bool fresh = !SD.exists(OTA_COMMITS_CSV_PATH);
      File h = SD.open(OTA_COMMITS_CSV_PATH, FILE_WRITE); // FILE_WRITE appends
      if (!h) {
            log.printf("  commit: WARN cannot open %s for history\n\r", OTA_COMMITS_CSV_PATH);
            return;
      }
      if (fresh) h.println(OTA_COMMITS_CSV_HEADER);
      h.printf("%lu,%s,%08lX,%lu\n", static_cast<unsigned long>(ts), path,
               static_cast<unsigned long>(crc), static_cast<unsigned long>(len));
      h.close();
      log.printf("  commit: logged to history (ts=%lu)\n\r", static_cast<unsigned long>(ts));
}

// Split a commit-history data line "timestamp,path,crc32,len" into its fields.
// Returns false if the row is malformed or is the header. `path` gets a copy of
// field 1; crc/len are parsed from fields 2/3.
bool parse_history_row(const char* line, char* path, int pathcap, uint32_t* crc, uint32_t* len) {
      const char* c1 = strchr(line, ',');
      if (!c1) return false;
      const char* c2 = strchr(c1 + 1, ',');
      if (!c2) return false;
      const char* c3 = strchr(c2 + 1, ',');
      if (!c3) return false;
      const int plen = static_cast<int>(c2 - (c1 + 1));
      if (plen <= 0 || plen >= pathcap) return false;
      memcpy(path, c1 + 1, plen);
      path[plen] = '\0';
      *crc = strtoul(c2 + 1, nullptr, 16);
      *len = strtoul(c3 + 1, nullptr, 10);
      return true; // header row ("...,path,crc32,len") fails the numeric uses harmlessly
}

} // namespace

ota_commit_result_t ota_commit_pending(Stream& log) {
      log.println("---- OTA commit: applying pending update ----");

      if (!SD.begin(BUILTIN_SDCARD)) {
            log.println("  commit: SD mount failed");
            return OTA_COMMIT_SD_ERR;
      }

      // Read the pending descriptor (4 positional lines: path, crc32, len, ts).
      // The timestamp line is optional for backward compatibility (defaults to 0).
      File desc = SD.open(OTA_PENDING_TXT_PATH, FILE_READ);
      if (!desc) {
            log.printf("  commit: cannot open %s\n\r", OTA_PENDING_TXT_PATH);
            return OTA_COMMIT_SD_ERR;
      }
      char path[256];
      char crc_line[64];
      char len_line[64];
      char ts_line[64];
      uint32_t ts = 0;
      {
            OtaFileReader dr(desc);
            if (dr.line(path, sizeof(path)) <= 0 || dr.line(crc_line, sizeof(crc_line)) <= 0 ||
                dr.line(len_line, sizeof(len_line)) <= 0) {
                  log.println("  commit: malformed pending.txt");
                  desc.close();
                  return OTA_COMMIT_SD_ERR;
            }
            if (dr.line(ts_line, sizeof(ts_line)) > 0) ts = strtoul(ts_line, nullptr, 10);
      }
      desc.close();
      const uint32_t want_crc = strtoul(crc_line, nullptr, 16);
      const uint32_t want_len = strtoul(len_line, nullptr, 10);
      log.printf("  commit: pending path=%s crc32=%08lX len=%lu ts=%lu\n\r", path,
                 static_cast<unsigned long>(want_crc), static_cast<unsigned long>(want_len),
                 static_cast<unsigned long>(ts));

      const ota_commit_result_t r = commit_hex_file(path, want_crc, want_len, log);
      if (r != OTA_COMMIT_OK) return r;

      // Record the committed image so a later attempt-rollback can revert to it.
      append_commit_history(ts, path, want_crc, want_len, log);
      log.println("  commit: slot A updated successfully");
      return OTA_COMMIT_OK;
}

ota_commit_result_t ota_revert_to_previous(Stream& log) {
      log.println("---- OTA revert: re-flashing previous image ----");

      if (!SD.begin(BUILTIN_SDCARD)) {
            log.println("  revert: SD mount failed");
            return OTA_COMMIT_SD_ERR;
      }

      File h = SD.open(OTA_COMMITS_CSV_PATH, FILE_READ);
      if (!h) {
            log.printf("  revert: no commit history (%s); cannot revert\n\r", OTA_COMMITS_CSV_PATH);
            return OTA_COMMIT_SD_ERR;
      }

      // Walk the history keeping the last two valid data rows. The final row is the
      // image currently in slot A (the one failing); the row before it is the last
      // good firmware we revert to. The header row fails to parse and is skipped.
      char prev_path[256] = {0};
      uint32_t prev_crc = 0, prev_len = 0;
      bool have_prev = false;
      {
            OtaFileReader rd(h);
            char cur_path[256] = {0};
            uint32_t cur_crc = 0, cur_len = 0;
            bool have_cur = false;
            for (;;) {
                  const int ll = rd.line(g_line, sizeof(g_line));
                  if (ll == -1) break;
                  if (ll <= 0) continue;
                  if (strncmp(g_line, "timestamp,", 10) == 0) continue; // header row
                  char p[256];
                  uint32_t c = 0, n = 0;
                  if (!parse_history_row(g_line, p, sizeof(p), &c, &n) || n == 0) continue;
                  // shift current -> previous, then take this row as current.
                  if (have_cur) {
                        memcpy(prev_path, cur_path, sizeof(prev_path));
                        prev_crc = cur_crc;
                        prev_len = cur_len;
                        have_prev = true;
                  }
                  memcpy(cur_path, p, sizeof(cur_path));
                  cur_crc = c;
                  cur_len = n;
                  have_cur = true;
            }
      }
      h.close();

      if (!have_prev) {
            log.println("  revert: no previous image on record; falling back to GOLDEN");
            return OTA_COMMIT_SD_ERR;
      }
      log.printf("  revert: previous image path=%s crc32=%08lX len=%lu\n\r", prev_path,
                 static_cast<unsigned long>(prev_crc), static_cast<unsigned long>(prev_len));

      const ota_commit_result_t r = commit_hex_file(prev_path, prev_crc, prev_len, log);
      if (r == OTA_COMMIT_OK) log.println("  revert: slot A restored to previous image");
      return r;
}
