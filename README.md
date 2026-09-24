# CLANKER Pocket BETA

Clean-room firmware for the Waveshare ESP32-S3 e-Paper 3.97" board.

This repository is intentionally **not** a continuation of the FolloUp application architecture. The previous CLANKER Pocket repository is reference material only. Hardware-specific driver code may be reused where it is useful, but behavior is rebuilt from simple, independently testable primitives.

## Current milestone: Phase 1

The first firmware proves only:

- AXP2101 power rails
- SSD1677 800x480 e-paper initialization and refresh
- BOOT on GPIO0
- radial Up on GPIO4
- radial Select on GPIO5
- radial Down on GPIO6

The screen boots into a dedicated hardware-test page. Every button press must update the button name and press counter directly on the e-paper display.

There is deliberately **no Wi-Fi, no microphone, no input dispatcher, no recording session service, no overlays, no navigation framework, and no inherited application state machine** in Phase 1.

See `docs/BRINGUP.md` for the gated build plan and `docs/HARDWARE.md` for the hardware contract.

## Build

CI builds with ESP-IDF 5.5.4 and produces:

- `clanker-pocket-beta-full.bin` — complete flash image for address `0x0`
- `clanker-pocket-beta-app.bin` — app image only

For the first BETA flash, use the **full image at 0x0** so the bootloader and new single-factory partition table match this repo.

## Rule

A layer does not graduate because it compiles. It graduates only after it is visibly proven on the physical device.
