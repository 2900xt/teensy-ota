/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Implementation of the OTA CRC32. See ota_crc32.h.
 *
 * Bitwise (no 256-entry table) to keep the bootloader's footprint and SRAM use
 * trivial — image CRCs run once per boot over a few MB, so throughput is not a
 * concern. The polynomial/init/xor exactly match Python's zlib.crc32 so the
 * host stamper and the device agree.
 */
#include "ota_crc32.h"

#include "app_header.h" // app_header_t (crc32 field offset)

#include <stddef.h> // offsetof

#define OTA_CRC32_POLY 0xEDB88320u

uint32_t ota_crc32_update(uint32_t crc, const void* data, uint32_t len) {
      const uint8_t* p = (const uint8_t*)data;
      for (uint32_t i = 0; i < len; i++) {
            crc ^= p[i];
            for (int b = 0; b < 8; b++) {
                  uint32_t mask = -(crc & 1u);
                  crc = (crc >> 1) ^ (OTA_CRC32_POLY & mask);
            }
      }
      return crc;
}

uint32_t ota_crc32(const void* data, uint32_t len) {
      return ota_crc32_final(ota_crc32_update(OTA_CRC32_INIT, data, len));
}

uint32_t ota_app_image_crc(uint32_t slot_base, uint32_t img_len) {
      const uint32_t crc_off = (uint32_t)offsetof(app_header_t, crc32);
      const uint8_t* base = (const uint8_t*)(uintptr_t)slot_base;

      uint32_t crc = OTA_CRC32_INIT;
      if (img_len <= crc_off) {
            // Image is smaller than the header's crc field — nothing valid to
            // hash, but stay well-defined.
            crc = ota_crc32_update(crc, base, img_len);
            return ota_crc32_final(crc);
      }

      // [slot_base, crc field): magic, entry, img_len.
      crc = ota_crc32_update(crc, base, crc_off);
      // The 4-byte crc field, treated as zero.
      const uint8_t zeros[4] = {0, 0, 0, 0};
      crc = ota_crc32_update(crc, zeros, sizeof(zeros));
      // [after crc field, slot_base + img_len): the rest of the image.
      const uint32_t after = crc_off + (uint32_t)sizeof(zeros);
      if (img_len > after) {
            crc = ota_crc32_update(crc, base + after, img_len - after);
      }
      return ota_crc32_final(crc);
}

int ota_slot_bootable(uint32_t slot_base) {
      const app_header_t* h = (const app_header_t*)(uintptr_t)slot_base;
      if (h->magic != APP_HEADER_MAGIC) return 0;
      // Entry must point inside the slot.
      if (h->entry < slot_base || h->entry >= slot_base + APP_SLOT_SIZE) return 0;
      // Bound img_len to the slot before the CRC walk so a bad header can't read
      // past the slot. img_len/crc32 are stamped post-build; an un-stamped image
      // (img_len == 0) fails here.
      if (h->img_len < sizeof(app_header_t) || h->img_len > APP_SLOT_SIZE) return 0;
      return ota_app_image_crc(slot_base, h->img_len) == h->crc32;
}
