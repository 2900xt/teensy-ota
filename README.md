# teensy-ota — Teensy resident bootloader + slot-linked application support

This is a firmware-update foundation for Teensy 4.1 boards: a resident bootloader
at the flash base that boots an application linked at a higher flash slot. The
bootloader is **application-agnostic** — the same binary boots any Teensy firmware
that builds a slot-A image. The proven handoff lives here; the UDP transport, SD
staging, CRC verification, and rollback logic stack on top in later steps.

## How a firmware project opts in

This is a PlatformIO library (`teensy-ota`). A consuming firmware adds it to
`lib_deps` and defines one extra build env:

```ini
lib_deps =
  ...
  teensy-ota=symlink://path/to/teensy-ota

[env:teensy41]            ; standalone image, linked at 0x60000000

[env:teensy41_slotA]      ; OTA-bootable image, linked at slot A (0x60040000)
board_build.ldscript = path/to/teensy-ota/ld/app_slotA.ld
build_flags = ${env.build_flags} -D TEENSY_OTA_SLOT_BUILD

[env:teensy41_slotB]      ; GOLDEN recovery image, linked at slot B (0x603C0000)
board_build.ldscript = path/to/teensy-ota/ld/app_slotB.ld
build_flags = ${env.build_flags} -D TEENSY_OTA_SLOT_BUILD
```

The library supplies `app_header.cpp`/`app_header.h`; each env only points at its
slot linker script and defines `TEENSY_OTA_SLOT_BUILD` so the header is emitted at the
slot base. Everything else (libs, flags, LTO, sources) is inherited from `[env]`,
so the slot-A and GOLDEN (slot-B) builds never diverge in behavior — they are the
same firmware linked at two bases.

A complete, buildable consumer lives in [`examples/blink_slotA/`](examples/blink_slotA) —
a minimal blink app wired up with all three envs. It doubles as the worked example
for the build commands below.

`library.json` sets `libArchive=false` so `app_header.cpp`'s object is linked
directly instead of into a `.a`. The `app_header` symbol is referenced only by the
bootloader (from a fixed flash address, not a link-time symbol), so an archived
member would be dropped before `app_slotA.ld`'s `KEEP(*(.appheader))` ever runs.

## Why two builds of the same firmware

The i.MX RT1062 ROM boots whatever is at `0x60000000` (it needs the FCB/IVT boot
header there). To insert a bootloader, the application must move to a slot and be
reached by a software branch. We keep **both** layouts:

| Build env          | Link base    | Purpose                                              |
|--------------------|--------------|------------------------------------------------------|
| `teensy41`         | `0x60000000` | Standalone USB flashing / recovery. Unchanged dev + CI workflow. |
| `teensy41_slotA`   | `0x60040000` | The OTA-bootable app. Carries an `app_header` at the slot base. |
| `teensy41_slotB`   | `0x603C0000` | GOLDEN: the immutable recovery image, flashed once at manufacturing. Same firmware as slot A. |

## Flash map

| Region      | Address range            | Notes                                                  |
|-------------|--------------------------|--------------------------------------------------------|
| Bootloader  | `0x60000000`–`0x6003FFFF`| 256 KiB, owns FCB/IVT, ROM-booted                      |
| App slot A  | `0x60040000`–`0x603BFFFF`| 3584 KiB, OTA-updatable; `app_header_t` at the base    |
| GOLDEN B    | `0x603C0000`–`0x6073FFFF`| 3584 KiB, immutable recovery; excluded from the OTA writable window |
| Spare       | `0x60740000`–`0x607BFFFF`| 512 KiB, reserved; flash self-test scratch lives here  |
| EEPROM emul.| `0x607C0000`–`0x607FFFFF`| 256 KiB, core-reserved persisted settings              |

The bootloader boots slot A when it is valid and CRC-verified, and falls back to
GOLDEN slot B otherwise (M2). GOLDEN is directly bootable, so recovery needs no
flashing and no SD. `ota_flash`'s writable window is two disjoint ranges (slot A +
spare) that deliberately exclude GOLDEN, so no runtime flash path can erase it.

## Files

- `src/app_header.h` — the `magic`/`entry`/`crc32`/`version` contract shared
  between the bootloader and the app.
- `src/app_header.cpp` — places the header at the slot base (compiled only in a
  `*_slot{A,B}` build, guarded by `TEENSY_OTA_SLOT_BUILD`; the linker pins the header
  to the slot ORIGIN, so the same source serves both slots).
- `ld/app_slotA.ld` — application linker script: `FLASH ORIGIN` at slot A,
  `ENTRY(ResetHandler)`, `.appheader` pinned to the base. (Moving `FLASH ORIGIN`
  is sufficient — Teensy startup copies ITCM/DATA from `LOADADDR(...)` symbols the
  linker computes relative to it, so the unmodified startup copies from the slot.)
- `ld/app_slotB.ld` — identical to `app_slotA.ld` but `FLASH ORIGIN = 0x603C0000`
  (GOLDEN). The only difference between the two builds.
- `ld/bootloader.ld` — bootloader linker script: stock Teensy 4.1 layout with
  FLASH bounded to the first 256 KiB so it can never overrun the app slot.
- `src/ota_crc32.h` / `src/ota_crc32.c` — zlib/IEEE-802.3 CRC32 shared by the host
  stamper and the device. `ota_app_image_crc()` hashes `[slot_base, slot_base+img_len)`
  with the header's `crc32` field treated as zero — the convention the bootloader
  uses to verify an image before jumping.
- `bootloader/` — the resident bootloader (its own PlatformIO project, shared by
  every firmware). Verifies the stamped CRC of slot A (then GOLDEN slot B) before
  the handoff, and falls back to GOLDEN when slot A is blank or corrupt.
- `examples/blink_slotA/` — a minimal consumer firmware (blink) with all three
  build envs wired up. Use it as a template for your own project.
- `scripts/merge_hex.py` — concatenates two or more disjoint Intel-HEX images into
  one flashable file. OTA build = bootloader + slot A; manufacturing = bootloader +
  slot A + GOLDEN slot B (the last argument is always the output).
- `scripts/stamp_header.py` — post-build step that stamps `img_len` + `crc32` into a
  slot app header so the bootloader can verify integrity. Runs in place before the
  merge (idempotent). Standalone: `stamp_header.py <in.hex> [out.hex] [--slot-base 0x...]`
  (`--slot-base 0x603C0000` for a GOLDEN/slot-B hex).
- `Makefile` — wraps build → stamp → merge → flash into single commands
  (`make ota`, `make manufacturing`, `make flash`). Drives the example by default;
  point it at your own firmware with `make ota APP_DIR=../my-firmware`.

## Build & flash

The `Makefile` at the repo root wraps the whole pipeline. By default it builds the
bundled example (`examples/blink_slotA`); pass `APP_DIR=<path>` to point it at your
own firmware. Requires `pio` (PlatformIO Core) and `tycmd` ([TyTools](https://github.com/Koromix/tytools))
on `PATH`.

```sh
# Bootloader + stamped slot-A app, merged into build/ota.hex, then flashed.
make flash

# Or build the OTA image without flashing.
make ota                       # -> build/ota.hex

# Drive your own firmware instead of the example.
make ota APP_DIR=../my-firmware
```

`make ota` runs these steps for you:

```sh
# 1. application, linked at slot A
pio run -d examples/blink_slotA -e teensy41_slotA

# 1b. stamp img_len + crc32 into the slot-A app header (in place, before merge)
python3 scripts/stamp_header.py examples/blink_slotA/.pio/build/teensy41_slotA/firmware.hex

# 2. resident bootloader (built once, reused by every firmware)
pio run -d bootloader

# 3. merge (Teensy loader erases the whole chip, so flash them together)
python3 scripts/merge_hex.py \
  bootloader/.pio/build/teensy41/firmware.hex \
  examples/blink_slotA/.pio/build/teensy41_slotA/firmware.hex \
  build/ota.hex

# 4. flash
tycmd upload build/ota.hex
```

### Manufacturing image (with GOLDEN)

To lay down the immutable GOLDEN recovery image alongside the OTA app, build the
slot-B env too, stamp it at the GOLDEN base, and do a 3-way merge. GOLDEN is
written **once** here and never updated over OTA:

```sh
make manufacturing             # -> build/manufacturing.hex
make flash-manufacturing       # build it and upload via tycmd
```

which is equivalent to:

```sh
pio run -d examples/blink_slotA -e teensy41_slotB
python3 scripts/stamp_header.py \
  examples/blink_slotA/.pio/build/teensy41_slotB/firmware.hex --slot-base 0x603C0000
python3 scripts/merge_hex.py \
  bootloader/.pio/build/teensy41/firmware.hex \
  examples/blink_slotA/.pio/build/teensy41_slotA/firmware.hex \
  examples/blink_slotA/.pio/build/teensy41_slotB/firmware.hex \
  build/manufacturing.hex
tycmd upload build/manufacturing.hex
```

## Success criteria (on hardware)

1. Bootloader prints `BOOTLOADER (0x60000000)` and gives **3 fast LED blinks**.
2. The application boots normally from slot A (its own LCD/LED/UDP behavior). USB
   CDC re-enumerates across the jump — reconnect the monitor.
3. If the app ever faults, the bootloader decodes Teensy's persistent
   `CrashReport` on the next boot (fault type, faulting address, reset reason) —
   keep this diagnostic; it's the unattended-fault visibility the full design
   relies on.

## Handoff gotcha already handled

`jump_to_app()` does `SCB_MPU_CTRL = 0` before branching. The bootloader's
`configure_cache()` leaves the MPU enabled with ITCM read-only + a null-trap at
`0x0`; the app's first startup step copies its code *into* ITCM, which a normal
reset allows because the MPU isn't configured until later. Without disabling the
MPU first, that copy faults (`DACCVIOL @ 0x0`) and the board boot-loops. It also
stops SysTick and masks/clears all NVIC IRQs before the branch.

## Next steps

1. RAM-resident flash write/erase layer (reuse FlasherX `RAMFUNC`s).
2. Bootloader reads a pending image from SD and flashes the inactive slot.
3. Boot-counter / `mark_healthy()` rollback to a GOLDEN image; CRC verify before
   boot.
4. Per-firmware OTA UDP handler that stages the `.hex` to SD and reboots.
