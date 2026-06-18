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
 * This bootloader is application-agnostic: it boots whichever Vigil firmware is
 * linked into slot A, so the same binary serves top-panel, GCS, and any other
 * Teensy firmware that builds a `*_slotA` image.
 */
#include <Arduino.h>

#include "app_header.h"

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

      blink(3, 80, 80);

      const app_header_t* hdr = reinterpret_cast<const app_header_t*>(APP_SLOT_A_BASE);
      Serial.printf("slot A @ %08lX  magic=%08lX  entry=%08lX  ver=%lu\n\r",
                    static_cast<unsigned long>(APP_SLOT_A_BASE),
                    static_cast<unsigned long>(hdr->magic),
                    static_cast<unsigned long>(hdr->entry),
                    static_cast<unsigned long>(hdr->version));

      if (hdr->magic != APP_HEADER_MAGIC) {
            Serial.println("no valid app image in slot A; staying in bootloader");
            return;
      }

      // Sanity: entry must point into the app slot.
      if (hdr->entry < APP_SLOT_A_BASE || hdr->entry >= (APP_SLOT_A_BASE + 0x380000u)) {
            Serial.println("app entry out of slot range; refusing to jump");
            return;
      }

      Serial.println("valid app found; jumping to slot A...");
      Serial.flush();
      delay(50);
      jump_to_app(APP_SLOT_A_BASE);
}

void loop() {
      // Only reached when no valid app is present: slow heartbeat.
      blink(1, 1000, 1000);
}
