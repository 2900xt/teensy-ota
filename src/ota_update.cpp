/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Implementation of the application-facing OTA staging API. See ota_update.h.
 */
#include "ota_update.h"

#include <Arduino.h>
#include <SdFat.h>
#include <string.h>

#include "app_header.h"
#include "ota_boot_state.h"
#include "ota_crc32.h"
#include "ota_hex.h"
#include "ota_sd_line.h"

namespace {

// CRC32 + byte count over an open file's raw bytes (the outer integrity the
// bootloader re-checks before erasing slot A). Reuses a caller scratch buffer.
void file_crc_len(FsFile& f, uint32_t* crc_out, uint32_t* len_out) {
      uint8_t buf[512];
      uint32_t crc = OTA_CRC32_INIT;
      uint32_t len = 0;
      for (;;) {
            const int r = f.read(buf, sizeof(buf));
            if (r <= 0) break;
            crc = ota_crc32_update(crc, buf, (uint32_t)r);
            len += (uint32_t)r;
      }
      *crc_out = ota_crc32_final(crc);
      *len_out = len;
}

} // namespace

ota_arm_result_t ota_arm_update(SdFs& sd, const char* sd_path, uint32_t unix_time) {
      // The bootloader only flashes images under OTA_HEX_DIR; reject anything else
      // here so the caller gets a clear error instead of a failed commit next boot.
      if (strncasecmp(sd_path, OTA_HEX_DIR_PREFIX, sizeof(OTA_HEX_DIR_PREFIX) - 1) != 0)
            return OTA_ARM_BAD_PATH;

      FsFile f = sd.open(sd_path, O_RDONLY);
      if (!f) return OTA_ARM_NOT_FOUND;
      uint32_t crc = 0, len = 0;
      file_crc_len(f, &crc, &len);
      f.close();
      if (len == 0) return OTA_ARM_SD_ERR; // empty / unreadable

      // Write the pending descriptor. O_TRUNC drops any stale descriptor in place.
      // Ensure the /ota tree exists (the bootloader creates it on boot, but the app
      // may stage before the first bootloader run).
      sd.mkdir("/ota");
      sd.mkdir(OTA_HEX_DIR);
      FsFile d = sd.open(OTA_PENDING_TXT_PATH, O_WRONLY | O_CREAT | O_TRUNC);
      if (!d) return OTA_ARM_SD_ERR;
      d.printf("%s\n", sd_path);
      d.printf("%08lX\n", (unsigned long)crc);
      d.printf("%lu\n", (unsigned long)len);
      d.printf("%lu\n", (unsigned long)unix_time); // commit timestamp for the history
      d.close();

      // Flip the boot-control flag the bootloader acts on next reset.
      ota_boot_state_t s;
      ota_boot_state_load(&s);
      s.ota_pending = 1;
      ota_boot_state_save(&s);
      return OTA_ARM_OK;
}

ota_arm_result_t ota_arm_update_and_reboot(SdFs& sd, const char* sd_path, uint32_t unix_time) {
      const ota_arm_result_t r = ota_arm_update(sd, sd_path, unix_time);
      if (r != OTA_ARM_OK) return r;
      ota_reboot();
      return r; // unreachable
}

void ota_disarm_update(void) {
      ota_boot_state_t s;
      ota_boot_state_load(&s);
      if (!s.ota_pending) return;
      s.ota_pending = 0;
      ota_boot_state_save(&s);
}

int ota_inspect_file(SdFs& sd, const char* sd_path, ota_file_info_t* out) {
      memset(out, 0, sizeof(*out));

      FsFile f = sd.open(sd_path, O_RDONLY);
      if (!f) return -1;

      // Pass 1: outer file CRC / length over the raw bytes.
      file_crc_len(f, &out->file_crc32, &out->file_len);

      // Pass 2: decode the records to find the address span, slot-A containment,
      // and the app_header at the slot base.
      if (!f.seek(0)) {
            f.close();
            return -1;
      }
      OtaFileReader<FsFile> rd(f);
      ota_hex_t hx;
      ota_hex_init(&hx);

      char line[600];
      uint8_t rec[OTA_HEX_MAX_DATA];
      uint8_t hdr[sizeof(app_header_t)];
      memset(hdr, 0xFF, sizeof(hdr));

      uint32_t min_addr = 0xFFFFFFFFu, max_addr = 0;
      bool any = false, in_slot = true, saw_eof = false, parse_ok = true;

      for (;;) {
            const int ll = rd.line(line, sizeof(line));
            if (ll == -1) break;
            if (ll == -2) {
                  parse_ok = false;
                  break;
            }
            if (ll == 0) continue;

            uint8_t type = 0, len = 0;
            uint32_t addr = 0;
            if (ota_hex_parse_line(&hx, line, &type, &addr, rec, &len) != OTA_HEX_OK) {
                  parse_ok = false;
                  break;
            }
            if (type == OTA_HEX_REC_DATA) {
                  any = true;
                  if (addr < min_addr) min_addr = addr;
                  if (addr + len - 1 > max_addr) max_addr = addr + len - 1;
                  if (addr < APP_SLOT_A_BASE || (addr + len) > APP_SLOT_B_BASE) in_slot = false;
                  for (uint8_t i = 0; i < len; i++) {
                        const uint32_t a = addr + i;
                        if (a >= APP_SLOT_A_BASE && a < APP_SLOT_A_BASE + sizeof(hdr)) {
                              hdr[a - APP_SLOT_A_BASE] = rec[i];
                        }
                  }
            } else if (type == OTA_HEX_REC_EOF) {
                  saw_eof = true;
                  break;
            }
      }
      f.close();

      out->parse_ok = (parse_ok && saw_eof) ? 1 : 0;
      if (any) {
            out->min_addr = min_addr;
            out->max_addr = max_addr;
            out->in_slot_a = in_slot ? 1 : 0;
      }
      // A real slot-A image starts at the slot base and carries the app_header
      // there; only then are the stamped fields meaningful.
      if (any && min_addr == APP_SLOT_A_BASE) {
            const app_header_t* h = (const app_header_t*)hdr;
            if (h->magic == APP_HEADER_MAGIC) {
                  out->has_header = 1;
                  out->img_len = h->img_len;
                  out->img_crc32 = h->crc32;
                  out->version = h->version;
            }
      }
      return 0;
}

void ota_reboot(void) {
      SCB_AIRCR = 0x05FA0004u; // VECTKEY | SYSRESETREQ
      for (;;) {
      }
}
