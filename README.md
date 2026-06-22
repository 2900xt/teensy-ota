# teensy-ota

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Teensy%204.1-blue)
![Framework](https://img.shields.io/badge/framework-Arduino%20%7C%20PlatformIO-orange)
![PRs welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)

A resident bootloader and OTA foundation for Teensy 4.1. A small bootloader sits at
the flash base and boots your application from a higher flash slot, validating a
CRC-stamped header before every jump and rolling back to an immutable **GOLDEN**
recovery image if the slot is blank, corrupt, or fails to come up healthy. The
bootloader is application-agnostic — the same binary boots any firmware built as a
slot image.

- **Safe in-place updates.** The running app stages a firmware image on the SD card;
  the *bootloader* programs it on the next reset. The app never writes app flash.
- **Always recoverable.** A bad image (corrupt transfer, crash loop, hang) falls back
  to the GOLDEN image, which is written once at manufacturing and is physically
  excluded from every runtime flash path.
- **Self-verifying images.** Every slot image carries a CRC-stamped header that both
  the stamper and the bootloader compute identically.

> **Status:** the bootloader, flash layer, CRC/rollback safety net, and the SD-based
> commit path are implemented (see [`OTA_PLAN.md`](OTA_PLAN.md), milestones M0–M4).
> The serial→SD file *transfer* (getting a `.hex` onto the card without USB) is not
> yet built — today you place the staged hex on the card yourself.

---

## Quickstart

### Prerequisites

- [PlatformIO Core](https://platformio.org/) (`pio`) on your `PATH`
- [TyTools](https://github.com/Koromix/tytools) (`tycmd`) on your `PATH`
- `python3`
- A Teensy 4.1 with a FAT32 SD card inserted
- (Linux) optional: `sudo ./scripts/install_udev_rules.sh` for non-root USB access

### Flash the device

The `Makefile` wraps the whole build → stamp → merge → flash pipeline and drives the
bundled [`examples/ota-test-os`](examples/ota-test-os) firmware by default:

```sh
make flash      # build bootloader + slot-A app + GOLDEN, merge, and upload
make test       # same, then open the serial console
```

This writes a single **manufacturing image** (bootloader + slot A + GOLDEN) over USB.
A `tycmd` upload erases the whole chip, so all three regions are flashed together —
that is why `make flash` builds the full image rather than slot A alone.

### Try an OTA update

1. Build a second, distinguishable slot-A image and copy it to the SD card under
   `/ota` (e.g. `/ota/app.hex`):

   ```sh
   pio run -d examples/ota-test-os -e teensy41_slotA \
     -DTEENSY_OTA_APP_VERSION=2
   python3 scripts/stamp_header.py \
     examples/ota-test-os/.pio/build/teensy41_slotA/firmware.hex
   # copy that firmware.hex to the card as /ota/app.hex
   ```

2. Open the serial console (`make test`, or any 115200-baud terminal) and use the
   tester OS:

   ```
   > ls                 # list *.hex in /ota
   > test /ota/app.hex  # no-flash dry run: CRC, header, address range
   > commit /ota/app.hex # arm + reboot; the bootloader flashes slot A
   > info                # confirm running version flipped to 2
   ```

   `commit` arms the update and reboots. The bootloader verifies the file's CRC
   *before* erasing slot A, programs it, re-verifies the stamped image CRC, and boots
   it. If anything fails it rolls back to GOLDEN — your device stays alive.

---

## Adding teensy-ota to your firmware

It's a PlatformIO library. Reference it from `lib_deps` and define the slot build
environments. A real firmware project provides three build layouts:

```ini
lib_deps =
  teensy-ota=symlink://path/to/teensy-ota

; Standalone image, linked at 0x60000000 (normal USB flashing). No app_header.
[env:teensy41]

; OTA-bootable image, slot A (0x60040000). The library emits an app_header at the
; slot base and the linker script pins it there.
[env:teensy41_slotA]
board_build.ldscript = path/to/teensy-ota/ld/app_slotA.ld
build_flags = ${env.build_flags} -D TEENSY_OTA_SLOT_BUILD

; GOLDEN recovery image, slot B (0x603C0000). Same source as slot A, different link
; base. Flashed once at manufacturing, never over OTA.
[env:teensy41_slotB]
board_build.ldscript = path/to/teensy-ota/ld/app_slotB.ld
build_flags = ${env.build_flags} -D TEENSY_OTA_SLOT_BUILD
```

`-D TEENSY_OTA_SLOT_BUILD` is what makes the library emit the `app_header` at the slot
base. Slot A and GOLDEN are the *same firmware* linked at two different bases. See
[`examples/ota-test-os/platformio.ini`](examples/ota-test-os/platformio.ini) for a
working setup.

Point the `Makefile` at your firmware instead of the example with `APP_DIR`:

```sh
make flash APP_DIR=../my-firmware
```

---

## Application API

A slot-A app interacts with teensy-ota through two small headers.

### Rollback participation — [`src/ota_boot.h`](src/ota_boot.h)

The bootloader arms a watchdog and increments a boot-attempt counter before jumping
to slot A. Your app must hold up its end of the safety net:

```c
#include "ota_boot.h"

void loop() {
    ota_boot_keepalive();  // feed the rollback watchdog; required every run, every loop
    // ... once the image has proven itself stable (reached idle, ran N seconds):
    ota_mark_healthy();    // clear the attempt counter so this image isn't rolled back
}
```

- `ota_boot_keepalive()` — feed the watchdog the bootloader armed across the handoff.
  WDOG1 **cannot be disabled once armed**, so call this for the whole run, comfortably
  within the timeout. Keep your loop non-blocking.
- `ota_mark_healthy()` — once stable, clear the boot-attempt counter. Idempotent and
  wear-friendly. An image that crashes, hangs, or never marks itself healthy within
  `OTA_BOOT_MAX_ATTEMPTS` boots is rolled back to GOLDEN. (No-op when running from
  GOLDEN — only a new OTA commit clears a sticky rollback.)

### Staging an update — [`src/ota_update.h`](src/ota_update.h)

The app hands a stamped slot-A `.hex` (already on the SD card) to the bootloader:

```c
#include "ota_update.h"

ota_file_info_t info;
ota_inspect_file("/ota/app.hex", &info);     // no-flash dry run: CRC, header, range
ota_arm_update("/ota/app.hex");              // record pending; commit on next boot
ota_arm_update_and_reboot("/ota/app.hex");   // arm, then reset immediately
ota_disarm_update();                         // cancel a pending update
ota_reboot();                                // clean SYSRESETREQ
```

`ota_arm_update` writes `/ota/pending.txt` (staged path, file CRC32, file length) and
sets the `ota_pending` flag. The bootloader re-verifies the file against that CRC and
length before touching flash, so a corrupt transfer is harmless. `ota_inspect_file`
lets a tool or UI vet an image (parse OK, valid header, every record inside slot A)
before arming. See the header for the full `ota_file_info_t` and result codes.

---

## How an update commits (the in-place path)

`make flash` is a full-chip program over USB. The field-update path is different and
is the whole point of this library:

1. The app stages a stamped slot-A `.hex` on the SD card and arms it
   (`ota_arm_update`), setting a pending flag.
2. On the next reset, the **bootloader** mounts the SD, reads `/ota/pending.txt`,
   and verifies the staged file's CRC/length — *before* slot A is erased.
3. It parses the Intel-HEX, accepting **only** records inside slot A, erases slot A,
   programs it via the RAM-resident flash layer, and re-verifies the stamped image
   CRC.
4. **On success:** clear pending, reset the attempt counter, boot slot A.
   **On any failure:** clear pending (no commit boot-loop), set the target to GOLDEN,
   record the result, and boot GOLDEN.

The commit lives in [`bootloader/src/ota_commit.*`](bootloader/src/ota_commit.cpp);
the Intel-HEX line decoder is shared between bootloader and app via `src/ota_hex.*`.

---

## Reference

### Flash map (Teensy 4.1, 8 MiB @ `0x60000000`)

| Region | Range | Size | Notes |
|---|---|---|---|
| Bootloader | `0x60000000–0x6003FFFF` | 256 KiB | ROM-booted; owns FCB/IVT |
| Slot A (OTA app) | `0x60040000–0x603BFFFF` | 3584 KiB | the only OTA-writable region |
| Slot B (GOLDEN) | `0x603C0000–0x6073FFFF` | 3584 KiB | immutable; never written at runtime |
| Spare | `0x60740000–0x607BFFFF` | 512 KiB | reserved; flash self-test scratch |
| EEPROM emul. | `0x607C0000–0x607FFFFF` | 256 KiB | core-reserved; boot-control state |

GOLDEN immutability is enforced, not just convention: the runtime flash window is two
disjoint ranges (slot A + spare) that exclude slot B entirely, so even a buggy commit
cannot erase the recovery image.

### Make targets

| Target | Does |
|---|---|
| `make` / `make manufacturing` | bootloader + slot A + GOLDEN → `build/manufacturing.hex` |
| `make flash` | build the manufacturing image and upload via `tycmd` |
| `make test` | `flash`, then open the serial console |
| `make compiledb` | regenerate `compile_commands.json` + `.clangd` for IDE/clangd |
| `make clean` | remove `build/` and all `.pio` output |

Overridable variables: `APP_DIR` (firmware to build, default the example), `PIO`,
`TYCMD`, `PYTHON`, `BUILD_DIR`, `GOLDEN_BASE`.

### Repository layout

```
bootloader/   resident bootloader + SD-based OTA commit (ota_commit.*)
src/          the library: app_header, ota_flash, ota_boot(_state), ota_wdog,
              ota_hex, ota_crc32, ota_update — the app-facing + internal API
ld/           linker scripts: bootloader.ld, app_slotA.ld, app_slotB.ld
scripts/      stamp_header.py, merge_hex.py, udev rules + installer
examples/     ota-test-os — interactive serial tester OS (the default consumer)
OTA_PLAN.md   full design rationale, flash map, boot decision, milestones
```

---

## License

MIT — see [LICENSE](LICENSE).
