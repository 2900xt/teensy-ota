/*
 * Copyright BlueVigil LLC 2026
 * All rights reserved
 *
 * Destructive bench self-test for the RAM-resident flash layer (ota_flash). It
 * erase/program/verify cycles a scratch sector in the FREE flash region
 * (default 0x603C0000 — above slot A, below the emulated-EEPROM region), so it
 * never touches the bootloader, the app slot, or persisted settings.
 *
 * Intended for bring-up: call once from a build that defines OTA_FLASH_SELFTEST.
 * Do NOT ship it on a normal boot path — it erases flash every time it runs.
 */
#ifndef BV_OTA_FLASH_SELFTEST_H
#define BV_OTA_FLASH_SELFTEST_H

#include <stdint.h>

class Print;

// Runs the erase/write/verify sequence against `scratch_addr` (must be 4 KiB
// aligned and inside the OTA writable window). Streams a human-readable report
// to `out`. Returns true on full pass.
bool ota_flash_selftest(Print& out, uint32_t scratch_addr = 0x603C0000u);

#endif // BV_OTA_FLASH_SELFTEST_H
