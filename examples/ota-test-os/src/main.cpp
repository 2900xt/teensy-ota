/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * teensy-ota example: a small interactive "tester OS" over the USB serial
 * console that drives the OTA staging API (ota_update.h). It lets you point at a
 * stamped slot-A Intel-HEX already sitting on the SD card and either inspect it
 * (a no-flash dry run) or commit it — which arms the update and resets, so the
 * bootloader programs it into slot A on the next boot. Serial file transfer is a
 * later milestone; for now the .hex is placed on the SD by other means.
 *
 * It is also a normal slot-A citizen of the M3 rollback safety net: it feeds the
 * rollback watchdog every loop (ota_boot_keepalive) and marks itself healthy
 * after running stably (ota_mark_healthy), so committing a broken image and
 * having it roll back to GOLDEN can be demonstrated end to end. The whole loop is
 * non-blocking so the watchdog is always fed while waiting for a command.
 *
 * Built three ways (see platformio.ini): standalone (teensy41), slot A
 * (teensy41_slotA), and GOLDEN (teensy41_slotB). Define TEENSY_OTA_APP_VERSION to
 * stamp a distinct version so you can confirm a commit actually swapped the image.
 *
 * Commands (type at the serial console, newline-terminated):
 *   help            this help
 *   ls              list *.hex files in /ota
 *   info            boot-control state + slot A / GOLDEN headers
 *   test <path>     inspect a hex (no flash): CRC, header, address range
 *   commit <path>   arm the hex and reboot so the bootloader flashes slot A
 *   abort           clear a pending update
 */
#include <Arduino.h>
#include <SD.h>
#include <string.h>

#include "app_header.h"
#include "ota_boot.h"
#include "ota_boot_state.h"
#include "ota_update.h"

#ifndef TEENSY_OTA_APP_VERSION
#define TEENSY_OTA_APP_VERSION 1
#endif

namespace {

const uint32_t HEALTHY_AFTER_MS = 3000; // run this long before declaring healthy

uint32_t g_start_ms = 0;
bool g_marked_healthy = false;
bool g_sd_ok = false;

uint32_t g_led_ms = 0;
bool g_led_on = false;

char g_cmd[160];
int g_cmd_len = 0;

bool ends_with_hex(const char* s) {
      const size_t n = strlen(s);
      if (n < 4) return false;
      const char* e = s + n - 4;
      return (e[0] == '.' && (e[1] == 'h' || e[1] == 'H') && (e[2] == 'e' || e[2] == 'E') &&
              (e[3] == 'x' || e[3] == 'X'));
}

void print_help() {
      Serial.println();
      Serial.println("teensy-ota tester OS — commands:");
      Serial.println("  help            this help");
      Serial.println("  ls              list *.hex in /ota");
      Serial.println("  info            boot state + slot headers");
      Serial.println("  test <path>     inspect a hex (no flash)");
      Serial.println("  commit <path>   arm + reboot to flash slot A");
      Serial.println("  abort           clear a pending update");
      Serial.print("> ");
}

void print_header(const char* name, uint32_t base) {
      const app_header_t* h = reinterpret_cast<const app_header_t*>(base);
      if (h->magic != APP_HEADER_MAGIC) {
            Serial.printf("  %-7s @ %08lX: no valid image (magic=%08lX)\n\r", name,
                          (unsigned long)base, (unsigned long)h->magic);
            return;
      }
      Serial.printf("  %-7s @ %08lX: ver=%lu img_len=%lu crc32=%08lX\n\r", name,
                    (unsigned long)base, (unsigned long)h->version,
                    (unsigned long)h->img_len, (unsigned long)h->crc32);
}

void cmd_ls() {
      if (!g_sd_ok) {
            Serial.println("SD not mounted");
            return;
      }
      File dir = SD.open("/ota");
      if (!dir || !dir.isDirectory()) {
            Serial.println("no /ota directory on SD");
            if (dir) dir.close();
            return;
      }
      Serial.println("hex files in /ota:");
      int count = 0;
      for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
            if (!e.isDirectory() && ends_with_hex(e.name())) {
                  Serial.printf("  /ota/%s  (%lu bytes)\n\r", e.name(), (unsigned long)e.size());
                  count++;
            }
            e.close();
      }
      dir.close();
      if (count == 0) Serial.println("  (none)");
}

void cmd_info() {
      ota_boot_state_t s;
      ota_boot_state_load(&s);
      Serial.printf("running firmware version: %u\n\r", (unsigned)TEENSY_OTA_APP_VERSION);
      Serial.printf("boot-control: target=%s attempts=%u/%u healthy=%u pending=%u last_commit=%u\n\r",
                    s.boot_target == OTA_BOOT_TARGET_GOLDEN ? "GOLDEN" : "A", s.slotA_attempts,
                    OTA_BOOT_MAX_ATTEMPTS, s.slotA_healthy, s.ota_pending, s.last_commit_result);
      print_header("slot A", APP_SLOT_A_BASE);
      print_header("GOLDEN", APP_SLOT_B_BASE);
      Serial.printf("SD: %s\n\r", g_sd_ok ? "mounted" : "not mounted");
}

void cmd_test(const char* path) {
      ota_file_info_t info;
      if (ota_inspect_file(path, &info) != 0) {
            Serial.printf("cannot read '%s' (SD mounted? path correct?)\n\r", path);
            return;
      }
      Serial.printf("inspect %s\n\r", path);
      Serial.printf("  file: %lu bytes, crc32=%08lX\n\r", (unsigned long)info.file_len,
                    (unsigned long)info.file_crc32);
      Serial.printf("  parse_ok=%u in_slot_a=%u has_header=%u\n\r", info.parse_ok, info.in_slot_a,
                    info.has_header);
      if (info.max_addr >= info.min_addr) {
            Serial.printf("  address span: %08lX .. %08lX\n\r", (unsigned long)info.min_addr,
                          (unsigned long)info.max_addr);
      }
      if (info.has_header) {
            Serial.printf("  image header: ver=%lu img_len=%lu crc32=%08lX\n\r",
                          (unsigned long)info.version, (unsigned long)info.img_len,
                          (unsigned long)info.img_crc32);
      }
      const bool ok = info.parse_ok && info.in_slot_a && info.has_header;
      Serial.printf("  => %s\n\r", ok ? "looks like a valid slot-A image" : "NOT a valid slot-A image");
}

void cmd_commit(const char* path) {
      // Vet the file first so we don't reboot into a guaranteed rollback.
      ota_file_info_t info;
      if (ota_inspect_file(path, &info) != 0) {
            Serial.printf("cannot read '%s'\n\r", path);
            return;
      }
      if (!(info.parse_ok && info.in_slot_a && info.has_header)) {
            Serial.println("refusing: not a valid slot-A image (run 'test' for details)");
            return;
      }
      Serial.printf("arming %s (ver=%lu, %lu bytes) and rebooting to commit...\n\r", path,
                    (unsigned long)info.version, (unsigned long)info.file_len);
      const ota_arm_result_t r = ota_arm_update(path);
      if (r != OTA_ARM_OK) {
            Serial.printf("arm failed (result=%d)\n\r", (int)r);
            return;
      }
      Serial.flush();
      delay(50);
      ota_reboot();
}

void cmd_abort() {
      ota_disarm_update();
      Serial.println("pending update cleared");
}

// Dispatch one complete command line.
void handle_line(char* line) {
      // Split into command + argument (first space).
      char* arg = strchr(line, ' ');
      if (arg) {
            *arg = '\0';
            arg++;
            while (*arg == ' ') arg++;
      } else {
            arg = line + strlen(line); // empty arg
      }

      if (line[0] == '\0') {
            // blank line: just reprint the prompt
      } else if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
            print_help();
            return;
      } else if (strcmp(line, "ls") == 0) {
            cmd_ls();
      } else if (strcmp(line, "info") == 0) {
            cmd_info();
      } else if (strcmp(line, "abort") == 0) {
            cmd_abort();
      } else if (strcmp(line, "test") == 0) {
            if (*arg) cmd_test(arg);
            else Serial.println("usage: test <path>");
      } else if (strcmp(line, "commit") == 0) {
            if (*arg) cmd_commit(arg);
            else Serial.println("usage: commit <path>");
      } else {
            Serial.printf("unknown command '%s' (try 'help')\n\r", line);
      }
      Serial.print("> ");
}

// Read serial without blocking (the rollback watchdog must keep being fed).
void poll_serial() {
      static bool swallow_next_nl = false;
      while (Serial.available()) {
            const int c = Serial.read();
            if (c == '\r' || c == '\n') {
                  // Treat \r, \n and \r\n all as a single line terminator.
                  if (c == '\n' && swallow_next_nl) {
                        swallow_next_nl = false;
                        continue;
                  }
                  swallow_next_nl = (c == '\r');
                  Serial.println();
                  g_cmd[g_cmd_len] = '\0';
                  handle_line(g_cmd);
                  g_cmd_len = 0;
            } else if (c == 8 || c == 127) { // backspace / delete
                  swallow_next_nl = false;
                  if (g_cmd_len > 0) {
                        g_cmd_len--;
                        Serial.print("\b \b");
                  }
            } else if (g_cmd_len < (int)sizeof(g_cmd) - 1) {
                  swallow_next_nl = false;
                  g_cmd[g_cmd_len++] = (char)c;
                  Serial.write((char)c); // echo
            }
      }
}

void heartbeat() {
      // Non-blocking LED blink so the console stays responsive and the watchdog
      // keeps getting fed.
      const uint32_t now = millis();
      const uint32_t interval = g_led_on ? 100 : 900;
      if (now - g_led_ms >= interval) {
            g_led_on = !g_led_on;
            digitalWrite(LED_BUILTIN, g_led_on ? HIGH : LOW);
            g_led_ms = now;
      }
}

} // namespace

void setup() {
      pinMode(LED_BUILTIN, OUTPUT);
      Serial.begin(115200);
      const uint32_t t0 = millis();
      while (!Serial && (millis() - t0) < 1500) {
      }
      g_start_ms = millis();

      g_sd_ok = SD.begin(BUILTIN_SDCARD);

      Serial.println();
      Serial.printf("=== teensy-ota tester OS (firmware v%u) ===\n\r", (unsigned)TEENSY_OTA_APP_VERSION);
      Serial.printf("SD: %s\n\r", g_sd_ok ? "mounted" : "NOT mounted (insert a FAT32 card)");
      print_help();
}

void loop() {
      // Feed the rollback watchdog the bootloader armed across the handoff. WDOG1
      // cannot be disabled once armed, so this is required the whole run.
      ota_boot_keepalive();

      // Declare healthy once we have run stably, so this image is not a rollback
      // candidate while you work with it.
      if (!g_marked_healthy && (millis() - g_start_ms) >= HEALTHY_AFTER_MS) {
            ota_mark_healthy();
            g_marked_healthy = true;
      }

      poll_serial();
      heartbeat();
}
