/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Implementation of the streaming Intel-HEX line decoder. See ota_hex.h.
 */
#include "ota_hex.h"

static int hex_nibble(char c) {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
}

void ota_hex_init(ota_hex_t* h) { h->upper = 0; }

ota_hex_status_t ota_hex_parse_line(ota_hex_t* h, const char* line, uint8_t* type,
                                    uint32_t* addr, uint8_t* out, uint8_t* len) {
      *len = 0;
      *addr = 0;

      if (line[0] != ':') return OTA_HEX_ERR_FORMAT;

      // Decode the hex byte pairs into a fixed record buffer: count(1) + addr(2) +
      // type(1) + up to 255 payload + checksum(1) = 260 bytes max.
      uint8_t rec[OTA_HEX_MAX_DATA + 5];
      int n = 0;
      const char* p = line + 1;
      while (p[0] && p[1]) {
            const int hi = hex_nibble(p[0]);
            const int lo = hex_nibble(p[1]);
            if (hi < 0 || lo < 0) return OTA_HEX_ERR_FORMAT;
            if (n >= (int)sizeof(rec)) return OTA_HEX_ERR_FORMAT;
            rec[n++] = (uint8_t)((hi << 4) | lo);
            p += 2;
      }
      if (p[0]) return OTA_HEX_ERR_FORMAT; // trailing odd nibble
      if (n < 5) return OTA_HEX_ERR_FORMAT;

      const uint8_t count = rec[0];
      if (n != count + 5) return OTA_HEX_ERR_FORMAT; // declared length mismatch

      uint8_t sum = 0;
      for (int i = 0; i < n - 1; i++) sum = (uint8_t)(sum + rec[i]);
      if ((uint8_t)((-(int)sum) & 0xFF) != rec[n - 1]) return OTA_HEX_ERR_FORMAT;

      const uint16_t offset = (uint16_t)((rec[1] << 8) | rec[2]);
      const uint8_t rectype = rec[3];
      *type = rectype;

      if (rectype == OTA_HEX_REC_EXT_LINEAR) {
            h->upper = ((uint32_t)rec[4] << 8 | rec[5]) << 16;
      } else if (rectype == OTA_HEX_REC_DATA) {
            *addr = h->upper | offset;
            for (uint8_t i = 0; i < count; i++) out[i] = rec[4 + i];
            *len = count;
      }
      return OTA_HEX_OK;
}
