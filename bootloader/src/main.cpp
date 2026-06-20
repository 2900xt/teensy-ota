/*
 * Copyright BlueVigil LLC 2026
 * All rights reserved
 *
 * OTA boot prototype - resident bootloader.
 *
 * Goal of this prototype: prove the handoff. The ROM boots this image at
 * 0x60000000; it then branches into an application linked at the slot base
 * 0x60040000 and lets the app's own (unmodified) Teensy startup take over.
 *
 * Identification by LED (USB CDC re-enumerates across the jump, so the blink
 * pattern is the reliable signal):
 *   bootloader present  -> 3 fast blinks before jumping
 *   no valid app        -> slow 1 Hz heartbeat (stays in bootloader)
 *   app running          -> distinct double-blink (see the app's own main.cpp)
 *
 * Dual-slot selection (M2): the bootloader boots slot A when it holds a valid,
 * CRC-verified image, and otherwise falls back to the immutable GOLDEN image in
 * slot B. GOLDEN is directly bootable — recovery needs no flashing and no SD. The
 * richer boot-target / attempt-counter / rollback policy lands in M3; here the
 * rule is simply "good slot A, else good GOLDEN, else stay in bootloader."
 *
 * This bootloader is application-agnostic: it boots whichever Vigil firmware is
 * linked into the slots, so the same binary serves top-panel, GCS, and any other
 * Teensy firmware that builds `*_slotA` / `*_slotB` images.
 */
#include <Arduino.h>

#include "app_header.h"
#include "ota_crc32.h"
#ifdef OTA_FLASH_SELFTEST
#include "ota_flash_selftest.h"
#endif

// Slot span (APP_SLOT_SIZE, from app_header.h; matches both app_slotA.ld and
// app_slotB.ld FLASH LENGTH). Used to bound img_len before the CRC walk so a
// corrupt header can never make us read past the slot.

namespace {

void blink(int n, int on_ms, int off_ms) {
      for (int i = 0; i < n; i++) {
            digitalWrite(LED_BUILTIN, HIGH);
            delay(on_ms);
            digitalWrite(LED_BUILTIN, LOW);
            delay(off_ms);
      }
}

// Tear down everything the bootloader started, then branch to the app's
// ResetHandler. The app's startup is `naked`: it reconfigures FlexRAM, sets its
// own stack pointer, re-copies ITCM/DATA from its slot, and installs its own
// vector table. So we only have to hand over a quiet machine and jump.
[[noreturn]] void jump_to_app(uint32_t slot_base) {
      const app_header_t* hdr = reinterpret_cast<const app_header_t*>(slot_base);
      const uint32_t entry = hdr->entry; // ResetHandler, Thumb bit already set

      __disable_irq();

      // Stop SysTick so it can't fire before the app installs its vectors.
      volatile uint32_t* syst = reinterpret_cast<volatile uint32_t*>(0xE000E010);
      syst[0] = 0; // SYST_CSR
      syst[2] = 0; // SYST_CVR (clear current value)

      // Disable and clear all NVIC interrupts (i.MX RT1062 has 160 IRQs -> 5 regs).
      volatile uint32_t* nvic_icer = reinterpret_cast<volatile uint32_t*>(0xE000E180);
      volatile uint32_t* nvic_icpr = reinterpret_cast<volatile uint32_t*>(0xE000E280);
      for (int i = 0; i < 5; i++) {
            nvic_icer[i] = 0xFFFFFFFFu;
            nvic_icpr[i] = 0xFFFFFFFFu;
      }

      // Disable the MPU. The bootloader's configure_cache() left it enabled with
      // ITCM mapped READ-ONLY and a null-trap region at 0x0. The app's first
      // startup step copies its code INTO ITCM (memory_copy to _stext == 0x0),
      // which a normal reset allows because the MPU isn't configured until later.
      // Without this, that copy faults (DACCVIOL @ 0x0). The app's own
      // configure_cache() re-enables and reconfigures the MPU after the copy.
      SCB_MPU_CTRL = 0;

      asm volatile("dsb" ::: "memory");
      asm volatile("isb" ::: "memory");

      // Teensy startup relies on PRIMASK being clear (it never calls
      // __enable_irq itself), so re-enable before the branch. Safe now: every
      // peripheral IRQ is masked and SysTick is stopped, so nothing is pending.
      __enable_irq();

      reinterpret_cast<void (*)(void)>(entry)();
      while (true) {} // unreachable
}

// Validate the app_header + stamped CRC at a slot base, streaming a one-line
// report under `name`. Returns true iff the slot holds a bootable image: valid
// magic, an entry pointing inside the slot, an in-range img_len, and a CRC that
// matches the stamped value. Used for both slot A and GOLDEN slot B.
bool slot_bootable(uint32_t slot_base, const char* name) {
      const app_header_t* hdr = reinterpret_cast<const app_header_t*>(slot_base);
      Serial.printf("%s @ %08lX  magic=%08lX  entry=%08lX  ver=%lu\n\r", name,
                    static_cast<unsigned long>(slot_base),
                    static_cast<unsigned long>(hdr->magic),
                    static_cast<unsigned long>(hdr->entry),
                    static_cast<unsigned long>(hdr->version));

      if (hdr->magic != APP_HEADER_MAGIC) {
            Serial.printf("  %s: no valid app image (bad magic)\n\r", name);
            return false;
      }

      // Sanity: entry must point into this slot.
      if (hdr->entry < slot_base || hdr->entry >= (slot_base + APP_SLOT_SIZE)) {
            Serial.printf("  %s: entry out of slot range; refusing\n\r", name);
            return false;
      }

      // Integrity: verify the stamped CRC32. img_len and crc32 are written by
      // stamp_header.py post-build; an un-stamped image (img_len == 0) fails here.
      // Bound img_len to the slot first so a bad header can't walk the CRC out of
      // the slot.
      const uint32_t img_len = hdr->img_len;
      if (img_len < sizeof(app_header_t) || img_len > APP_SLOT_SIZE) {
            Serial.printf("  %s: img_len=%lu out of range [%u, %lu]; refusing\n\r", name,
                          static_cast<unsigned long>(img_len),
                          static_cast<unsigned>(sizeof(app_header_t)),
                          static_cast<unsigned long>(APP_SLOT_SIZE));
            return false;
      }
      const uint32_t want = hdr->crc32;
      const uint32_t got = ota_app_image_crc(slot_base, img_len);
      Serial.printf("  %s: img_len=%lu crc=%08lX (expected %08lX)\n\r", name,
                    static_cast<unsigned long>(img_len),
                    static_cast<unsigned long>(got),
                    static_cast<unsigned long>(want));
      if (got != want) {
            Serial.printf("  %s: CRC mismatch; refusing\n\r", name);
            return false;
      }
      return true;
}

} // namespace

void setup() {
      pinMode(LED_BUILTIN, OUTPUT);
      Serial.begin(115200);
      const uint32_t t0 = millis();
      while (!Serial && (millis() - t0) < 1500) {}

      Serial.println("=== OTA BOOT POC :: BOOTLOADER (0x60000000) ===");

      // If the app faulted last boot, Teensy stashed a crash record (at
      // 0x2027FF80) that survives the reset. Decode it so we can see exactly
      // what killed the handed-off app (fault type + faulting address).
      if (CrashReport) {
            Serial.println("---- last fault (decoded from previous boot) ----");
            Serial.print(CrashReport);
            Serial.println("------------------------------------------------");
      }

#ifdef OTA_FLASH_SELFTEST
      // Bring-up only: prove the RAM-resident flash layer against a scratch
      // sector in the free region before trusting it to write the app slot.
      ota_flash_selftest(Serial);
#endif

      blink(3, 80, 80);

      // Prefer slot A (the OTA-updatable image); fall back to GOLDEN slot B (the
      // immutable factory image) when slot A is blank, un-stamped, or corrupt.
      uint32_t boot_base = 0;
      if (slot_bootable(APP_SLOT_A_BASE, "slot A")) {
            boot_base = APP_SLOT_A_BASE;
      } else {
            Serial.println("slot A not bootable; trying GOLDEN slot B...");
            if (slot_bootable(APP_SLOT_B_BASE, "GOLDEN")) {
                  boot_base = APP_SLOT_B_BASE;
            }
      }

      if (boot_base == 0) {
            Serial.println("no bootable image in either slot; staying in bootloader");
            return;
      }

      Serial.printf("valid app found; jumping to %08lX...\n\r",
                    static_cast<unsigned long>(boot_base));
      Serial.flush();
      delay(50);
      jump_to_app(boot_base);
}

void loop() {
      // Only reached when no valid app is present: slow heartbeat.
      blink(1, 1000, 1000);
}
