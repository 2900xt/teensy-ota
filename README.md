# teensy-ota

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Teensy%204.1-blue)
![Framework](https://img.shields.io/badge/framework-Arduino%20%7C%20PlatformIO-orange)
![PRs welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)

A resident bootloader and OTA foundation for Teensy 4.1. A small bootloader sits at
the flash base and boots your application from a higher flash slot, validating a
CRC-stamped header before every jump and falling back to an immutable GOLDEN image
if the slot is blank or corrupt. The bootloader is application-agnostic — the same
binary boots any firmware built as a slot image.

## Install

It's a PlatformIO library. Add it to `lib_deps` and define the slot build envs:

```ini
lib_deps =
  teensy-ota=symlink://path/to/teensy-ota

[env:teensy41]            ; standalone image, linked at 0x60000000
[env:teensy41_slotA]      ; OTA-bootable image, slot A (0x60040000)
board_build.ldscript = path/to/teensy-ota/ld/app_slotA.ld
build_flags = ${env.build_flags} -D TEENSY_OTA_SLOT_BUILD

[env:teensy41_slotB]      ; GOLDEN recovery image, slot B (0x603C0000)
board_build.ldscript = path/to/teensy-ota/ld/app_slotB.ld
build_flags = ${env.build_flags} -D TEENSY_OTA_SLOT_BUILD
```

`TEENSY_OTA_SLOT_BUILD` makes the library emit an `app_header` at the slot base; the
linker script pins it there. Slot A and GOLDEN are the same firmware linked at two
bases. A working consumer lives in [`examples/blink_slotA/`](examples/blink_slotA).

## Build & flash

Needs [PlatformIO Core](https://platformio.org/) (`pio`) and
[TyTools](https://github.com/Koromix/tytools) (`tycmd`) on `PATH`. The `Makefile`
wraps the build → stamp → merge → flash pipeline and drives the example by default;
pass `APP_DIR=<path>` for your own firmware.

```sh
make flash                       # build + stamp + merge + upload bootloader test image
make manufacturing               # + GOLDEN slot B -> build/manufacturing.hex
make ota APP_DIR=../my-firmware  # use your firmware instead of the example
```

(The Teensy loader erases the whole chip, so the bootloader and app are merged into
one image and flashed together.)

## In-place updates (OTA commit)

`make flash` is a full-chip program over USB. The field-update path is different:
the running app stages a **stamped slot-A `.hex` on the SD card** and sets a
pending flag, and on the next reset the **bootloader itself** mounts the SD, parses
the hex, and programs slot A in place — the app never writes flash. The staged
file's CRC is checked before slot A is erased, so a corrupt transfer is harmless,
and a failed or interrupted commit falls back to the immutable GOLDEN image. The
commit lives in `bootloader/src/ota_commit.*`; the serial transfer and `OTA_ARM`
front-ends that stage the file land in later milestones (see `OTA_PLAN.md`).

## License

MIT — see [LICENSE](LICENSE).
