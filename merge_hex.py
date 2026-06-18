#!/usr/bin/env python3
"""Merge the bootloader and application Intel-HEX images into one flashable file.

The Teensy loader erases the whole chip before programming, so the two images
(bootloader at 0x60000000, app at 0x60040000) cannot be flashed independently
during the handoff prototype. They occupy disjoint address ranges, so a plain
concatenation of their records - dropping all but the final End-Of-File record -
produces a single valid Intel-HEX covering both.

Each input carries its own Extended-Linear-Address (type 04) records, so the
upper-16-bits of every data address are self-contained; no rebasing is needed.

Usage:
  ./merge_hex.py bootloader/.pio/build/teensy41/firmware.hex \\
                 app/.pio/build/teensy41/firmware.hex \\
                 combined.hex
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
    if len(argv) != 4:
        sys.exit(f"usage: {argv[0]} <bootloader.hex> <app.hex> <out.hex>")

    boot_hex, app_hex, out_hex = argv[1], argv[2], argv[3]
    merged = []
    for path in (boot_hex, app_hex):
        for rec in read_records(path):
            # Drop each file's EOF; we emit exactly one at the end.
            if rec.upper().replace(" ", "") == EOF_RECORD:
                continue
            merged.append(rec)
    merged.append(EOF_RECORD)

    with open(out_hex, "w") as f:
        f.write("\n".join(merged) + "\n")

    print(f"wrote {out_hex}: {len(merged)} records ({boot_hex} + {app_hex})")


if __name__ == "__main__":
    main(sys.argv)
