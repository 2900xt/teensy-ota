/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * OTA commit (M4) — see ota_commit.h. Mounts the SD card, reads the pending
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

namespace {

// Intel-HEX record types (see scripts/stamp_header.py, which mirrors this).
constexpr uint8_t REC_DATA = 0x00;
constexpr uint8_t REC_EOF = 0x01;
constexpr uint8_t REC_EXT_LINEAR = 0x04;

constexpr uint8_t ERASED = 0xFF; // NOR erased state; gap/tail fill for a sector.

// One flash sector assembled in RAM before being programmed, and a single hex
// text line. File-scope statics (no heap): a data record holds <=255 bytes, so a
// 600-byte line covers the longest possible record (':' + 2*(255+5) + CR/LF).
uint8_t g_sector[OTA_FLASH_SECTOR_SIZE];
char g_line[600];

// --- small SD helpers ------------------------------------------------------

// Block-buffered reader over an open File, so the hex parse and the CRC pre-pass
// don't pay a per-byte SD-cache round trip.
class FileReader {
 public:
      explicit FileReader(File& f) : f_(f) {}

      // Next byte, or -1 at EOF.
      int next() {
            if (i_ >= n_) {
                  const int r = f_.read(buf_, sizeof(buf_));
                  if (r <= 0) return -1;
                  n_ = r;
                  i_ = 0;
            }
            return buf_[i_++];
      }

 private:
      File& f_;
      uint8_t buf_[512];
      int n_ = 0;
      int i_ = 0;
};

// Read one line (up to but not including '\n') into `out`, stripping a trailing
// '\r'. Returns the length, 0 for a blank line, or -1 at EOF with no data, or
// -2 if the line would overflow `cap`.
int read_line(FileReader& r, char* out, int cap) {
      int len = 0;
      int c = r.next();
      if (c < 0) return -1; // clean EOF
      while (c >= 0 && c != '\n') {
            if (len >= cap - 1) return -2;
            out[len++] = static_cast<char>(c);
            c = r.next();
      }
      if (len > 0 && out[len - 1] == '\r') len--;
      out[len] = '\0';
      return len;
}

int hex_nibble(char c) {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
}

// Decode an Intel-HEX line (':' + hex byte pairs) into raw bytes, validating the
// trailing checksum. Returns the decoded byte count, or -1 if malformed. The
// caller interprets bytes[0]=count, [1..2]=offset, [3]=rectype, payload, [last]=cksum.
int decode_record(const char* line, uint8_t* bytes, int cap) {
      if (line[0] != ':') return -1;
      int n = 0;
      const char* p = line + 1;
      while (p[0] && p[1]) {
            const int hi = hex_nibble(p[0]);
            const int lo = hex_nibble(p[1]);
            if (hi < 0 || lo < 0) return -1;
            if (n >= cap) return -1;
            bytes[n++] = static_cast<uint8_t>((hi << 4) | lo);
            p += 2;
      }
      if (p[0]) return -1;  // odd number of hex chars
      if (n < 5) return -1; // count + addr(2) + type + checksum
      const uint8_t count = bytes[0];
      if (n != count + 5) return -1; // declared length must match
      uint8_t sum = 0;
      for (int i = 0; i < n - 1; i++) sum = static_cast<uint8_t>(sum + bytes[i]);
      const uint8_t cksum = static_cast<uint8_t>((-static_cast<int>(sum)) & 0xFF);
      if (cksum != bytes[n - 1]) return -1;
      return n;
}

// --- sector-buffered programmer -------------------------------------------

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

// Re-verify slot A exactly as the boot path does: magic, entry in slot, in-range
// img_len, stamped CRC. Returns true iff the freshly programmed image is bootable.
bool verify_slot_a(Stream& log) {
      const app_header_t* hdr = reinterpret_cast<const app_header_t*>(APP_SLOT_A_BASE);
      if (hdr->magic != APP_HEADER_MAGIC) {
            log.println("  commit: verify failed (bad magic)");
            return false;
      }
      if (hdr->entry < APP_SLOT_A_BASE || hdr->entry >= (APP_SLOT_A_BASE + APP_SLOT_SIZE)) {
            log.println("  commit: verify failed (entry out of slot)");
            return false;
      }
      const uint32_t img_len = hdr->img_len;
      if (img_len < sizeof(app_header_t) || img_len > APP_SLOT_SIZE) {
            log.println("  commit: verify failed (img_len out of range)");
            return false;
      }
      const uint32_t got = ota_app_image_crc(APP_SLOT_A_BASE, img_len);
      if (got != hdr->crc32) {
            log.printf("  commit: verify failed (crc %08lX != %08lX)\n\r",
                       static_cast<unsigned long>(got),
                       static_cast<unsigned long>(hdr->crc32));
            return false;
      }
      log.printf("  commit: verify OK (img_len=%lu crc=%08lX ver=%lu)\n\r",
                 static_cast<unsigned long>(img_len), static_cast<unsigned long>(got),
                 static_cast<unsigned long>(hdr->version));
      return true;
}

} // namespace

ota_commit_result_t ota_commit_pending(Stream& log) {
      log.println("---- OTA commit: applying pending update ----");

      if (!SD.begin(BUILTIN_SDCARD)) {
            log.println("  commit: SD mount failed");
            return OTA_COMMIT_SD_ERR;
      }

      // 1. Read the pending descriptor (3 positional lines: path, crc32, len).
      File desc = SD.open(OTA_PENDING_TXT_PATH, FILE_READ);
      if (!desc) {
            log.printf("  commit: cannot open %s\n\r", OTA_PENDING_TXT_PATH);
            return OTA_COMMIT_SD_ERR;
      }
      char path[256];
      char crc_line[64];
      char len_line[64];
      {
            FileReader dr(desc);
            if (read_line(dr, path, sizeof(path)) <= 0 ||
                read_line(dr, crc_line, sizeof(crc_line)) <= 0 ||
                read_line(dr, len_line, sizeof(len_line)) <= 0) {
                  log.println("  commit: malformed pending.txt");
                  desc.close();
                  return OTA_COMMIT_SD_ERR;
            }
      }
      desc.close();
      const uint32_t want_crc = strtoul(crc_line, nullptr, 16);
      const uint32_t want_len = strtoul(len_line, nullptr, 10);
      log.printf("  commit: pending path=%s crc32=%08lX len=%lu\n\r", path,
                 static_cast<unsigned long>(want_crc), static_cast<unsigned long>(want_len));

      // 2. Outer integrity check on the staged file, BEFORE touching flash: if
      //    the transfer is corrupt we abort here and the current slot A survives.
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
                  if (r < 0) break;
                  if (r == 0) break;
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

      // 3. Erase the whole slot A span (the only OTA-writable app region).
      if (ota_flash_erase_range(APP_SLOT_A_BASE, APP_SLOT_SIZE) != OTA_FLASH_OK) {
            log.println("  commit: slot A erase failed");
            hex.close();
            return OTA_COMMIT_FLASH_ERR;
      }

      // 4. Stream-parse the hex and program slot A. Only records whose every byte
      //    falls strictly inside slot A are accepted; anything else aborts.
      if (!hex.seek(0)) {
            log.println("  commit: cannot rewind staged hex");
            hex.close();
            return OTA_COMMIT_SD_ERR;
      }
      FileReader hr(hex);
      SectorWriter writer(log);
      uint32_t upper = 0; // ext-linear upper 16 bits
      uint8_t rec[256 + 5];
      bool saw_eof = false;
      ota_commit_result_t result = OTA_COMMIT_OK;

      for (;;) {
            const int ll = read_line(hr, g_line, sizeof(g_line));
            if (ll == -1) break; // EOF
            if (ll == -2) {
                  log.println("  commit: hex line too long");
                  result = OTA_COMMIT_PARSE_ERR;
                  break;
            }
            if (ll == 0) continue; // blank line

            const int n = decode_record(g_line, rec, sizeof(rec));
            if (n < 0) {
                  log.println("  commit: bad hex record");
                  result = OTA_COMMIT_PARSE_ERR;
                  break;
            }
            const uint8_t count = rec[0];
            const uint16_t offset = static_cast<uint16_t>((rec[1] << 8) | rec[2]);
            const uint8_t rectype = rec[3];

            if (rectype == REC_EXT_LINEAR) {
                  upper = (static_cast<uint32_t>(rec[4]) << 8) | rec[5];
            } else if (rectype == REC_DATA) {
                  const uint32_t addr = (upper << 16) | offset;
                  if (addr < APP_SLOT_A_BASE || (addr + count) > APP_SLOT_B_BASE) {
                        log.printf("  commit: record @ %08lX out of slot A; aborting\n\r",
                                   static_cast<unsigned long>(addr));
                        result = OTA_COMMIT_PARSE_ERR;
                        break;
                  }
                  for (uint8_t i = 0; i < count; i++) {
                        const int e = writer.put(addr + i, rec[4 + i]);
                        if (e == -1) {
                              log.println("  commit: non-ascending hex record; aborting");
                              result = OTA_COMMIT_PARSE_ERR;
                        } else if (e != OTA_FLASH_OK) {
                              result = OTA_COMMIT_FLASH_ERR;
                        }
                        if (result != OTA_COMMIT_OK) break;
                  }
                  if (result != OTA_COMMIT_OK) break;
            } else if (rectype == REC_EOF) {
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

      // 5. Flush the final sector, then re-verify the programmed image.
      if (writer.finish() != OTA_FLASH_OK) return OTA_COMMIT_FLASH_ERR;
      if (!verify_slot_a(log)) return OTA_COMMIT_VERIFY_ERR;

      log.println("  commit: slot A updated successfully");
      return OTA_COMMIT_OK;
}
