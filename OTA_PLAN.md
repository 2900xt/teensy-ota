# Vigil OTA — design & implementation plan

Status: **plan**. The only code that exists today is the handoff bootloader and the
RAM-resident flash layer (`ota_flash`, with bench self-test). Everything below is
the agreed design for the rest.

## Goal

Update a Teensy firmware in the field without USB/tycmd, with a safety net that
makes a bad update non-fatal. Delivery is a **generic serial → SD file transfer**
(useful beyond OTA); OTA is just one consumer of it. Recovery is a **permanent,
directly-bootable GOLDEN image** in a second flash slot.

## Design decisions (locked)

- **Single active slot + GOLDEN fallback.** Slot A is the only OTA-writable app
  slot. Slot B holds an immutable factory GOLDEN image, **directly bootable** — on
  any failure the bootloader jumps to GOLDEN with zero flashing and no SD
  dependency on the recovery path.
- **GOLDEN is never updatable at runtime.** It is written once at manufacturing
  (external programmer / tycmd) and is hard-protected from every runtime flash
  path — the `ota_flash` writable window itself excludes slot B (see below), so no
  bug in the commit path can erase it. There is no OTA path that touches GOLDEN.
- **Transport = generic SD file R/W over the serial console**, not a bespoke UDP
  OTA protocol. Firmware is staged as a normal file on SD, then "armed."
- **Binary chunks** for the file body: a text header line + exactly `len` raw
  bytes. Serial-terminal only (not exposed over the read-only UDP diag console).
- **Commit happens in the bootloader**, never in the running app. The app only
  stages a file and sets a flag.

## Flash map (Teensy 4.1, 8 MiB @ 0x60000000)

| Region | Range | Size | Notes |
|---|---|---|---|
| Bootloader | `0x60000000–0x6003FFFF` | 256 KiB | ROM-booted, owns FCB/IVT. SdFat fits comfortably (see risks). |
| Slot A (OTA app) | `0x60040000–0x603BFFFF` | 3584 KiB | `app_slotA.ld`; the only OTA-writable region |
| Slot B (GOLDEN) | `0x603C0000–0x6073FFFF` | 3584 KiB | `app_slotB.ld` (new); immutable, never written at runtime |
| Spare | `0x60740000–0x607BFFFF` | 512 KiB | reserved; flash self-test scratch lives here |
| EEPROM emul. | `0x607C0000–0x607FFFFF` | 256 KiB | core-reserved (`FLASH_SECTORS=63`); boot-control state lives here |

**`ota_flash` writable window excludes GOLDEN.** To make slot B physically
unreachable from any runtime flash path, the guard is two disjoint ranges, not one:
slot A `[0x60040000,0x603C0000)` and spare `[0x60740000,0x607C0000)`. Slot B and the
bootloader and EEPROM are all outside it. The bootloader commit path additionally
range-checks to slot A only; the self-test only ever touches the spare range.

> **Required tweak (done in M2):** the self-test default scratch moved from
> `0x603C0000` (now slot B) into the spare range (`0x60740000`), so the bench
> test can never erase GOLDEN.

## Components & status

| Component | Where | Status |
|---|---|---|
| Handoff bootloader (jump to slot A) | `common/ota/bootloader` | done |
| RAM-resident flash erase/write/verify | `common/ota/src/ota_flash.*` | done (needs hardware bench pass) |
| Flash bench self-test | `common/ota/src/ota_flash_selftest.*` | done; scratch now `0x60740000` (M2) |
| CRC32 + header stamping | `stamp_header.py` + `ota_crc32` + bootloader verify | **done (M1)** |
| GOLDEN slot B (link env + boot select) | `app_slotB.ld`, `teensy41_slotB`, bootloader | **done (M2)** |
| Boot-counter / `mark_healthy` / watchdog / rollback | EEPROM state + bootloader + app API | **todo (M3)** |
| Bootloader SD mount + hex parse + commit | bootloader | **todo (M4)** |
| Serial SD file transfer commands | `serialTerminal` dispatch + host script | **todo (M5)** |
| `OTA_ARM`/`OTA_STATUS`/`OTA_ABORT`/`REBOOT` | `serialTerminal` dispatch | **todo (M6)** |

## Boot-control state (emulated EEPROM)

A small CRC-protected struct (written only when a field changes, to limit wear):

```
magic, version
boot_target        // A | GOLDEN (sticky GOLDEN until a new OTA arrives)
slotA_attempts     // ++ by bootloader before jumping to A; cleared by app mark_healthy
slotA_healthy      // set by app once stable
ota_pending        // an armed update is staged on SD
last_commit_result // ok | parse_err | flash_err | verify_err | sd_err
crc32              // over the struct
```

## Boot decision (bootloader, each reset)

1. **Apply pending update** if `ota_pending`:
   mount SD → read `/ota/pending.txt` (path, crc32, len) → parse the staged Intel
   HEX, accepting **only** records inside slot A `[0x60040000,0x603C0000)` (abort
   on any out-of-range record) → erase slot A span → program via `ota_flash` →
   verify CRC. On success: clear `ota_pending`, reset `slotA_attempts`,
   `boot_target=A`. On any failure: clear `ota_pending` (avoid commit boot-loop),
   set `boot_target=GOLDEN`, record `last_commit_result`.
2. **Select slot:** boot A iff (slot A header valid + CRC ok) **and**
   `slotA_attempts < MAX_ATTEMPTS` **and** `boot_target==A`; else boot GOLDEN.
3. **Arm rollback:** before jumping to A, `slotA_attempts++` (persist), arm the
   RT1062 watchdog. The app clears the counter via `ota_mark_healthy()` once it has
   run stably (e.g. reached IDLE / N seconds). A crash (Teensy fault handler
   reboots after storing CrashReport) or hang (watchdog reset) leaves the counter
   un-cleared → after `MAX_ATTEMPTS`, GOLDEN. GOLDEN never increments/rolls back.

## Image format & CRC stamping (M1)

- Keep the existing `app_header` (`magic/entry/img_len/crc32/version`) at the slot
  base. A post-build step stamps `img_len` (image byte span from slot base) and
  `crc32` = CRC32 over `[slot_base, slot_base+img_len)` **with the `crc32` field
  treated as 0** during computation. Bootloader recomputes identically and
  compares before any jump.
- Tooling: new `stamp_header.py` (or extend `merge_hex.py`), wired into the OTA
  build path so `teensy41_slotA` / `teensy41_slotB` hexes are stamped before merge
  / staging. The staged SD hex is therefore self-verifying; the sidecar CRC is a
  redundant outer check.
- Staged artifact = the **app-only slot-A hex** (`teensy41_slotA/firmware.hex`),
  not the combined image — we never rewrite the bootloader over OTA. (Raw `.bin`
  is a possible later optimization; `.hex` keeps explicit addresses for range
  checks.)

## GOLDEN slot B (M2)

- `app_slotB.ld`: identical to `app_slotA.ld` but `FLASH ORIGIN = 0x603C0000`.
- `teensy41_slotB` env: same source, `BV_OTA_SLOT_BUILD`, slot-B ldscript.
- Manufacturing combined image = bootloader + slot-A image + slot-B image (extend
  the merge to 3 inputs). Flash once via tycmd. Slot A is then OTA-updated; **slot B
  is immutable** — written only here, never by any runtime path.
- Immutability is enforced, not just convention: the `ota_flash` writable window
  excludes `[0x603C0000,0x60740000)`, so even a buggy commit can't erase GOLDEN.
- Also move the self-test scratch out of slot B (now `0x603C0000`) into the spare
  range (e.g. `0x60740000`).

## Serial SD file-transfer protocol (M5, binary chunks)

New `serialTerminal` dispatch commands (state-changing → serial only, not UDP diag):

- `SD_WRITE <path> <offset> <len> <crc32>` → then exactly `<len>` raw bytes; device
  seeks `offset` in `path` (`O_CREAT|O_WRITE`), writes, checks crc, replies
  `OK <offset> <len>` or `ERR <reason>`. Caller advances/retries per chunk.
- `SD_READ <path> <offset> <len>` → `DATA <len> <crc32>` + raw bytes.
- `SD_STAT <path>` (size/exists), `SD_RM <path>`, `SD_MKDIR <path>`.
- Chunk body bypasses line editing/echo: header parse switches the terminal into a
  bounded raw-read (read exactly `len` bytes, timeout → `ERR`). Chunk size bounded
  by a static buffer (e.g. 4–8 KiB).
- Host helper under `tools/` (mirrors `tools/diag-console/`): drives `SD_WRITE` in a
  loop, waits for `OK`, retries on `ERR`/timeout.

## OTA commands (M6)

- `OTA_ARM <path> <crc32> <len>` → verify SD file matches len/crc → write pending
  marker (EEPROM flag + `/ota/pending.txt`). 
- `OTA_STATUS` → pending state, slot A/B headers (magic/ver/crc), boot counter,
  healthy flag, `last_commit_result`.
- `OTA_ABORT` → clear pending. `REBOOT` → SCB AIRCR system reset.

End-to-end: `SD_WRITE`(slotA hex) → `OTA_ARM` → `REBOOT` → bootloader commits →
boots slot A → app `ota_mark_healthy()`.

## Milestones (build the safety net before the auto-writer, delivery last)

- **M0 (done):** `ota_flash` + self-test. → *bench-verify on hardware.*
- **M1 (done):** CRC + header stamping; bootloader verifies slot A CRC before boot.
  Purely additive, makes the current boot safer. `stamp_header.py` fills `img_len`
  + `crc32` into the slot-A hex post-build (wired into the OTA build before merge);
  `ota_crc32` (zlib-compatible, shared host/device) backs both the stamper and the
  bootloader's pre-jump verify. *Needs a hardware boot confirming valid-CRC jump
  and corrupt-CRC refusal.*
- **M2 (done):** GOLDEN slot B — `app_slotB.ld` (slot-A script with FLASH ORIGIN
  `0x603C0000`), `teensy41_slotB` env (top-panel + GCS), dual-slot boot selection
  in the bootloader (good slot A, else good GOLDEN, else stay), `ota_flash`
  writable window split into two disjoint ranges that exclude GOLDEN, self-test
  scratch moved to the spare range, and `merge_hex.py` extended to a variadic
  (3-way) manufacturing merge. *Needs a hardware bench: blank/corrupt slot A →
  boots GOLDEN; valid slot A → boots A.*
- **M3:** EEPROM boot-control: counter + `ota_mark_healthy()` + watchdog + rollback.
  Bench: app that deliberately faults → rolls back to GOLDEN after `MAX_ATTEMPTS`.
- **M4:** Bootloader SD mount (SdFat, FAT32-only config) + Intel-HEX parse +
  commit slot A via `ota_flash`. Bench: hand-place a hex + set pending → reboot →
  commits + verifies.
- **M5:** Serial SD file transfer commands + raw chunk mode + host script. Bench:
  push a file, read back, CRC match.
- **M6:** OTA arm/status/abort/reboot; full end-to-end OTA.
- **M7:** Hardening — failure injection (power loss mid-commit, corrupt chunk, bad
  CRC, boot-loop), docs, host CLI polish.

## Risks / open questions

- **SdFat in the 256 KiB bootloader — resolved.** SdFat is small: ~9.8 KB flash /
  ~875 B SRAM (FAT16/FAT32 only), ~13.8 KB (exFAT), ~19.3 KB (full). The bootloader
  is ~40 KB today, so even full support stays well under 256 KiB. Use the
  **FAT32-only** config (smallest) in the bootloader — confirm the deployed SD
  cards are FAT32 (the apps use SdFat already; check their config). No need to grow
  the bootloader region or move slot A.
- **Slot size vs app size.** Confirm the real slot-A/B image fits well under
  3584 KiB.
- **Commit interruptibility.** Power loss mid-commit leaves slot A partially
  written → CRC fails next boot → GOLDEN (safe). The staged hex + pending marker
  survive on SD, so a retry is possible; decide auto-retry vs require re-arm.
- **Watchdog vs app's own watchdog usage** — reconcile timeouts/ownership.
- **EEPROM wear** — write boot-control only on change; struct is CRC-protected for
  torn-write detection.
- **Raw chunk mode vs line terminal** — echo/history/the UDP-diag reuse must not
  see the binary body; keep binary mode strictly serial.
- **GOLDEN immutability — decided.** GOLDEN is never updatable at runtime; there is
  no `OTA_ARM_GOLDEN`. It changes only via a full re-manufacture (external
  programmer / tycmd). This keeps the safety net truly immutable.
