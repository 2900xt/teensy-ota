/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Destructive bench self-test for the RAM-resident flash layer (ota_flash). It
 * erase/program/verify cycles a scratch sector in the SPARE flash region
 * (default 0x60740000 — above GOLDEN slot B, below the emulated-EEPROM region),
 * so it never touches the bootloader, either app slot, GOLDEN, or persisted
 * settings. (The old default 0x603C0000 is now the GOLDEN slot base, so the
 * scratch had to move up into the spare range or the bench test would erase the
 * recovery image.)
 *
 * Intended for bring-up: call once from a build that defines OTA_FLASH_SELFTEST.
 * Do NOT ship it on a normal boot path — it erases flash every time it runs.
 */
#ifndef TEENSY_OTA_FLASH_SELFTEST_H
#define TEENSY_OTA_FLASH_SELFTEST_H

#include <stdint.h>

class Print;

// Runs the erase/write/verify sequence against `scratch_addr` (must be 4 KiB
// aligned and inside the OTA writable window). Streams a human-readable report
// to `out`. Returns true on full pass.
bool ota_flash_selftest(Print& out, uint32_t scratch_addr = 0x60740000u);

#endif // TEENSY_OTA_FLASH_SELFTEST_H
