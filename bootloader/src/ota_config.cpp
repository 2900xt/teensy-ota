/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * OTA bootloader configuration — see ota_config.h. Mounts the SD, self-provisions
 * the /ota/ folder and a default config.txt if absent, then parses key=value lines
 * into ota_config_t with per-field validation and default fallback.
 */
#include "ota_config.h"

#include <SD.h>
#include <stdlib.h>
#include <string.h>

#include "ota_sd_line.h"

namespace {

// The preset written to a fresh card. Mirrors the compiled defaults and documents
// each knob's range so the file is self-explanatory when edited in the field.
const char kDefaultConfig[] =
    "# teensy-ota bootloader configuration\n"
    "# Auto-generated defaults. Edit a value and reset the board to apply.\n"
    "\n"
    "# Failed slot-A boots (no ota_mark_healthy) before rollback. Range 1-255.\n"
    "max_attempts=3\n"
    "\n"
    "# Rollback watchdog window in ms, armed before jumping to slot A.\n"
    "# Range 500-128000 (WDOG1 resolution is 500 ms).\n"
    "wdog_timeout_ms=8000\n"
    "\n"
    "# Preferred boot target when the persisted boot state is fresh: A or GOLDEN.\n"
    "boot_target=A\n"
    "\n"
    "# Attempt-counted rollback: 1 enabled, 0 boots the preferred target directly\n"
    "# (no attempt counter, no watchdog).\n"
    "rollback=1\n"
    "\n"
    "# Baud rate for this bootloader's serial log.\n"
    "baud=115200\n"
    "\n"
    "# Verbose boot log: 1 prints the full diagnostic log, 0 prints key lines only.\n"
    "verbose=1\n";

// Trim leading/trailing ASCII whitespace in place; returns the start of content.
char* trim(char* s) {
      while (*s == ' ' || *s == '\t') s++;
      char* end = s + strlen(s);
      while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = '\0';
      return s;
}

// Parse an open config file into `c` (defaults already applied by the caller).
void parse_config(File& f, ota_config_t* c);

// Apply one already-split key/value pair to `c`, clamping/validating each field.
void apply_kv(ota_config_t* c, const char* key, const char* val) {
      if (strcmp(key, "max_attempts") == 0) {
            const unsigned long v = strtoul(val, nullptr, 10);
            if (v >= 1 && v <= 255) c->max_attempts = static_cast<uint8_t>(v);
      } else if (strcmp(key, "wdog_timeout_ms") == 0) {
            const unsigned long v = strtoul(val, nullptr, 10);
            if (v >= 500 && v <= 128000) c->wdog_timeout_ms = static_cast<uint32_t>(v);
      } else if (strcmp(key, "boot_target") == 0) {
            if (strcasecmp(val, "GOLDEN") == 0 || strcmp(val, "1") == 0)
                  c->boot_target = OTA_BOOT_TARGET_GOLDEN;
            else if (strcasecmp(val, "A") == 0 || strcmp(val, "0") == 0)
                  c->boot_target = OTA_BOOT_TARGET_A;
      } else if (strcmp(key, "rollback") == 0) {
            c->rollback_enabled = (strtoul(val, nullptr, 10) != 0) ? 1 : 0;
      } else if (strcmp(key, "baud") == 0) {
            const unsigned long v = strtoul(val, nullptr, 10);
            if (v >= 1200 && v <= 6000000) c->serial_baud = static_cast<uint32_t>(v);
      } else if (strcmp(key, "verbose") == 0) {
            c->verbose = (strtoul(val, nullptr, 10) != 0) ? 1 : 0;
      }
      // Unknown keys are ignored (forward-compatible).
}

void parse_config(File& f, ota_config_t* c) {
      OtaFileReader rd(f);
      char line[160];
      for (;;) {
            const int ll = rd.line(line, sizeof(line));
            if (ll < 0) break;
            char* s = trim(line);
            if (*s == '\0' || *s == '#') continue;
            char* eq = strchr(s, '=');
            if (!eq) continue;
            *eq = '\0';
            apply_kv(c, trim(s), trim(eq + 1));
      }
}

} // namespace

void ota_config_default(ota_config_t* c) {
      c->max_attempts = OTA_CONFIG_DEFAULT_MAX_ATTEMPTS;
      c->wdog_timeout_ms = OTA_CONFIG_DEFAULT_WDOG_TIMEOUT_MS;
      c->boot_target = OTA_CONFIG_DEFAULT_BOOT_TARGET;
      c->rollback_enabled = OTA_CONFIG_DEFAULT_ROLLBACK;
      c->serial_baud = OTA_CONFIG_DEFAULT_BAUD;
      c->verbose = OTA_CONFIG_DEFAULT_VERBOSE;
}

void ota_config_load(ota_config_t* c, Stream& log) {
      ota_config_default(c);

      if (!SD.begin(BUILTIN_SDCARD)) {
            log.println("config: SD mount failed; using compiled defaults");
            return;
      }

      // Self-provision: create /ota/ and a default config.txt on a fresh card so
      // the folder the app also relies on exists from the very first boot.
      if (!SD.exists(OTA_CONFIG_DIR)) {
            if (SD.mkdir(OTA_CONFIG_DIR))
                  log.println("config: created " OTA_CONFIG_DIR " folder");
            else
                  log.println("config: WARN could not create " OTA_CONFIG_DIR " folder");
      }

      if (!SD.exists(OTA_CONFIG_TXT_PATH)) {
            // Fresh card: write the default preset. The in-memory defaults already
            // match it, so there is nothing to parse back this boot.
            File w = SD.open(OTA_CONFIG_TXT_PATH, FILE_WRITE);
            if (w) {
                  w.write(reinterpret_cast<const uint8_t*>(kDefaultConfig),
                          sizeof(kDefaultConfig) - 1);
                  w.close();
                  log.println("config: wrote default preset " OTA_CONFIG_TXT_PATH);
            } else {
                  log.println("config: WARN cannot create " OTA_CONFIG_TXT_PATH
                              "; using compiled defaults");
            }
      } else {
            File f = SD.open(OTA_CONFIG_TXT_PATH, FILE_READ);
            if (f) {
                  parse_config(f, c);
                  f.close();
            } else {
                  log.println("config: cannot open " OTA_CONFIG_TXT_PATH "; using defaults");
            }
      }

      log.printf(
          "config: max_attempts=%u wdog=%lums target=%s rollback=%u baud=%lu verbose=%u\n\r",
          c->max_attempts, static_cast<unsigned long>(c->wdog_timeout_ms),
          c->boot_target == OTA_BOOT_TARGET_GOLDEN ? "GOLDEN" : "A", c->rollback_enabled,
          static_cast<unsigned long>(c->serial_baud), c->verbose);
}
