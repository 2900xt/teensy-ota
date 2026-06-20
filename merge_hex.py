#!/usr/bin/env python3
"""Merge two or more Intel-HEX images into one flashable file.

The Teensy loader erases the whole chip before programming, so images that occupy
disjoint address ranges cannot be flashed independently — they must be combined.
Because the ranges are disjoint, a plain concatenation of their records (dropping
all but the final End-Of-File record) produces a single valid Intel-HEX covering
all of them. Each input carries its own Extended-Linear-Address (type 04) records,
so the upper-16-bits of every data address are self-contained; no rebasing needed.

Two uses:
  - OTA build (2 inputs): bootloader (0x60000000) + slot-A app (0x60040000).
  - Manufacturing (3 inputs): bootloader + slot-A app + GOLDEN slot-B app
    (0x603C0000). Flashed once via tycmd; slot A is then OTA-updated while GOLDEN
    stays immutable.

Usage (the LAST argument is always the output; everything before it is an input):
  ./merge_hex.py bootloader.hex app_slotA.hex combined.hex
  ./merge_hex.py bootloader.hex app_slotA.hex app_slotB.hex manufacturing.hex
"""

import sys

EOF_RECORD = ":00000001FF"


def read_records(path):
    with open(path, "r") as f:
        lines = [ln.strip() for ln in f if ln.strip()]
    for ln in lines:
        if not ln.startswith(":"):
            raise ValueError(f"{path}: not an Intel-HEX line: {ln!r}")
    return lines


def main(argv):
    if len(argv) < 4:
        sys.exit(f"usage: {argv[0]} <in1.hex> <in2.hex> [<in3.hex> ...] <out.hex>")

    *in_hexes, out_hex = argv[1:]
    merged = []
    for path in in_hexes:
        for rec in read_records(path):
            # Drop each file's EOF; we emit exactly one at the end.
            if rec.upper().replace(" ", "") == EOF_RECORD:
                continue
            merged.append(rec)
    merged.append(EOF_RECORD)

    with open(out_hex, "w") as f:
        f.write("\n".join(merged) + "\n")

    print(f"wrote {out_hex}: {len(merged)} records ({' + '.join(in_hexes)})")


if __name__ == "__main__":
    main(sys.argv)
