/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Places the app_header_t at the base of the flash slot. ld/app_slotA.ld pins
 * section ".appheader" to ORIGIN(FLASH), so the bootloader finds it at a fixed
 * address (APP_SLOT_A_BASE). library.json sets libArchive=false so the linker
 * keeps this otherwise-unreferenced object until KEEP(.appheader) runs.
 *
 * Compiled only in a `*_slotA`/`*_slotB` build (TEENSY_OTA_SLOT_BUILD); the
 * default `teensy41` build links at 0x60000000 with the normal FCB/IVT header and
 * must NOT carry an app header, so this translation unit is empty there.
 *
 * `entry` is the Teensy core's ResetHandler; its address already has the Thumb
 * bit set, which is what the bootloader branches to.
 */
#ifdef TEENSY_OTA_SLOT_BUILD

#include "app_header.h"

extern "C" void ResetHandler(void);

// The firmware version stamped into the header. Defaults to 1; a consuming build
// can override it (e.g. -D TEENSY_OTA_APP_VERSION=2) to make successive images
// distinguishable — handy for confirming an OTA commit actually took effect.
#ifndef TEENSY_OTA_APP_VERSION
#define TEENSY_OTA_APP_VERSION 1
#endif

__attribute__((section(".appheader"), used)) const app_header_t app_header = {
    APP_HEADER_MAGIC,
    reinterpret_cast<uint32_t>(&ResetHandler),
    0, // img_len  (populated by the OTA pipeline later)
    0, // crc32    (populated by the OTA pipeline later)
    TEENSY_OTA_APP_VERSION,
};

#endif // TEENSY_OTA_SLOT_BUILD
