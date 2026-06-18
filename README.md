# VigilOta — shared Teensy resident bootloader + slot-linked application support

This is the shared firmware-update foundation for every Vigil Teensy board: a
resident bootloader at the flash base that boots an application linked at a higher
flash slot. The bootloader is **application-agnostic** — the same binary boots the
top-panel firmware, the GCS firmware, or any other Teensy firmware that builds a
slot-A image. The proven handoff lives here; the UDP transport, SD staging, CRC
verification, and rollback logic stack on top in later steps.

## How a firmware project opts in

`common/ota` is a PlatformIO library (`VigilOta`). A consuming firmware adds it to
`lib_deps` and defines one extra build env:

```ini
lib_deps =
  ...
  VigilOta=symlink://../../common/ota

[env:teensy41]            ; default GOLDEN image, linked at 0x60000000

[env:teensy41_slotA]      ; OTA-bootable image, linked at slot A (0x60040000)
board_build.ldscript = ../../common/ota/ld/app_slotA.ld
build_flags = ${env.build_flags} -D BV_OTA_SLOT_BUILD
```

The library supplies `app_header.cpp`/`app_header.h`; the env only points at the
slot linker script and defines `BV_OTA_SLOT_BUILD` so the header is emitted at the
slot base. Everything else (libs, flags, LTO, sources) is inherited from `[env]`,
so the GOLDEN and slot-A builds never diverge in behavior.

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
| `teensy41`         | `0x60000000` | Standalone USB flashing, recovery, manufacturing / GOLDEN. Unchanged dev + CI workflow. |
| `teensy41_slotA`   | `0x60040000` | The OTA-bootable app. Carries an `app_header` at the slot base. |

## Flash map

| Region      | Address range            | Notes                                  |
|-------------|--------------------------|----------------------------------------|
| Bootloader  | `0x60000000`–`0x6003FFFF`| 256 KiB, owns FCB/IVT, ROM-booted      |
| App slot A  | `0x60040000`–…           | `app_header_t` pinned to the slot base |

## Files

- `src/app_header.h` — the `magic`/`entry`/`crc32`/`version` contract shared
  between the bootloader and the app.
- `src/app_header.cpp` — places the header at the slot base (compiled only in a
  `*_slotA` build, guarded by `BV_OTA_SLOT_BUILD`).
- `ld/app_slotA.ld` — application linker script: `FLASH ORIGIN` at the slot,
  `ENTRY(ResetHandler)`, `.appheader` pinned to the base. (Moving `FLASH ORIGIN`
  is sufficient — Teensy startup copies ITCM/DATA from `LOADADDR(...)` symbols the
  linker computes relative to it, so the unmodified startup copies from the slot.)
- `ld/bootloader.ld` — bootloader linker script: stock Teensy 4.1 layout with
  FLASH bounded to the first 256 KiB so it can never overrun the app slot.
- `bootloader/` — the resident bootloader (its own PlatformIO project, shared by
  every firmware).
- `merge_hex.py` — combines bootloader + app images into one flashable file.

## Build & flash

The normal path is `just tpf` (top-panel) — the recipe delegates to
`scripts/firmware-platformio-build.sh --ota`, which builds the slot-A app, builds
the shared bootloader, merges them, and flashes the combined image via `tycmd`.
The equivalent manual steps for a project at `core/<fw>`:

```sh
# 1. application, linked at slot A
pio run -d core/<fw> -e teensy41_slotA

# 2. shared resident bootloader (built once, reused by every firmware)
pio run -d common/ota/bootloader

# 3. merge (Teensy loader erases the whole chip, so flash them together)
python3 common/ota/merge_hex.py \
  common/ota/bootloader/.pio/build/teensy41/firmware.hex \
  core/<fw>/.pio/build/teensy41_slotA/firmware.hex \
  core/<fw>/.pio/combined.hex

# 4. flash
tycmd upload core/<fw>/.pio/combined.hex
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
4. Per-firmware OTA UDP handler that stages the `.hex` to SD and reboots (mirrors
   `core/top-panel-firmware/src/communication/diagHandler.cpp`).
