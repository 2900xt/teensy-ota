/*
 * Copyright (c) 2026 Taha Rawjani
 * SPDX-License-Identifier: MIT
 *
 * Streaming Intel-HEX line decoder, shared by the bootloader commit path and the
 * app-side staging/inspect API. Decodes one text line at a time and tracks the
 * running type-04 extended-linear-address so a sequence of lines yields absolute
 * byte addresses. The caller owns reading lines (SD File, serial, ...); this
 * module is pure C with no I/O dependency. Record format/checksum match
 * scripts/stamp_header.py, so its output round-trips here bit-for-bit.
 */
#ifndef TEENSY_OTA_HEX_H
#define TEENSY_OTA_HEX_H

#include <stdint.h>

// Intel-HEX record types this decoder understands. Other types (segment
// addressing) are reported via *type unchanged and carry no data; Teensy builds
// never emit them.
#define OTA_HEX_REC_DATA 0x00
#define OTA_HEX_REC_EOF 0x01
#define OTA_HEX_REC_EXT_LINEAR 0x04

// Largest payload an Intel-HEX data record can carry (the count byte is 8-bit).
#define OTA_HEX_MAX_DATA 255

typedef enum {
      OTA_HEX_OK = 0,
      OTA_HEX_ERR_FORMAT = 1, // bad prefix, non-hex/odd chars, length, or checksum
} ota_hex_status_t;

// Per-file decoder state: the extended-linear upper address accumulated from
// type-04 records. One instance per hex file; reset with ota_hex_init.
typedef struct {
      uint32_t upper; // upper 16 bits of the address, already shifted left 16
} ota_hex_t;

#ifdef __cplusplus
extern "C" {
#endif

void ota_hex_init(ota_hex_t* h);

// Decode one NUL-terminated Intel-HEX line (no trailing newline). On OTA_HEX_OK:
//   *type = record type (OTA_HEX_REC_*).
//   DATA:       *addr = absolute byte address; *len payload bytes copied to `out`
//               (caller provides a buffer of at least OTA_HEX_MAX_DATA bytes).
//   EXT_LINEAR: the instance's upper address is updated; *len = 0.
//   other:      *len = 0.
// Returns OTA_HEX_ERR_FORMAT on any malformed line.
ota_hex_status_t ota_hex_parse_line(ota_hex_t* h, const char* line, uint8_t* type,
                                    uint32_t* addr, uint8_t* out, uint8_t* len);

#ifdef __cplusplus
}
#endif

#endif // TEENSY_OTA_HEX_H
