// Double-buffered RGB panel + minimal display class for ESP32-S3.
//
// Why this exists: stock Arduino_GFX hardcodes num_fbs=1 / double_fb=
// false, so writes land in the same framebuffer the panel is actively
// scanning — visible tearing during sprite motion. With 2 FBs, the CPU
// edits "back" while the panel scans "front", and commitFrame() flips
// the roles at vsync. End result: tear-free sprite motion / VDP-style.
//
// Scope is intentionally narrow: only the GFX methods ti-basic.ino
// actually uses (writePixel / fillRect / draw16bitRGBBitmap /
// fillScreen / setRotation / begin). If upstream Arduino_GFX gains
// native DB support we can delete this file.

#pragma once

#include <Arduino.h>

#if defined(ESP32) && CONFIG_IDF_TARGET_ESP32S3

#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp32s3/rom/cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class Arduino_ESP32RGBPanelDB
{
public:
  Arduino_ESP32RGBPanelDB(
      int8_t de, int8_t vsync, int8_t hsync, int8_t pclk,
      int8_t r0, int8_t r1, int8_t r2, int8_t r3, int8_t r4,
      int8_t g0, int8_t g1, int8_t g2, int8_t g3, int8_t g4, int8_t g5,
      int8_t b0, int8_t b1, int8_t b2, int8_t b3, int8_t b4,
      uint16_t hsync_polarity, uint16_t hsync_front_porch,
      uint16_t hsync_pulse_width, uint16_t hsync_back_porch,
      uint16_t vsync_polarity, uint16_t vsync_front_porch,
      uint16_t vsync_pulse_width, uint16_t vsync_back_porch,
      uint16_t pclk_active_neg = 0, int32_t prefer_speed = -1,
      bool useBigEndian = false,
      uint16_t de_idle_high = 0, uint16_t pclk_idle_high = 0,
      size_t bounce_buffer_size_px = 0);

  bool begin(int16_t w, int16_t h);

  uint16_t* backBuffer()  { return _backFb; }
  uint16_t* frontBuffer() { return _frontFb; }
  int16_t   width()  const { return _w; }
  int16_t   height() const { return _h; }

  // Trigger swap on next vsync. After the call, backBuffer() points
  // at the OLD front (now scheduled to become back). Returns true.
  bool commitFrame();

private:
  int8_t _de, _vsync, _hsync, _pclk;
  int8_t _r0, _r1, _r2, _r3, _r4;
  int8_t _g0, _g1, _g2, _g3, _g4, _g5;
  int8_t _b0, _b1, _b2, _b3, _b4;
  uint16_t _hsync_polarity, _hsync_front_porch;
  uint16_t _hsync_pulse_width, _hsync_back_porch;
  uint16_t _vsync_polarity, _vsync_front_porch;
  uint16_t _vsync_pulse_width, _vsync_back_porch;
  uint16_t _pclk_active_neg;
  int32_t  _prefer_speed;
  bool     _useBigEndian;
  uint16_t _de_idle_high, _pclk_idle_high;
  size_t   _bounce_buffer_size_px;

  esp_lcd_panel_handle_t _panel_handle = nullptr;
  uint16_t* _fb1 = nullptr;
  uint16_t* _fb2 = nullptr;
  uint16_t* _backFb  = nullptr;
  uint16_t* _frontFb = nullptr;
  int16_t   _w = 0, _h = 0;
};


// Minimal display class. Caches the panel's current back buffer and
// implements just the drawing primitives ti-basic.ino uses.
// Coordinate transform handles rotation 0 (native) and 2 (180°).
// Rotations 1/3 (90°/270°) are not implemented — RGB panels can't
// do those without per-pixel transposition anyway.
class RGBDisplayDB
{
public:
  RGBDisplayDB(int16_t w, int16_t h, Arduino_ESP32RGBPanelDB* panel);

  bool begin();
  void setRotation(uint8_t r);
  void fillScreen(uint16_t color);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t* bitmap,
                          int16_t w, int16_t h);
  void writePixel(int16_t x, int16_t y, uint16_t color);

  // Commit pending writes: trigger panel swap, switch our cached
  // pointer to the new back buffer, and pre-fill the new back from
  // the new front so partial updates stay coherent.
  void flush();

private:
  Arduino_ESP32RGBPanelDB* _panel;
  uint16_t* _fb;     // current back buffer (cached)
  int16_t   _w, _h;
  uint8_t   _rotation = 0;
  size_t    _fbBytes = 0;
};

#endif // ESP32S3
