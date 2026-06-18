#!/usr/bin/env python3
"""Stamp img_len + crc32 into a slot-linked Vigil application's Intel-HEX (M1).

A `*_slotA` (or later `*_slotB`) build emits an app_header at the slot base with
img_len/crc32 left as 0 (see common/ota/src/app_header.cpp). This post-build step
fills them in so the bootloader can verify image integrity before jumping:

  img_len = (highest byte address in the image) - slot_base + 1
  crc32   = CRC32 over [slot_base, slot_base + img_len), with the 4-byte crc32
            field of the header treated as zero during the computation.

The CRC is the standard zlib/IEEE-802.3 CRC32, matching ota_crc32() on the device
so the bootloader recomputes a bit-identical value. Gaps inside the image span
are hashed as 0xFF — that is what erased NOR flash reads back as, so the on-device
recomputation (which reads real flash) sees the same bytes. The header layout
mirrors app_header_t:

  offset  0  magic       offset  8  img_len
  offset  4  entry       offset 12  crc32        offset 16  version

Usage:
  ./stamp_header.py <in.hex> [out.hex] [--slot-base 0x60040000]

With no out.hex the input is stamped in place. Stamping is idempotent: re-running
recomputes from scratch (crc field zeroed first), so a stamped hex stamps to the
same bytes.
"""

import argparse
import sys
import zlib

# Must match APP_HEADER_MAGIC and APP_SLOT_A_BASE in common/ota/src/app_header.h.
APP_HEADER_MAGIC = 0x42564150  # 'BVAP', little-endian
DEFAULT_SLOT_BASE = 0x60040000

# Byte offsets within app_header_t (5x uint32, little-endian).
OFF_MAGIC = 0
OFF_IMG_LEN = 8
OFF_CRC32 = 12
HEADER_SIZE = 20

ERASED = 0xFF  # NOR flash erased state; gap fill for the CRC span.

# Intel-HEX record types.
REC_DATA = 0x00
REC_EOF = 0x01
REC_EXT_LINEAR = 0x04


def _checksum(byte_values):
    return (-sum(byte_values)) & 0xFF


def parse_hex(path):
    """Return (sparse dict addr->byte, min_addr, max_addr)."""
    mem = {}
    upper = 0  # extended-linear-address upper 16 bits
    lo = hi = None
    with open(path, "r") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line:
                continue
            if not line.startswith(":"):
                raise ValueError(f"{path}:{lineno}: not an Intel-HEX line: {line!r}")
            data = bytes.fromhex(line[1:])
            count = data[0]
            offset = (data[1] << 8) | data[2]
            rectype = data[3]
            payload = data[4 : 4 + count]
            if _checksum(data[:-1]) != data[-1]:
                raise ValueError(f"{path}:{lineno}: bad record checksum")
            if rectype == REC_DATA:
                addr = (upper << 16) | offset
                for i, b in enumerate(payload):
                    a = addr + i
                    mem[a] = b
                    lo = a if lo is None else min(lo, a)
                    hi = a if hi is None else max(hi, a)
            elif rectype == REC_EXT_LINEAR:
                upper = (payload[0] << 8) | payload[1]
            elif rectype == REC_EOF:
                break
            # Other record types (segment addressing) are unused by Teensy builds.
    if lo is None:
        raise ValueError(f"{path}: no data records")
    return mem, lo, hi


def emit_hex(path, mem, slot_base, img_len):
    """Re-emit the image span as Intel-HEX, 16 bytes/record, with type-04
    extended-linear-address records on each 64 KiB boundary. The image span is
    contiguous (gaps filled with 0xFF) so the output is self-consistent with what
    the device CRCs."""
    lines = []
    cur_upper = None
    end = slot_base + img_len
    addr = slot_base
    while addr < end:
        upper = (addr >> 16) & 0xFFFF
        if upper != cur_upper:
            rec = [0x02, 0x00, 0x00, REC_EXT_LINEAR, (upper >> 8) & 0xFF, upper & 0xFF]
            lines.append(":" + bytes(rec + [_checksum(rec)]).hex().upper())
            cur_upper = upper
        # Don't let a 16-byte chunk straddle a 64 KiB boundary (offset wraps).
        chunk = min(16, end - addr, 0x10000 - (addr & 0xFFFF))
        offset = addr & 0xFFFF
        body = [mem.get(addr + i, ERASED) for i in range(chunk)]
        rec = [chunk, (offset >> 8) & 0xFF, offset & 0xFF, REC_DATA] + body
        lines.append(":" + bytes(rec + [_checksum(rec)]).hex().upper())
        addr += chunk
    lines.append(":00000001FF")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def stamp(in_path, out_path, slot_base):
    mem, lo, hi = parse_hex(in_path)

    if lo != slot_base:
        raise ValueError(
            f"image starts at {lo:#010x}, expected slot base {slot_base:#010x} "
            f"(is this a *_slot build with the right --slot-base?)"
        )

    magic = int.from_bytes(bytes(mem.get(slot_base + OFF_MAGIC + i, ERASED) for i in range(4)), "little")
    if magic != APP_HEADER_MAGIC:
        raise ValueError(
            f"no app_header magic at {slot_base:#010x} (found {magic:#010x}, "
            f"expected {APP_HEADER_MAGIC:#010x})"
        )

    img_len = hi - slot_base + 1
    if img_len < HEADER_SIZE:
        raise ValueError(f"image span {img_len} smaller than header ({HEADER_SIZE})")

    # Build the contiguous span the device will CRC: gaps -> 0xFF, img_len field
    # set, crc field zeroed.
    span = bytearray(ERASED for _ in range(img_len))
    for a, b in mem.items():
        span[a - slot_base] = b
    span[OFF_IMG_LEN : OFF_IMG_LEN + 4] = img_len.to_bytes(4, "little")
    span[OFF_CRC32 : OFF_CRC32 + 4] = b"\x00\x00\x00\x00"

    crc = zlib.crc32(span) & 0xFFFFFFFF

    # Commit img_len + crc back into the sparse image, then re-emit.
    for i, b in enumerate(img_len.to_bytes(4, "little")):
        mem[slot_base + OFF_IMG_LEN + i] = b
    for i, b in enumerate(crc.to_bytes(4, "little")):
        mem[slot_base + OFF_CRC32 + i] = b

    emit_hex(out_path, mem, slot_base, img_len)
    return img_len, crc


def main(argv):
    ap = argparse.ArgumentParser(description="Stamp img_len + crc32 into a slot-A app hex.")
    ap.add_argument("in_hex")
    ap.add_argument("out_hex", nargs="?", help="default: stamp in place")
    ap.add_argument(
        "--slot-base",
        type=lambda s: int(s, 0),
        default=DEFAULT_SLOT_BASE,
        help=f"slot base address (default {DEFAULT_SLOT_BASE:#010x})",
    )
    args = ap.parse_args(argv[1:])
    out = args.out_hex or args.in_hex

    try:
        img_len, crc = stamp(args.in_hex, out, args.slot_base)
    except (OSError, ValueError) as e:
        sys.exit(f"stamp_header: {e}")

    print(
        f"stamped {out}: slot_base={args.slot_base:#010x} "
        f"img_len={img_len} ({img_len / 1024:.1f} KiB) crc32={crc:#010x}"
    )


if __name__ == "__main__":
    main(sys.argv)
