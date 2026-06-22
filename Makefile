# teensy-ota build helper.
#
# Wraps the bootloader build, the consuming app build, header stamping, hex
# merging, and flashing into single commands. By default it drives the bundled
# example (examples/ota-test-os); point it at your own firmware with:
#
#   make ota APP_DIR=../my-firmware
#
# Common targets:
#   make manufacturing  bootloader + slot-A app + GOLDEN slot-B  -> build/manufacturing.hex
#   make flash          build the manufacturing image and upload via tycmd
#   make test           flash, then open the serial console
#   make clean          remove build/ and all .pio output
#
# Note: a tycmd upload is a full-chip program, so it always writes all three
# regions (and erases everything else). That is why flash builds the full
# manufacturing image, not a slot-A-only image — a partial upload would wipe
# GOLDEN. In-place slot-A-only updates are the job of the OTA commit path, not
# of make flash.

PIO    ?= pio
TYCMD  ?= tycmd
PYTHON ?= python3

APP_DIR   ?= examples/ota-test-os
BUILD_DIR ?= build

# GOLDEN (slot B) link base; must match APP_SLOT_B_BASE in src/app_header.h.
GOLDEN_BASE ?= 0x603C0000

BOOTLOADER_HEX    := bootloader/.pio/build/teensy41/firmware.hex
APP_SLOTA_HEX     := $(APP_DIR)/.pio/build/teensy41_slotA/firmware.hex
APP_SLOTB_HEX     := $(APP_DIR)/.pio/build/teensy41_slotB/firmware.hex

MANUFACTURING_HEX := $(BUILD_DIR)/manufacturing.hex

STAMP := $(PYTHON) scripts/stamp_header.py
MERGE := $(PYTHON) scripts/merge_hex.py

.PHONY: all bootloader app golden manufacturing flash test compiledb clean

all: manufacturing

# Resident bootloader (ROM-booted image at 0x60000000).
bootloader:
	$(PIO) run -d bootloader

# Slot-A app, then stamp img_len + crc32 into its header (in place, pre-merge).
app:
	$(PIO) run -d $(APP_DIR) -e teensy41_slotA
	$(STAMP) $(APP_SLOTA_HEX)

# GOLDEN slot-B app, stamped at the GOLDEN base.
golden:
	$(PIO) run -d $(APP_DIR) -e teensy41_slotB
	$(STAMP) $(APP_SLOTB_HEX) --slot-base $(GOLDEN_BASE)

# Manufacturing image: bootloader + slot-A app + GOLDEN slot-B app.
manufacturing: bootloader app golden | $(BUILD_DIR)
	$(MERGE) $(BOOTLOADER_HEX) $(APP_SLOTA_HEX) $(APP_SLOTB_HEX) $(MANUFACTURING_HEX)

flash: manufacturing
	$(TYCMD) upload $(MANUFACTURING_HEX)

test: flash
	minicom -D /dev/ttyACM0 -b 115200

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Regenerate bootloader/compile_commands.json and .clangd for IDE/clangd.
compiledb:
	$(PIO) run -t compiledb -d bootloader
	@printf '%s\n' \
	  'CompileFlags:' \
	  '  CompilationDatabase: bootloader' \
	  '  Add:' \
	  '    - --target=arm-none-eabi' \
	  "    - --gcc-toolchain=$$HOME/.platformio/packages/toolchain-gccarmnoneeabi-teensy" \
	  "    - -isystem$$HOME/.platformio/packages/toolchain-gccarmnoneeabi-teensy/arm-none-eabi/include" \
	  > .clangd

clean:
	rm -rf $(BUILD_DIR) bootloader/.pio $(APP_DIR)/.pio
