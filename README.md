# Scott Adams Adventures for ESP32-S3

A native Scott Adams `.DAT` adventure-game interpreter for the Sunton
ESP32-8048S043C 4.3" RGB-display dev board. Runs Adventureland,
Pirate Adventure, The Count, and the rest of the original twelve, plus
the Marvel Questprobe series — anything in the standard Scott Adams
`.DAT` format.

> **⚠ Work in progress / scaffold.** This repo was just split off
> from [`ti-extended-basic-esp32`](../ti-extended-basic-esp32) at
> commit `8c100ce`. The hardware bringup, display, BLE keyboard, and
> file I/O are inherited and known-good. The BASIC interpreter has
> been stripped; the current sketch boots into a small command shell
> (`HELP`, `DIR`, `COPY`, `COPYALL`, `BYE`) for managing `.DAT`
> files. The adventure interpreter itself has not been written yet.

## Why a separate repo?

The Scott Adams interpreter shares no game logic with TI Extended
BASIC — only the hardware drivers (display, keyboard, filesystem).
Keeping the projects separate keeps each sketch lean and lets the
adventure runtime evolve without dragging the BASIC interpreter
along (or vice versa).

## Hardware

- **MCU**: ESP32-S3 with 16 MB flash and 8 MB octal PSRAM.
- **Display**: 800×480 RGB panel via the parallel RGB interface,
  rendered at 32×24 TI-style char cells (16×16 px each).
- **Storage**: internal LittleFS (`FLASH.`), micro-SD on SPI
  (`SDCARD.`), and mountable V9T9 `.dsk` images (`DSK1.` … `DSKn.`).
- **Input**: any BLE HID keyboard (tested with ProtoArc L75) via the
  bundled `BleHidHost/` library.

Where to buy the dev board:

- <https://www.amazon.com/dp/B0CLGCMWQ7>
- <https://www.aliexpress.us/item/3256809024564764.html>
- <https://www.aliexpress.us/item/3256808406435888.html>
- <https://www.aliexpress.us/item/3256809832274384.html>

## Game data

Game `.DAT` files are **not redistributed in this repo**. Scott Adams
released the original twelve adventures as freeware decades ago and
they are widely available; place them on the SD card under
`/scottadams/` (e.g. `/scottadams/adv01.dat`) or in LittleFS.

## Prerequisites

### Arduino IDE / arduino-cli

Either works. The build scripts use `arduino-cli` and assume
`COM17` by default (override with `build.bat upload COM7`).

### ESP32 board package

Install the **`esp32:esp32`** platform, version **3.x** (3.3.8+
verified). For arduino-cli:

```bash
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

Or in the Arduino IDE, add this Boards Manager URL:
`https://raw.githubusercontent.com/espressif/arduino-esp32/master/package/package_esp32_index.json`
and install **esp32 by Espressif Systems**.

### Arduino libraries

| Library | Tested version | Notes |
|---|---|---|
| **GFX Library for Arduino** (`Arduino_GFX`) | 1.6.5 | Provides `Arduino_RGB_Display` and the RGB-panel data-bus; the project also vendors a forked panel (`rgb_db.h/.cpp`) that adds double buffering. |

Built-in (ship with the ESP32 core, no separate install):
`SD`, `SPI`, `LittleFS`, `FS`, `BLE`, `Preferences`, `Wire`.

The repo also vendors **`BleHidHost/`** (BLE HID host for keyboards
and gamepads) at the top level — it's picked up automatically via
the `--libraries` flag in `build.bat`, no manual install needed.

### Board configuration (FQBN)

The build scripts use:

```
esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default
```

If using the Arduino IDE: select **ESP32S3 Dev Module**, set
**PSRAM = OPI PSRAM**, **Flash Size = 16MB**, **Partition Scheme =
3M App / 9M FATFS**, **USB CDC On Boot = Disabled** (the default).

## Building & flashing

```bash
build.bat                # compile + upload
build.bat compile        # compile only
build.bat upload         # upload only (auto-kills any open serial monitor)
build.bat monitor        # serial monitor on COM17
build.bat all COM7       # everything, on a non-default port
```

## License

GPL-2.0-or-later — see `LICENSE` for the canonical GNU text. This
matches the license of ScottFree and the broader Scott Adams
interpreter ecosystem (garglk, etc.), so we can borrow freely from
those references without compatibility friction.

The interpreter (`scott_dat.h`, `scott_play.h`, `scott_exec.h`) was
written from the openly-published `.DAT` format spec, not transcribed
from any specific GPL implementation. It is original work, now
released under GPL-2.0-or-later as part of this project.
