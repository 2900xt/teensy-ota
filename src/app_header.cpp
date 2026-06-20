/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Places the app_header_t at the base of the flash slot for an application image.
 * ld/app_slotA.ld pins section ".appheader" to ORIGIN(FLASH), so the bootloader
 * finds this struct at a fixed address (APP_SLOT_A_BASE).
 *
 * This file ships in the shared teensy-ota library; the consuming firmware links
 * it directly (library.json sets libArchive=false so the linker can't drop this
 * otherwise-unreferenced object before app_slotA.ld's KEEP(.appheader) runs).
 *
 * Compiled only in a `*_slotA` build (which defines TEENSY_OTA_SLOT_BUILD and uses
 * the slot linker script). In the default `teensy41` build the app is linked at
 * 0x60000000 with the normal FCB/IVT boot header and must NOT carry an app
 * header, so this translation unit is empty there.
 *
 * `entry` is the address of the Teensy core's ResetHandler. Taking the address
 * of a function yields a value with the Thumb bit set, which is exactly what the
 * bootloader needs to branch to.
 */
#ifdef TEENSY_OTA_SLOT_BUILD

#include "app_header.h"

extern "C" void ResetHandler(void);

__attribute__((section(".appheader"), used)) const app_header_t app_header = {
    APP_HEADER_MAGIC,
    reinterpret_cast<uint32_t>(&ResetHandler),
    0, // img_len  (populated by the OTA pipeline later)
    0, // crc32    (populated by the OTA pipeline later)
    1, // version
};

#endif // TEENSY_OTA_SLOT_BUILD
