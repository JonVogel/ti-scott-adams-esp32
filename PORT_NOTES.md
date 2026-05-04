# Port Notes — ESP32-8048S043 (Sunton 4.3" 800×480 RGB)

Migrating from the ESP32-S3-USB-OTG (ST7789 SPI, 240×240) to the Sunton
ESP32-8048S043-C (ILI-family 16-bit RGB parallel, 800×480, capacitive
touch). The frozen OTG version lives in `ti-basic-otg/`.

## Target board

- MCU: ESP32-S3, 16 MB flash, `qio_opi` + PSRAM enabled
- Display: 4.3" IPS, **800×480**, 16-bit RGB565 parallel
- Touch: GT911 capacitive over I²C
- Partitions: `default_16MB.csv`
- CPU: 240 MHz, flash 80 MHz

## Display pin map

Reference: [clumsyCoder00/Sunton-ESP32-8048S043](https://github.com/clumsyCoder00/Sunton-ESP32-8048S043)

| Signal  | GPIO | Notes                                   |
|---------|------|-----------------------------------------|
| DE      | 40   |                                         |
| VSYNC   | 41   |                                         |
| HSYNC   | 39   |                                         |
| PCLK    | 42   |                                         |
| R0..R4  | 45, 48, 47, 21, 14 | 5-bit red                   |
| G0..G5  | 5, 6, 7, 15, 16, 4 | 6-bit green                 |
| B0..B4  | 8, 3, 46, 9, 1     | 5-bit blue                  |
| Backlight | 2  | Active high (PWM-capable)              |

### Timing (default for Arduino_GFX constructor)

```
hsync_polarity    = 0
hsync_front_porch = 8
hsync_pulse_width = 4
hsync_back_porch  = 8
vsync_polarity    = 0
vsync_front_porch = 8
vsync_pulse_width = 4
vsync_back_porch  = 8
```

## Touch (confirmed from vendor demo)

| Signal | GPIO | Notes                                      |
|--------|------|--------------------------------------------|
| SDA    | 19   | Native USB D- (conflicts — see below)      |
| SCL    | 20   | Native USB D+ (conflicts — see below)      |
| INT    | none | Not wired (-1)                             |
| RST    | 38   |                                            |

Controller: **GT911**. Library: `TAMC_GT911` (github.com/TAMCTec/gt911-arduino).
`Wire.begin(19, 20)` then `ts.begin()`.

## Serial / USB

- **Native USB is unavailable** — GPIO 19/20 are claimed by GT911 touch I²C.
- Serial goes through the onboard CP2102 UART bridge on **GPIO 43 (TXD0)
  / 44 (RXD0)**. This is the default Arduino Serial on the ESP32-S3
  Arduino core.
- `build.bat` FQBN needs to target **USBCDCOnBoot=disabled** and let
  the default UART0 handle serial:
  `esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,...`

## SD card (TF slot, SPI)

| Signal | GPIO | Notes                      |
|--------|------|----------------------------|
| CS     | 10   | TF_CS                      |
| MOSI   | 11   | shared with display? no    |
| SCK    | 12   | TF_CLK                     |
| MISO   | 13   |                            |

Pull-ups (10 kΩ) on CS/MISO/MOSI to 3.3 V, and CD (card-detect) pin is
not wired on this board. Use the standard Arduino `SD.h` with custom
SPI pins: `SPI.begin(12, 13, 11, 10); SD.begin(10);`.

## Buttons

- **BOOT**: GPIO 0 (same as OTG) — `BleHidHost` already uses this for
  the BLE pairing-mode trigger, so no code change needed.
- **RESET**: hardware reset, not exposed to software.

## Unknown / TBD

- SD card CS pin (check schematic)
- Any onboard buzzer / speaker (board photo doesn't show one)
- Free GPIOs for status LED (GPIO 38 = touch RST; "RGB@IO38" in the
  generic pinout is blocked — no onboard status LED on this variant)

## Migration strategy

### Layer 1 — replace the display driver (biggest change)

`ti-basic/ti-basic.ino` uses `Adafruit_ST7789` over SPI. Replace with one
of:

1. **Arduino_GFX** (`moononournation/Arduino_GFX`) — mirrors the same
   Adafruit_GFX API (`fillRect`, `drawPixel`, `drawRGBBitmap`), so most
   of `drawCell`, `fillBackground`, `redrawScreen`, etc. stays the same.
   Constructor above works out of the box. **Lowest-effort path.**
2. **LVGL** — more library overhead, but gives us widgets / touch for
   free if we want to build on-screen keys later.
3. **Raw `esp_lcd_panel_rgb`** — full framebuffer access, fastest, but
   more plumbing to write ourselves.

Recommend starting with **Arduino_GFX** so the existing draw code keeps
working, then revisit for sprite performance.

### Layer 2 — rescale the character grid

OTG uses 28×24 chars at 8×8 pixels. 800×480 gives us plenty of options:

| cols × rows | char size | screen used | border   |
|-------------|-----------|-------------|----------|
| 32 × 24     | 16×16     | 512 × 384   | 144/48   |
| 40 × 30     | 16×16     | 640 × 480   | 80/0     |
| 32 × 24     | 24×24     | 768 × 576 ❌| —        |
| 40 × 24     | 20×20     | 800 × 480 ✅| 0/0      |

Cleanest "authentic TI" is **32×24 at 16×16** — scaled-up chars with
room for a border. Scaling is an 8→16 doubling of every bitmap bit
(easy to do in `drawCell` with a tight loop).

Constants to change: `COLS`, `ROWS`, `CHAR_W`, `CHAR_H`,
`DISPLAY_X_OFFSET`, `DISPLAY_Y_OFFSET`, `SCREEN_W`, `SCREEN_H`.

### Layer 3 — things that don't change

These are all pure logic and need no porting:

- `token_parser.h`, `exec_manager.h`, `expr_parser.h`, `var_table.h`,
  `tp_types.h`, `line_editor.h`
- `ble_keyboard.h` (BLE is on-chip, same on S3)
- `BleHidHost` library
- LittleFS save/load
- Boot-screen logic (just rescale coordinates)

### Layer 4 — new capabilities unlocked

Things the OTG couldn't realistically do that the new board can:

- **Sprites** — RGB parallel is fast enough for full-screen redraws at
  30+ fps, so TMS9918-style sprite overlay becomes viable.
- **Touch input** — could add on-screen TI keyboard for prompts.
- **Larger LittleFS / SD card** — bigger programs.

## Build config changes

`build.bat` needs updates:

```bat
set FQBN=esp32:esp32:esp32s3:PartitionScheme=default_16MB,PSRAM=opi,FlashSize=16M,CPUFreq=240
```

Plus `--libraries` pointing at `..` for BleHidHost, and likely a new
`platform.txt` entry for Arduino_GFX when we bring it in.

Partition: the stock `default_16MB` works (3 MB app × 2 + 7 MB SPIFFS);
no need for our custom `partitions.csv` now.
