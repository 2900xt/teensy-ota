/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * See ota_flash_selftest.h.
 */
#include "ota_flash_selftest.h"

#include "ota_flash.h"

#include <Print.h>
#include <time.h>

namespace {

constexpr uint32_t kPatternLen = 1024; // < one 4 KiB sector

// A non-trivial pattern: a stuck address line, a swapped chunk, or a stuck bit
// all change the read-back, unlike an all-0x00 or incrementing fill.
void fill_pattern(uint8_t* buf, uint32_t len, uint32_t seed) {
      uint32_t x = seed ? seed : 0xA5A5A5A5u;
      for (uint32_t i = 0; i < len; i++) {
            // xorshift32 — cheap, decent diffusion.
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            buf[i] = static_cast<uint8_t>(x ^ (i * 31u));
      }
}

bool step(Print& out, const char* name, int rc) {
      out.printf("  %-22s %s", name, rc == OTA_FLASH_OK ? "ok" : "FAIL");
      if (rc != OTA_FLASH_OK) out.printf(" (rc=%d)", rc);
      out.println();
      return rc == OTA_FLASH_OK;
}

} // namespace

bool ota_flash_selftest(Print& out, uint32_t scratch_addr) {
      static uint8_t pattern[kPatternLen];
      fill_pattern(pattern, kPatternLen, time(NULL));

      out.println("=== OTA flash self-test ===");
      out.printf("scratch sector @ %08lX  (%lu-byte pattern)\n\r", static_cast<unsigned long>(scratch_addr), static_cast<unsigned long>(kPatternLen));

      bool ok = true;

      // 1. Erase the scratch sector and confirm it reads all-0xFF.
      ok &= step(out, "erase", ota_flash_erase_range(scratch_addr, OTA_FLASH_SECTOR_SIZE));
      ok &= step(out, "blank-check", ota_flash_is_erased(scratch_addr, kPatternLen));

      // 2. Program the pattern and read it back.
      ok &= step(out, "write", ota_flash_write(scratch_addr, pattern, kPatternLen));
      ok &= step(out, "verify", ota_flash_verify(scratch_addr, pattern, kPatternLen));

      // 3. Re-erase so we leave the scratch region clean (and prove erase clears
      //    programmed bits, not just an already-blank sector).
      ok &= step(out, "re-erase", ota_flash_erase_range(scratch_addr, OTA_FLASH_SECTOR_SIZE));
      ok &= step(out, "blank-check-2", ota_flash_is_erased(scratch_addr, kPatternLen));

      out.printf("=== self-test %s ===\n\r", ok ? "PASSED" : "FAILED");
      return ok;
}
