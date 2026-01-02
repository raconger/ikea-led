#!/bin/bash
# Manual upload script to bypass platformio's reset issues
# Run this after pressing RST button

~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32s3 \
  --port /dev/ttyACM0 \
  --baud 460800 \
  --before no_reset \
  --after hard_reset \
  write_flash -z \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 16MB \
  0x0 .pio/build/nano_esp32/bootloader.bin \
  0x8000 .pio/build/nano_esp32/partitions.bin \
  0x10000 .pio/build/nano_esp32/firmware.bin
