# teensy-ota build helper.
#
# Wraps the bootloader build, the consuming app build, header stamping, hex
# merging, and flashing into single commands. By default it drives the bundled
# example (examples/blink_slotA); point it at your own firmware with:
#
#   make ota APP_DIR=../my-firmware
#
# Common targets:
#   make ota            bootloader + stamped slot-A app  -> build/ota.hex
#   make manufacturing  + GOLDEN slot-B app              -> build/manufacturing.hex
#   make flash          build the OTA image and upload via tycmd
#   make clean          remove build/ and all .pio output

PIO    ?= pio
TYCMD  ?= tycmd
PYTHON ?= python3

APP_DIR   ?= examples/blink_slotA
BUILD_DIR ?= build

# GOLDEN (slot B) link base; must match APP_SLOT_B_BASE in src/app_header.h.
GOLDEN_BASE ?= 0x603C0000

BOOTLOADER_HEX    := bootloader/.pio/build/teensy41/firmware.hex
APP_SLOTA_HEX     := $(APP_DIR)/.pio/build/teensy41_slotA/firmware.hex
APP_SLOTB_HEX     := $(APP_DIR)/.pio/build/teensy41_slotB/firmware.hex

OTA_HEX           := $(BUILD_DIR)/ota.hex
MANUFACTURING_HEX := $(BUILD_DIR)/manufacturing.hex

STAMP := $(PYTHON) scripts/stamp_header.py
MERGE := $(PYTHON) scripts/merge_hex.py

.PHONY: all bootloader app golden ota manufacturing flash flash-manufacturing clean

all: ota

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

# OTA image: bootloader + stamped slot-A app. The Teensy loader erases the whole
# chip, so the two disjoint images are merged and flashed together.
ota: bootloader app | $(BUILD_DIR)
	$(MERGE) $(BOOTLOADER_HEX) $(APP_SLOTA_HEX) $(OTA_HEX)

# Manufacturing image: bootloader + slot-A app + GOLDEN slot-B app.
manufacturing: bootloader app golden | $(BUILD_DIR)
	$(MERGE) $(BOOTLOADER_HEX) $(APP_SLOTA_HEX) $(APP_SLOTB_HEX) $(MANUFACTURING_HEX)

flash: ota
	$(TYCMD) upload $(OTA_HEX)

flash-manufacturing: manufacturing
	$(TYCMD) upload $(MANUFACTURING_HEX)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) bootloader/.pio $(APP_DIR)/.pio
