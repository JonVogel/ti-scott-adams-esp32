// Double-buffered RGB panel + minimal display class — implementation.
// See rgb_db.h for design rationale.

#include "rgb_db.h"

#if defined(ESP32) && CONFIG_IDF_TARGET_ESP32S3

#include <string.h>
#include <esp_heap_caps.h>

// --- Panel ---------------------------------------------------------

Arduino_ESP32RGBPanelDB::Arduino_ESP32RGBPanelDB(
    int8_t de, int8_t vsync, int8_t hsync, int8_t pclk,
    int8_t r0, int8_t r1, int8_t r2, int8_t r3, int8_t r4,
    int8_t g0, int8_t g1, int8_t g2, int8_t g3, int8_t g4, int8_t g5,
    int8_t b0, int8_t b1, int8_t b2, int8_t b3, int8_t b4,
    uint16_t hsync_polarity, uint16_t hsync_front_porch,
    uint16_t hsync_pulse_width, uint16_t hsync_back_porch,
    uint16_t vsync_polarity, uint16_t vsync_front_porch,
    uint16_t vsync_pulse_width, uint16_t vsync_back_porch,
    uint16_t pclk_active_neg, int32_t prefer_speed, bool useBigEndian,
    uint16_t de_idle_high, uint16_t pclk_idle_high,
    size_t bounce_buffer_size_px)
  : _de(de), _vsync(vsync), _hsync(hsync), _pclk(pclk),
    _r0(r0), _r1(r1), _r2(r2), _r3(r3), _r4(r4),
    _g0(g0), _g1(g1), _g2(g2), _g3(g3), _g4(g4), _g5(g5),
    _b0(b0), _b1(b1), _b2(b2), _b3(b3), _b4(b4),
    _hsync_polarity(hsync_polarity),
    _hsync_front_porch(hsync_front_porch),
    _hsync_pulse_width(hsync_pulse_width),
    _hsync_back_porch(hsync_back_porch),
    _vsync_polarity(vsync_polarity),
    _vsync_front_porch(vsync_front_porch),
    _vsync_pulse_width(vsync_pulse_width),
    _vsync_back_porch(vsync_back_porch),
    _pclk_active_neg(pclk_active_neg),
    _prefer_speed(prefer_speed),
    _useBigEndian(useBigEndian),
    _de_idle_high(de_idle_high),
    _pclk_idle_high(pclk_idle_high),
    _bounce_buffer_size_px(bounce_buffer_size_px)
{
}

bool Arduino_ESP32RGBPanelDB::begin(int16_t w, int16_t h)
{
  _w = w;
  _h = h;

  esp_lcd_rgb_panel_config_t panel_config = {
      .clk_src = LCD_CLK_SRC_DEFAULT,
      .timings = {
          .pclk_hz = (uint32_t)(_prefer_speed > 0 ? _prefer_speed : 14000000),
          .h_res = (uint32_t)w,
          .v_res = (uint32_t)h,
          .hsync_pulse_width = _hsync_pulse_width,
          .hsync_back_porch  = _hsync_back_porch,
          .hsync_front_porch = _hsync_front_porch,
          .vsync_pulse_width = _vsync_pulse_width,
          .vsync_back_porch  = _vsync_back_porch,
          .vsync_front_porch = _vsync_front_porch,
          .flags = {
              .hsync_idle_low  = (uint32_t)((_hsync_polarity == 0) ? 1 : 0),
              .vsync_idle_low  = (uint32_t)((_vsync_polarity == 0) ? 1 : 0),
              .de_idle_high    = _de_idle_high,
              .pclk_active_neg = _pclk_active_neg,
              .pclk_idle_high  = _pclk_idle_high,
          },
      },
      .data_width = 16,
      .bits_per_pixel = 16,
      .num_fbs = 2,                                         // <-- DB
      .bounce_buffer_size_px = _bounce_buffer_size_px,
      .sram_trans_align  = 8,
      .psram_trans_align = 64,
      .hsync_gpio_num = _hsync,
      .vsync_gpio_num = _vsync,
      .de_gpio_num    = _de,
      .pclk_gpio_num  = _pclk,
      .disp_gpio_num  = GPIO_NUM_NC,
      .data_gpio_nums = {0},
      .flags = {
          .disp_active_low    = true,
          .refresh_on_demand  = false,
          .fb_in_psram        = true,
          .double_fb          = true,                       // <-- DB
          .no_fb              = false,
          .bb_invalidate_cache = false,
      },
  };

  // Same pin mapping as upstream Arduino_GFX
  if (_useBigEndian)
  {
    panel_config.data_gpio_nums[0]  = _g3;
    panel_config.data_gpio_nums[1]  = _g4;
    panel_config.data_gpio_nums[2]  = _g5;
    panel_config.data_gpio_nums[3]  = _r0;
    panel_config.data_gpio_nums[4]  = _r1;
    panel_config.data_gpio_nums[5]  = _r2;
    panel_config.data_gpio_nums[6]  = _r3;
    panel_config.data_gpio_nums[7]  = _r4;
    panel_config.data_gpio_nums[8]  = _b0;
    panel_config.data_gpio_nums[9]  = _b1;
    panel_config.data_gpio_nums[10] = _b2;
    panel_config.data_gpio_nums[11] = _b3;
    panel_config.data_gpio_nums[12] = _b4;
    panel_config.data_gpio_nums[13] = _g0;
    panel_config.data_gpio_nums[14] = _g1;
    panel_config.data_gpio_nums[15] = _g2;
  }
  else
  {
    panel_config.data_gpio_nums[0]  = _b0;
    panel_config.data_gpio_nums[1]  = _b1;
    panel_config.data_gpio_nums[2]  = _b2;
    panel_config.data_gpio_nums[3]  = _b3;
    panel_config.data_gpio_nums[4]  = _b4;
    panel_config.data_gpio_nums[5]  = _g0;
    panel_config.data_gpio_nums[6]  = _g1;
    panel_config.data_gpio_nums[7]  = _g2;
    panel_config.data_gpio_nums[8]  = _g3;
    panel_config.data_gpio_nums[9]  = _g4;
    panel_config.data_gpio_nums[10] = _g5;
    panel_config.data_gpio_nums[11] = _r0;
    panel_config.data_gpio_nums[12] = _r1;
    panel_config.data_gpio_nums[13] = _r2;
    panel_config.data_gpio_nums[14] = _r3;
    panel_config.data_gpio_nums[15] = _r4;
  }

  if (esp_lcd_new_rgb_panel(&panel_config, &_panel_handle) != ESP_OK) return false;
  if (esp_lcd_panel_reset(_panel_handle) != ESP_OK)                    return false;
  if (esp_lcd_panel_init(_panel_handle) != ESP_OK)                     return false;

  // esp_lcd_rgb_panel_get_frame_buffer is VARIADIC: the fb_num arg
  // is the count of out-pointer args that follow. Pass 2 with both
  // pointers in one call. (Two separate calls with `2, &fb` would
  // write to a non-existent second slot and crash — caught that the
  // hard way.)
  void *fb1 = nullptr, *fb2 = nullptr;
  if (esp_lcd_rgb_panel_get_frame_buffer(_panel_handle, 2, &fb1, &fb2) != ESP_OK) return false;
  if (!fb1 || !fb2) return false;

  _fb1 = (uint16_t*)fb1;
  _fb2 = (uint16_t*)fb2;
  _frontFb = _fb1;
  _backFb  = _fb2;

  size_t bytes = (size_t)_w * _h * sizeof(uint16_t);
  memset(_fb1, 0, bytes);
  memset(_fb2, 0, bytes);
  Cache_WriteBack_Addr((uint32_t)_fb1, bytes);
  Cache_WriteBack_Addr((uint32_t)_fb2, bytes);
  return true;
}

bool Arduino_ESP32RGBPanelDB::commitFrame()
{
  if (!_panel_handle || !_backFb) return false;

  // Flush CPU caches so panel DMA sees the latest writes.
  size_t bytes = (size_t)_w * _h * sizeof(uint16_t);
  Cache_WriteBack_Addr((uint32_t)_backFb, bytes);

  // Trigger swap on next vsync. Passing the back-buffer pointer
  // tells the driver "no copy needed, just present this buffer".
  esp_lcd_panel_draw_bitmap(_panel_handle, 0, 0, _w, _h, _backFb);

  // Update our pointer tracking. The buffer we just committed is
  // now the new front; the old front becomes the new back.
  uint16_t* tmp = _frontFb;
  _frontFb = _backFb;
  _backFb  = tmp;

  // Pre-fill the new back from the new front so partial updates next
  // frame don't reveal stale content from 2 frames ago. This memcpy
  // is the dominant cost (~10 ms for 800x480x2 in PSRAM); without
  // it, anything we don't redraw shows up wrong.
  memcpy(_backFb, _frontFb, bytes);
  Cache_WriteBack_Addr((uint32_t)_backFb, bytes);
  return true;
}


// --- Display -------------------------------------------------------

RGBDisplayDB::RGBDisplayDB(int16_t w, int16_t h,
                          Arduino_ESP32RGBPanelDB* panel)
  : _panel(panel), _fb(nullptr), _w(w), _h(h)
{
  _fbBytes = (size_t)w * h * sizeof(uint16_t);
}

bool RGBDisplayDB::begin()
{
  if (!_panel->begin(_w, _h)) return false;
  _fb = _panel->backBuffer();
  return _fb != nullptr;
}

void RGBDisplayDB::setRotation(uint8_t r)
{
  _rotation = r;
}

// Coordinate transform helper for rotation 2 (180°). Returns true if
// the (x,y) is inside the panel bounds; transformed in-place.
static inline bool xform(int16_t& x, int16_t& y, int16_t W, int16_t H,
                         uint8_t rot)
{
  if (x < 0 || y < 0 || x >= W || y >= H) return false;
  if (rot == 2)
  {
    x = W - 1 - x;
    y = H - 1 - y;
  }
  return true;
}

void RGBDisplayDB::fillScreen(uint16_t color)
{
  if (!_fb) return;
  // Fast path: solid color, no rotation needed.
  size_t total = (size_t)_w * _h;
  for (size_t i = 0; i < total; i++) _fb[i] = color;
}

void RGBDisplayDB::fillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                            uint16_t color)
{
  if (!_fb) return;
  // For rotation 2, mirror the rect's anchor.
  int16_t x0 = x, y0 = y;
  int16_t x1 = x + w - 1;
  int16_t y1 = y + h - 1;
  if (_rotation == 2)
  {
    int16_t tx0 = _w - 1 - x1;
    int16_t ty0 = _h - 1 - y1;
    x0 = tx0; y0 = ty0;
    x1 = tx0 + w - 1;
    y1 = ty0 + h - 1;
  }
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= _w) x1 = _w - 1;
  if (y1 >= _h) y1 = _h - 1;
  if (x0 > x1 || y0 > y1) return;
  for (int16_t yy = y0; yy <= y1; yy++)
  {
    uint16_t* row = &_fb[yy * _w];
    for (int16_t xx = x0; xx <= x1; xx++)
    {
      row[xx] = color;
    }
  }
}

void RGBDisplayDB::draw16bitRGBBitmap(int16_t x, int16_t y,
                                      uint16_t* bitmap,
                                      int16_t w, int16_t h)
{
  if (!_fb || !bitmap) return;
  if (_rotation == 0)
  {
    // Native: row-by-row memcpy with clipping.
    int16_t x0 = x, y0 = y;
    int16_t x1 = x + w - 1, y1 = y + h - 1;
    if (x1 < 0 || y1 < 0 || x0 >= _w || y0 >= _h) return;
    int16_t srcXOff = (x0 < 0) ? -x0 : 0;
    int16_t srcYOff = (y0 < 0) ? -y0 : 0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= _w) x1 = _w - 1;
    if (y1 >= _h) y1 = _h - 1;
    int16_t copyW = x1 - x0 + 1;
    int16_t copyH = y1 - y0 + 1;
    for (int16_t yy = 0; yy < copyH; yy++)
    {
      uint16_t* dst = &_fb[(y0 + yy) * _w + x0];
      uint16_t* src = &bitmap[(srcYOff + yy) * w + srcXOff];
      memcpy(dst, src, copyW * sizeof(uint16_t));
    }
    return;
  }
  // Rotation 2: write each src pixel at (W-1-(x+sx), H-1-(y+sy)).
  for (int16_t sy = 0; sy < h; sy++)
  {
    for (int16_t sx = 0; sx < w; sx++)
    {
      int16_t dx = x + sx;
      int16_t dy = y + sy;
      int16_t tx = dx, ty = dy;
      if (!xform(tx, ty, _w, _h, _rotation)) continue;
      _fb[ty * _w + tx] = bitmap[sy * w + sx];
    }
  }
}

void RGBDisplayDB::writePixel(int16_t x, int16_t y, uint16_t color)
{
  if (!_fb) return;
  int16_t tx = x, ty = y;
  if (!xform(tx, ty, _w, _h, _rotation)) return;
  _fb[ty * _w + tx] = color;
}

void RGBDisplayDB::flush()
{
  if (!_panel) return;
  _panel->commitFrame();
  _fb = _panel->backBuffer();
}

#endif // ESP32S3
