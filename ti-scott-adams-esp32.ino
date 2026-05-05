// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Scott Adams Adventures for ESP32-S3
 *
 * Native .DAT adventure-game player targeting the Sunton
 * ESP32-8048S043C 4.3" RGB-display dev board. This file is the
 * scaffolded sketch — hardware bringup, TI-style 32x24 character
 * display, BLE keyboard input, and a minimal command shell with
 * file copy/list utilities. The Scott Adams interpreter itself
 * has not been written yet.
 *
 * Hardware bringup is inherited from ti-extended-basic-esp32 and is
 * known-good. Display geometry, palette, character font, RGB pin map,
 * and BLE keyboard wiring are unchanged from that project.
 *
 * Board settings (Arduino IDE):
 *   Board: "ESP32S3 Dev Module"
 *   PSRAM: "OPI PSRAM"
 *   Flash Size: "16MB"
 *   Partition: "3M App / 9M FATFS (16MB)"
 *   USB CDC On Boot: "Disabled" (default)
 */

#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <FS.h>
#include "rgb_db.h"
#include "ti_font.h"
#include "ble_keyboard.h"
#include "file_io.h"
#include "scott_dat.h"
#include "scott_play.h"
#include "scott_exec.h"

// ---------------------------------------------------------------------------
// ESP32-8048S043C (Sunton 4.3" 800x480 RGB) pin map + display geometry
// ---------------------------------------------------------------------------
#define TFT_BL 2

#define COLS       32
#define ROWS       24
#define CHAR_W     16
#define CHAR_H     16
#define SCREEN_W   800
#define SCREEN_H   480

#define DISPLAY_X_OFFSET ((SCREEN_W - COLS * CHAR_W) / 2)            // 144
#define DISPLAY_Y_OFFSET (((SCREEN_H - ROWS * CHAR_H) / 2) - 8)      // 40

#define BORDER_W       8
#define GRID_BOTTOM_Y  (DISPLAY_Y_OFFSET + ROWS * CHAR_H)            // 432

#define MAX_INPUT_LEN  64

// ---------------------------------------------------------------------------
// Display globals
// ---------------------------------------------------------------------------
static Arduino_ESP32RGBPanelDB *rgbBus = new Arduino_ESP32RGBPanelDB(
    40 /* DE */, 41 /* VSYNC */, 39 /* HSYNC */, 42 /* PCLK */,
    45 /* R0 */, 48 /* R1 */, 47 /* R2 */, 21 /* R3 */, 14 /* R4 */,
    5  /* G0 */, 6  /* G1 */, 7  /* G2 */, 15 /* G3 */, 16 /* G4 */, 4 /* G5 */,
    8  /* B0 */, 3  /* B1 */, 46 /* B2 */, 9  /* B3 */, 1  /* B4 */,
    0, 8, 4, 8,
    0, 8, 4, 8,
    1, 14000000,
    false,
    0, 0,
    20 * 800
);

static RGBDisplayDB *tft = new RGBDisplayDB(800, 480, rgbBus);

static int cursorCol = 0;
static int cursorRow = 0;

static uint16_t fgColor = 0x0000;
static uint16_t bgColor = 0x07FF;

static char screenBuf[ROWS][COLS];
static char prevScreenBuf[ROWS][COLS];

// Optional per-cell foreground color override. 0 means "use the
// per-character-code charFgIdx default"; 1..16 force that palette
// index for this cell. Lets renderRoom paint individual items in a
// distinct color without disturbing the global character palette.
static uint8_t cellFgOverride[ROWS][COLS];

// Color used by upcoming printChar calls. 0 = default (transparent /
// white). Set via setPrintColor() before printing a colored span,
// reset to 0 after.
static uint8_t currentPrintFg = 0;

static const uint16_t tiPalette[17] =
{
  0x0000,   // 0 unused
  0x0000,   // 1 transparent (resolves to screen color in drawCell)
  0x0000,   // 2 black
  0x0585,   // 3 medium green
  0x2D8B,   // 4 light green
  0x0012,   // 5 dark blue
  0x0417,   // 6 light blue
  0x8000,   // 7 dark red
  0x0EBF,   // 8 cyan
  0xE000,   // 9 medium red
  0xF2A3,   // 10 light red
  0xD5C0,   // 11 dark yellow
  0xE600,   // 12 light yellow
  0x0280,   // 13 dark green
  0xB816,   // 14 magenta
  0xC618,   // 15 gray
  0xFFFF,   // 16 white
};

static uint8_t charFgIdx[256];
static uint8_t charBgIdx[256];
static uint8_t screenColorIdx = 8;

// ---------------------------------------------------------------------------
// Display rendering
// ---------------------------------------------------------------------------
static void paintBorder()
{
  int frameX = DISPLAY_X_OFFSET - BORDER_W;
  int frameY = DISPLAY_Y_OFFSET - BORDER_W;
  int frameW = COLS * CHAR_W + 2 * BORDER_W;
  tft->fillRect(frameX, frameY, frameW, BORDER_W, bgColor);
  tft->fillRect(frameX, GRID_BOTTOM_Y, frameW, BORDER_W, bgColor);
  tft->fillRect(frameX, DISPLAY_Y_OFFSET, BORDER_W, ROWS * CHAR_H, bgColor);
  int rightX = DISPLAY_X_OFFSET + COLS * CHAR_W;
  tft->fillRect(rightX, DISPLAY_Y_OFFSET, BORDER_W, ROWS * CHAR_H, bgColor);
}

static void initDisplay()
{
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);

  tft->begin();
  tft->setRotation(2);
  tft->fillScreen(0x0000);
  tft->flush();

  digitalWrite(TFT_BL, HIGH);
}

static uint16_t resolveColor(uint8_t idx)
{
  if (idx < 1 || idx > 16) return 0;
  if (idx == 1) return tiPalette[screenColorIdx];
  return tiPalette[idx];
}

static void drawCell(int col, int row)
{
  uint8_t ch = (uint8_t)screenBuf[row][col];
  int px = col * CHAR_W + DISPLAY_X_OFFSET;
  int py = row * CHAR_H + DISPLAY_Y_OFFSET;

  uint8_t fgIdx = cellFgOverride[row][col];
  if (fgIdx == 0) fgIdx = charFgIdx[ch];
  uint16_t fg = resolveColor(fgIdx);
  uint16_t bg = resolveColor(charBgIdx[ch]);

  uint16_t pixBuf[CHAR_W * CHAR_H];
  for (int y = 0; y < 8; y++)
  {
    uint8_t bits = charPatterns[ch][y];
    uint16_t *row0 = &pixBuf[(y * 2)     * CHAR_W];
    uint16_t *row1 = &pixBuf[(y * 2 + 1) * CHAR_W];
    for (int x = 0; x < 8; x++)
    {
      uint16_t c = (bits & 0x80) ? fg : bg;
      row0[x * 2]     = c;
      row0[x * 2 + 1] = c;
      row1[x * 2]     = c;
      row1[x * 2 + 1] = c;
      bits <<= 1;
    }
  }
  tft->draw16bitRGBBitmap(px, py, pixBuf, CHAR_W, CHAR_H);
}

static void refreshScreen()
{
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      if (screenBuf[r][c] != prevScreenBuf[r][c])
      {
        drawCell(c, r);
        prevScreenBuf[r][c] = screenBuf[r][c];
      }
    }
  }
}

static void redrawScreen()
{
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      drawCell(c, r);
      prevScreenBuf[r][c] = screenBuf[r][c];
    }
  }
}

static void fillBackground(uint16_t bg)
{
  tft->fillScreen(0x0000);
  uint16_t savedBg = bgColor;
  bgColor = bg;
  paintBorder();
  bgColor = savedBg;
  tft->fillRect(DISPLAY_X_OFFSET, DISPLAY_Y_OFFSET,
                COLS * CHAR_W, ROWS * CHAR_H, bg);
}

static void resetCharColors()
{
  for (int i = 0; i < 256; i++)
  {
    charFgIdx[i] = 2;   // black
    charBgIdx[i] = 1;   // transparent (→ screen color)
  }
  screenColorIdx = 8;   // cyan
  fgColor = tiPalette[2];
  bgColor = tiPalette[8];
}

// Switch to the in-game terminal palette: white text on black screen.
// Called after the TI boot splash so the splash stays cyan but the
// shell / adventure text reads as a classic phosphor terminal.
static void setTerminalColors()
{
  for (int i = 0; i < 256; i++)
  {
    charFgIdx[i] = 16;  // white
    charBgIdx[i] = 1;   // transparent (→ screen color = black)
  }
  screenColorIdx = 2;   // black
  fgColor = tiPalette[16];
  bgColor = tiPalette[2];
  fillBackground(bgColor);
  for (int r = 0; r < ROWS; r++)
  {
    memset(screenBuf[r], ' ', COLS);
    memset(prevScreenBuf[r], 0, COLS);
  }
  cursorCol = 0;
  cursorRow = ROWS - 1;
}

// ---------------------------------------------------------------------------
// Console primitives
// ---------------------------------------------------------------------------
static void scrollUp()
{
  memcpy(&screenBuf[0][0], &screenBuf[1][0], COLS * (ROWS - 1));
  memset(&screenBuf[ROWS - 1][0], 0x20, COLS);
  memcpy(&cellFgOverride[0][0], &cellFgOverride[1][0], COLS * (ROWS - 1));
  memset(&cellFgOverride[ROWS - 1][0], 0, COLS);
  refreshScreen();
  int y = (ROWS - 1) * CHAR_H + DISPLAY_Y_OFFSET;
  tft->fillRect(DISPLAY_X_OFFSET, y, COLS * CHAR_W, CHAR_H, bgColor);
}

static void printChar(char c)
{
  Serial.write(c);
  if (c == '\n') Serial.write('\r');

  if (c == '\n')
  {
    scrollUp();
    cursorRow = ROWS - 1;
    cursorCol = 0;
    return;
  }

  if (cursorCol >= COLS)
  {
    scrollUp();
    cursorRow = ROWS - 1;
    cursorCol = 0;
  }

  screenBuf[cursorRow][cursorCol] = c;
  cellFgOverride[cursorRow][cursorCol] = currentPrintFg;
  drawCell(cursorCol, cursorRow);
  prevScreenBuf[cursorRow][cursorCol] = c;
  cursorCol++;
}

static void printString(const char* str)
{
  while (*str) printChar(*str++);
  tft->flush();
}

static void printLine(const char* str)
{
  printString(str);
  printChar('\n');
  tft->flush();
}

// Word-aware print: emit characters, but at each space check whether
// the next word would overflow the current line; if so, emit a newline
// before printing it. Honors embedded '\n' bytes as hard breaks. Used
// for game text (room descriptions, action messages) where the
// 32-column screen would otherwise chop words mid-letter.
static void printWrapped(const char* str)
{
  while (*str)
  {
    if (*str == '\n')
    {
      printChar('\n');
      str++;
      continue;
    }
    if (*str == ' ')
    {
      // Look ahead for the next word's length.
      const char* p = str + 1;
      while (*p == ' ') p++;
      int wlen = 0;
      while (p[wlen] && p[wlen] != ' ' && p[wlen] != '\n') wlen++;

      if (wlen > 0 && wlen <= COLS &&
          cursorCol + 1 + wlen > COLS)
      {
        // Skip the run of spaces, drop to a new line, print the word.
        printChar('\n');
        str = p;
        continue;
      }
      printChar(' ');
      str++;
      continue;
    }
    printChar(*str++);
  }
  tft->flush();
}

static void clearScreen()
{
  memset(screenBuf, ' ', COLS * ROWS);
  memset(cellFgOverride, 0, sizeof(cellFgOverride));
  fillBackground(bgColor);
  cursorCol = 0;
  cursorRow = ROWS - 1;
}

// ---------------------------------------------------------------------------
// TI boot screen
// ---------------------------------------------------------------------------
static const uint8_t tiLogoChars[9][8] =
{
  {0x00, 0x01, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03},
  {0x00, 0xFC, 0x04, 0x05, 0x05, 0x04, 0x06, 0x02},
  {0x00, 0x00, 0x80, 0x40, 0x40, 0x80, 0x00, 0x0C},
  {0x03, 0xFF, 0x80, 0xC0, 0x40, 0x60, 0x38, 0x1C},
  {0x0C, 0x19, 0x21, 0x21, 0x3D, 0x05, 0x05, 0x05},
  {0x12, 0xBA, 0x8A, 0x8A, 0xBA, 0xA1, 0xA1, 0xA1},
  {0x0E, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00},
  {0xC4, 0xE2, 0x31, 0x10, 0x18, 0x0C, 0x07, 0x03},
  {0x22, 0x4C, 0x90, 0x20, 0x40, 0x40, 0x20, 0xE0},
};

static void drawTexasLogo(int startRow, int startCol)
{
  for (int i = 0; i < 9; i++)
  {
    memcpy(charPatterns[129 + i], tiLogoChars[i], 8);
  }
  for (int r = 0; r < 3; r++)
  {
    for (int c = 0; c < 3; c++)
    {
      int ch = 129 + r * 3 + c;
      screenBuf[startRow + r][startCol + c] = (char)ch;
      drawCell(startCol + c, startRow + r);
    }
  }
}

static const uint8_t copyrightBitmap[8] =
{
  0x3C, 0x42, 0x99, 0xA1, 0xA1, 0x99, 0x42, 0x3C,
};

static void drawCenteredText(const char* text, int row)
{
  int len = strlen(text);
  int col = (COLS - len) / 2;
  if (col < 0) col = 0;
  for (int i = 0; i < len && col + i < COLS; i++)
  {
    screenBuf[row][col + i] = text[i];
    drawCell(col + i, row);
  }
}

static void waitForAnyKey()
{
  while (!Serial.available() && !bleKbAvailable())
  {
    bleKbTask();
    yield();
    delay(10);
  }
  while (Serial.available()) { Serial.read(); delay(2); }
  while (bleKbAvailable())   { bleKbRead();  delay(2); }
}

static void showBootScreen()
{
  fillBackground(tiPalette[8]);
  memcpy(charPatterns[128], copyrightBitmap, 8);

  const uint8_t stripes[] = {
    9, 4, 2, 12, 13, 14,
    5, 3, 14, 9, 15, 6, 10, 12, 9
  };
  const int numStripes = sizeof(stripes);
  const int stripeW = (COLS * CHAR_W) / 16;
  const int stripeH = 3 * CHAR_H;
  const int gapEnd  = 7 * stripeW;

  int topY = DISPLAY_Y_OFFSET;
  for (int i = 0; i < numStripes; i++)
  {
    int x = DISPLAY_X_OFFSET +
            ((i < 6) ? i * stripeW : (i - 6) * stripeW + gapEnd);
    tft->fillRect(x, topY, stripeW, stripeH, tiPalette[stripes[i]]);
  }

  int bottomY = DISPLAY_Y_OFFSET + 18 * CHAR_H;
  for (int i = 0; i < numStripes; i++)
  {
    int x = DISPLAY_X_OFFSET +
            ((i < 6) ? i * stripeW : (i - 6) * stripeW + gapEnd);
    tft->fillRect(x, bottomY, stripeW, stripeH, tiPalette[stripes[i]]);
  }

  drawTexasLogo(5, (COLS - 3) / 2);

  drawCenteredText("SCOTT ADAMS",                  9);
  drawCenteredText("ADVENTURES",                  11);
  drawCenteredText("PRESS ANY KEY TO BEGIN",      16);
  drawCenteredText("\x80" "1981  ADVENTURE INTL.", 22);

  Serial.println("PRESS ANY KEY TO CONTINUE");
  tft->flush();

  waitForAnyKey();

  fillBackground(tiPalette[8]);
  for (int r = 0; r < ROWS; r++)
  {
    memset(screenBuf[r], ' ', COLS);
    memset(prevScreenBuf[r], 0, COLS);
  }
  clearScreen();
}

// ---------------------------------------------------------------------------
// Line input — single-line editor reading from BLE keyboard or Serial.
// Strictly simpler than the BASIC editor: no line-number recall, no INS
// mode, no history beyond a single REDO buffer. Backspace + Enter only.
// ---------------------------------------------------------------------------
static char inputBuf[MAX_INPUT_LEN + 1];
static int  inputPos = 0;
static bool inputReady = false;
static int  inputStartCol = 0;
static int  inputStartRow = 0;

static int editorReadChar()
{
  if (Serial.available())
  {
    int c = Serial.read();
    if (c == '\n') c = '\r';
    return c;
  }
  if (bleKbAvailable())
  {
    return bleKbRead();
  }
  return -1;
}

static void editorBeginLine()
{
  inputPos = 0;
  inputReady = false;
  inputBuf[0] = '\0';
  inputStartCol = cursorCol;
  inputStartRow = cursorRow;
}

static void editorBackspace()
{
  if (inputPos == 0) return;
  inputPos--;
  inputBuf[inputPos] = '\0';
  if (cursorCol > 0)
  {
    cursorCol--;
    screenBuf[cursorRow][cursorCol] = ' ';
    drawCell(cursorCol, cursorRow);
    prevScreenBuf[cursorRow][cursorCol] = ' ';
    tft->flush();
  }
  Serial.write('\b');
  Serial.write(' ');
  Serial.write('\b');
}

static void editorTypeChar(uint8_t c)
{
  if (inputPos >= MAX_INPUT_LEN) return;
  inputBuf[inputPos++] = (char)c;
  inputBuf[inputPos] = '\0';
  printChar((char)c);
  tft->flush();
}

// Drain every available char (Serial + BLE) in one call. Stops early on
// Enter so the dispatcher gets a chance to run before the next char.
static void checkInput()
{
  int c;
  while ((c = editorReadChar()) >= 0)
  {
    if (c == '\r')
    {
      inputBuf[inputPos] = '\0';
      printChar('\n');
      inputReady = true;
      return;
    }

    // BKSP only (HID 0x7F). Don't treat 7 (DEL) or 8 (LEFT) as backspace
    // — they're TI cursor codes and we drop them silently for now.
    if (c == 127)
    {
      editorBackspace();
      continue;
    }

    if (c >= 32 && c < 127)
    {
      editorTypeChar((uint8_t)c);
      continue;
    }

    // Anything else (cursor keys, function keys, other control) — ignore
  }
}

// ---------------------------------------------------------------------------
// File operations: list .DAT files, copy SD -> FLASH
// ---------------------------------------------------------------------------
static void listDatFilesIn(fs::FS& fs, const char* root, const char* label)
{
  File dir = fs.open(root);
  if (!dir || !dir.isDirectory())
  {
    if (dir) dir.close();
    // Silent miss — `/scottadams` not yet created is normal on fresh
    // devices, and the rest of the listing covers the legacy root case.
    return;
  }

  char header[40];
  snprintf(header, sizeof(header), "-- %s %s --", label, root);
  printLine(header);

  int found = 0;
  File f = dir.openNextFile();
  while (f)
  {
    const char* name = f.name();
    int nlen = strlen(name);
    bool isDat =
      (nlen >= 4) &&
      (strcasecmp(name + nlen - 4, ".dat") == 0 ||
       strcasecmp(name + nlen - 4, ".DAT") == 0);
    if (isDat && !f.isDirectory())
    {
      char line[40];
      snprintf(line, sizeof(line), "  %-20s %6u", name, (unsigned)f.size());
      printLine(line);
      found++;
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();

  if (found == 0) printLine("  (no .DAT files)");
}

static void cmdDir()
{
  listDatFilesIn(LittleFS, "/scottadams", "FLASH");
  listDatFilesIn(LittleFS, "/", "FLASH");
  if (fio::g_sdOk)
  {
    listDatFilesIn(SD, "/scottadams", "SDCARD");
    listDatFilesIn(SD, "/", "SDCARD");
  }
  else
  {
    printLine("(SD card not present)");
  }
}

// Copy one file from src (any FS) to dst (any FS), overwriting.
// Returns true on success, false on any error.
static bool copyFileBinary(fs::FS& srcFs, const char* srcPath,
                           fs::FS& dstFs, const char* dstPath)
{
  File src = srcFs.open(srcPath, FILE_READ);
  if (!src || src.isDirectory())
  {
    if (src) src.close();
    return false;
  }
  File dst = dstFs.open(dstPath, FILE_WRITE);
  if (!dst)
  {
    src.close();
    return false;
  }

  uint8_t buf[512];
  size_t total = 0;
  while (src.available())
  {
    int n = src.read(buf, sizeof(buf));
    if (n <= 0) break;
    if (dst.write(buf, n) != (size_t)n)
    {
      src.close();
      dst.close();
      return false;
    }
    total += n;
    yield();
  }
  src.close();
  dst.close();
  return true;
}

// Resolve a user-supplied filename onto SD and FLASH paths. The user
// can type "ADV01.DAT" or "/scottadams/ADV01.DAT"; we look in
// /scottadams/ first, then root.
static bool findOnSd(const char* name, char* outPath, int outSize)
{
  if (!fio::g_sdOk) return false;
  if (name[0] == '/')
  {
    if (SD.exists(name))
    {
      snprintf(outPath, outSize, "%s", name);
      return true;
    }
    return false;
  }
  snprintf(outPath, outSize, "/scottadams/%s", name);
  if (SD.exists(outPath)) return true;
  snprintf(outPath, outSize, "/%s", name);
  return SD.exists(outPath);
}

// /scottadams/ADV01.DAT (anywhere) -> /scottadams/ADV01.DAT on FLASH.
// FLASH writes always land in /scottadams/ so we don't drop .DAT files
// at root, where the BASIC sketch's CAT would list them and a future
// wholesale-cleanup operation might touch them.
static void flashPathFor(const char* sdPath, char* outPath, int outSize)
{
  const char* base = strrchr(sdPath, '/');
  base = base ? base + 1 : sdPath;
  snprintf(outPath, outSize, "/scottadams/%s", base);
}

static void cmdCopy(const char* name)
{
  if (!name || !*name)
  {
    printLine("Usage: COPY name.dat");
    return;
  }
  if (!fio::g_sdOk)
  {
    printLine("SD card not present.");
    return;
  }

  char sdPath[64];
  if (!findOnSd(name, sdPath, sizeof(sdPath)))
  {
    char msg[40];
    snprintf(msg, sizeof(msg), "Not found: %s", name);
    printLine(msg);
    return;
  }

  char flashPath[64];
  flashPathFor(sdPath, flashPath, sizeof(flashPath));

  char msg[40];
  snprintf(msg, sizeof(msg), "Copying %s ...", sdPath);
  printLine(msg);

  if (copyFileBinary(SD, sdPath, LittleFS, flashPath))
  {
    snprintf(msg, sizeof(msg), "  -> FLASH%s", flashPath);
    printLine(msg);
  }
  else
  {
    printLine("Copy failed.");
  }
}

static void cmdCopyAll()
{
  if (!fio::g_sdOk)
  {
    printLine("SD card not present.");
    return;
  }

  const char* roots[] = { "/scottadams", "/" };
  int totalOk = 0, totalFail = 0;

  for (int ri = 0; ri < 2; ri++)
  {
    File dir = SD.open(roots[ri]);
    if (!dir || !dir.isDirectory())
    {
      if (dir) dir.close();
      continue;
    }

    File f = dir.openNextFile();
    while (f)
    {
      const char* name = f.name();
      int nlen = strlen(name);
      bool isDat =
        (nlen >= 4) &&
        (strcasecmp(name + nlen - 4, ".dat") == 0 ||
         strcasecmp(name + nlen - 4, ".DAT") == 0);
      if (isDat && !f.isDirectory())
      {
        char sdPath[64];
        snprintf(sdPath, sizeof(sdPath), "%s%s%s",
                 roots[ri],
                 (roots[ri][strlen(roots[ri]) - 1] == '/') ? "" : "/",
                 name);

        char flashPath[64];
        flashPathFor(sdPath, flashPath, sizeof(flashPath));

        char msg[40];
        snprintf(msg, sizeof(msg), "%-20s ", name);
        printString(msg);

        if (copyFileBinary(SD, sdPath, LittleFS, flashPath))
        {
          printLine("ok");
          totalOk++;
        }
        else
        {
          printLine("FAIL");
          totalFail++;
        }
      }
      f.close();
      f = dir.openNextFile();
    }
    dir.close();
  }

  char summary[40];
  snprintf(summary, sizeof(summary), "Done: %d ok, %d failed", totalOk, totalFail);
  printLine(summary);
}

static void cmdHelp()
{
  printLine("Commands:");
  printLine("  HELP         show this");
  printLine("  DIR          list .DAT files");
  printLine("  COPY NAME    SD -> FLASH");
  printLine("  COPYALL      copy every .DAT");
  printLine("  LOAD NAME    parse + show summary");
  printLine("  PLAY NAME    show starting room");
  printLine("  PARSE N CMD  parser test");
  printLine("  PAIR         re-open BLE pairing");
  printLine("  UNPAIR       forget all BLE peers");
  printLine("  BYE          restart");
}

static void cmdUnpair()
{
  BleHidHost::unpairAll();
  BleHidHost::requestPairingMode();
  printLine("All BLE peers forgotten.");
  printLine("Pairing mode open (30s).");
  printLine("Press a key on your keyboard.");
}

static void cmdPair()
{
  BleHidHost::requestPairingMode();
  printLine("Pairing mode requested (30s).");
}

// Resolve a user-supplied .DAT name onto a (fs, path) pair.
// Looks in FLASH /scottadams/, then FLASH / (intentional cross-access
// from BASIC-style root files), then SD /scottadams/, then SD /.
static bool findDat(const char* name, fs::FS*& outFs,
                    char* outPath, int outSize)
{
  char buf[64];

  snprintf(buf, sizeof(buf), "/scottadams/%s", name);
  if (LittleFS.exists(buf))
  {
    outFs = &LittleFS;
    snprintf(outPath, outSize, "%s", buf);
    return true;
  }
  snprintf(buf, sizeof(buf), "/%s", name);
  if (LittleFS.exists(buf))
  {
    outFs = &LittleFS;
    snprintf(outPath, outSize, "%s", buf);
    return true;
  }

  if (fio::g_sdOk)
  {
    snprintf(buf, sizeof(buf), "/scottadams/%s", name);
    if (SD.exists(buf))
    {
      outFs = &SD;
      snprintf(outPath, outSize, "%s", buf);
      return true;
    }
    snprintf(buf, sizeof(buf), "/%s", name);
    if (SD.exists(buf))
    {
      outFs = &SD;
      snprintf(outPath, outSize, "%s", buf);
      return true;
    }
  }
  return false;
}

static void cmdLoad(const char* name)
{
  if (!name || !*name)
  {
    printLine("Usage: LOAD name.dat");
    return;
  }

  fs::FS* fs = nullptr;
  char path[64];
  if (!findDat(name, fs, path, sizeof(path)))
  {
    char msg[40];
    snprintf(msg, sizeof(msg), "Not found: %s", name);
    printLine(msg);
    return;
  }

  uint32_t heapBefore = ESP.getFreeHeap();
  uint32_t psramBefore = ESP.getFreePsram();
  uint32_t t0 = millis();

  scott::Game g;
  char err[40] = {0};
  bool ok = g.load(*fs, path, err, sizeof(err));

  uint32_t dt = millis() - t0;

  if (!ok)
  {
    char msg[64];
    snprintf(msg, sizeof(msg), "Parse error: %s", err);
    printLine(msg);
    return;
  }

  uint32_t heapAfter = ESP.getFreeHeap();
  uint32_t psramAfter = ESP.getFreePsram();

  char line[40];
  snprintf(line, sizeof(line), "Loaded %s in %ums", path, (unsigned)dt);
  printLine(line);

  snprintf(line, sizeof(line), "Items:%-3d Actions:%d",
           g.h.numItems + 1, g.h.numActions + 1);
  printLine(line);

  snprintf(line, sizeof(line), "Words:%-3d (%d-char) Rooms:%d",
           g.h.numWords + 1, g.h.wordLen, g.h.numRooms + 1);
  printLine(line);

  snprintf(line, sizeof(line), "Treasures:%d in room %d",
           g.h.numTreasures, g.h.treasureRoom);
  printLine(line);

  snprintf(line, sizeof(line), "Start:%d Carry:%d Light:%d",
           g.h.startRoom, g.h.maxCarry, g.h.lightTime);
  printLine(line);

  snprintf(line, sizeof(line), "Messages:%d", g.h.numMessages + 1);
  printLine(line);

  snprintf(line, sizeof(line), "Trailer v%d adv%d 0x%04x",
           g.trailerVersion, g.trailerAdvNum, g.trailerMagic);
  printLine(line);

  snprintf(line, sizeof(line), "PSRAM used: %u bytes",
           (unsigned)(psramBefore - psramAfter));
  printLine(line);
  snprintf(line, sizeof(line), "Heap used: %u bytes",
           (unsigned)(heapBefore - heapAfter));
  printLine(line);
}

// Print a span of text with a temporary foreground color (1..16
// palette index). Resets to default after.
//
// Has its own up-front word-wrap check so single-word colored spans
// (like "WEST", "NORTH") drop to a new line cleanly when they would
// overflow. printWrapped's space-lookahead can't help in that case,
// because there's no space *before* the span — only after it.
static void printColored(uint8_t fgIdx, const char* str)
{
  if (str && *str && *str != '\n')
  {
    int wlen = 0;
    while (str[wlen] && str[wlen] != ' ' && str[wlen] != '\n') wlen++;
    if (wlen > 0 && wlen <= COLS && cursorCol + wlen > COLS)
    {
      printChar('\n');
    }
  }
  uint8_t saved = currentPrintFg;
  currentPrintFg = fgIdx;
  printWrapped(str);
  currentPrintFg = saved;
}

// Color-coded room renderer that draws description, exits, and items
// in distinct palette colors. Matches scott::renderRoom's structure
// but takes advantage of the per-cell color override to highlight
// each section. Treasures (item description starts with '*') get
// light yellow; ordinary items get light green; exits get cyan.
static void renderRoomColored(const scott::Game& g,
                              const scott::PlayState& ps)
{
  if (scott::isDark(g, ps))
  {
    printColored(/*light red*/10, "I can't see. It is too dark!\n");
    return;
  }

  const scott::Room& r = g.rooms[ps.curRoom];
  if (r.description && r.description[0] == '*')
  {
    printWrapped(r.description + 1);
  }
  else if (r.description && r.description[0] != '\0')
  {
    printWrapped("I'm in a ");
    printWrapped(r.description);
  }
  else
  {
    printWrapped("(no description)");
  }
  printWrapped("\n");

  bool anyExit = false;
  for (int i = 0; i < 6; i++)
  {
    if (r.exits[i] != 0)
    {
      printWrapped(anyExit ? ", " : "Obvious exits: ");
      printColored(/*cyan*/8, scott::DIR_NAMES[i]);
      anyExit = true;
    }
  }
  if (!anyExit) printWrapped("No obvious exits");
  printWrapped("\n");

  bool anyItem = false;
  for (int i = 0; i <= g.h.numItems; i++)
  {
    if (g.items[i].curLoc == ps.curRoom &&
        g.items[i].description &&
        g.items[i].description[0] != '\0')
    {
      printWrapped(anyItem ? ", " : "I can also see: ");
      bool isTreasure = (g.items[i].description[0] == '*');
      printColored(isTreasure ? /*light yellow*/12 : /*light green*/4,
                   g.items[i].description);
      anyItem = true;
    }
  }
  if (anyItem) printWrapped("\n");
}

// Block here, polling BLE + serial, until the line editor reports a
// completed line. Returns the trimmed input via the global inputBuf.
static void waitForLine()
{
  while (!inputReady)
  {
    bleKbTask();
    checkInput();
    static uint32_t lastFlush = 0;
    uint32_t now = millis();
    if (now - lastFlush >= 16)
    {
      tft->flush();
      lastFlush = now;
    }
    yield();
  }
}

// PLAY: load a .DAT, initialize play state, render the starting room,
// and enter the per-turn input loop. Returns to the shell on QUIT,
// game-over, or a parse error.
static void cmdPlay(const char* name)
{
  if (!name || !*name)
  {
    printLine("Usage: PLAY name.dat");
    return;
  }

  fs::FS* fs = nullptr;
  char path[64];
  if (!findDat(name, fs, path, sizeof(path)))
  {
    char msg[40];
    snprintf(msg, sizeof(msg), "Not found: %s", name);
    printLine(msg);
    return;
  }

  scott::Game g;
  char err[40] = {0};
  if (!g.load(*fs, path, err, sizeof(err)))
  {
    char msg[64];
    snprintf(msg, sizeof(msg), "Parse error: %s", err);
    printLine(msg);
    return;
  }

  scott::PlayState ps;
  scott::initPlay(g, ps);

  printLine("");
  renderRoomColored(g, ps);

  scott::ExecResult res;
  while (!ps.gameOver && !res.quit)
  {
    printLine("");
    printString("Tell me what to do? ");
    editorBeginLine();
    waitForLine();
    inputReady = false;

    scott::Parsed p = scott::parseInput(g, inputBuf);

    if (p.verbIdx == 0 && p.nounIdx == 0 && inputBuf[0] != '\0')
    {
      printLine("I don't know that word.");
      continue;
    }

    res = scott::execTurn(g, ps, p.verbIdx, p.nounIdx, printWrapped);
    scott::tickLight(g, ps, printWrapped);

    if (res.redrawRoom)
    {
      printLine("");
      renderRoomColored(g, ps);
    }
  }

  printLine("");
  if (res.quit) printLine("(left game)");
  else if (ps.gameOver) printLine("(game over)");
}

// PARSE NAME.DAT WORD [WORD] — load the given .DAT, parse the rest of
// the line as a player command, and print the verb/noun the parser
// matched. Diagnostic only; goes away once interactive PLAY exists.
static void cmdParse(const char* args)
{
  if (!args || !*args)
  {
    printLine("Usage: PARSE name.dat WORD [WORD]");
    return;
  }

  char name[32];
  int n = 0;
  while (*args && *args != ' ' && *args != '\t' && n < 31)
  {
    name[n++] = *args++;
  }
  name[n] = '\0';
  while (*args == ' ' || *args == '\t') args++;

  fs::FS* fs = nullptr;
  char path[64];
  if (!findDat(name, fs, path, sizeof(path)))
  {
    char msg[40];
    snprintf(msg, sizeof(msg), "Not found: %s", name);
    printLine(msg);
    return;
  }

  scott::Game g;
  char err[40] = {0};
  if (!g.load(*fs, path, err, sizeof(err)))
  {
    char msg[64];
    snprintf(msg, sizeof(msg), "Parse error: %s", err);
    printLine(msg);
    return;
  }

  scott::Parsed p = scott::parseInput(g, args);

  char line[40];
  snprintf(line, sizeof(line), "verb %d (%s)",
           p.verbIdx,
           (p.verbIdx > 0 && p.verbIdx <= g.h.numWords)
             ? g.verbs[p.verbIdx].text : "-");
  printLine(line);
  snprintf(line, sizeof(line), "noun %d (%s)",
           p.nounIdx,
           (p.nounIdx > 0 && p.nounIdx <= g.h.numWords)
             ? g.nouns[p.nounIdx].text : "-");
  printLine(line);
}

static void cmdBye()
{
  printLine("Restarting...");
  tft->flush();
  delay(250);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// Command dispatcher
// ---------------------------------------------------------------------------
static void skipSpaces(const char*& p)
{
  while (*p == ' ' || *p == '\t') p++;
}

static bool matchKeyword(const char*& p, const char* kw)
{
  int n = strlen(kw);
  if (strncasecmp(p, kw, n) != 0) return false;
  char nxt = p[n];
  if (nxt != '\0' && nxt != ' ' && nxt != '\t') return false;
  p += n;
  skipSpaces(p);
  return true;
}

static void processInput(const char* line)
{
  const char* p = line;
  skipSpaces(p);
  if (*p == '\0') return;

  if (matchKeyword(p, "HELP") || matchKeyword(p, "?"))
  {
    cmdHelp();
    return;
  }
  if (matchKeyword(p, "DIR") || matchKeyword(p, "CAT") ||
      matchKeyword(p, "LS"))
  {
    cmdDir();
    return;
  }
  if (matchKeyword(p, "COPYALL"))
  {
    cmdCopyAll();
    return;
  }
  if (matchKeyword(p, "COPY"))
  {
    cmdCopy(p);
    return;
  }
  if (matchKeyword(p, "LOAD"))
  {
    cmdLoad(p);
    return;
  }
  if (matchKeyword(p, "PLAY"))
  {
    cmdPlay(p);
    return;
  }
  if (matchKeyword(p, "PARSE"))
  {
    cmdParse(p);
    return;
  }
  if (matchKeyword(p, "UNPAIR"))
  {
    cmdUnpair();
    return;
  }
  if (matchKeyword(p, "PAIR"))
  {
    cmdPair();
    return;
  }
  if (matchKeyword(p, "BYE") || matchKeyword(p, "EXIT") ||
      matchKeyword(p, "QUIT"))
  {
    cmdBye();
    return;
  }

  printLine("? Unknown command. Type HELP.");
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup()
{
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  delay(500);

  if (!LittleFS.begin(true))
  {
    Serial.println("LittleFS mount failed!");
  }
  else
  {
    Serial.println("LittleFS mounted.");
    // Ensure our private subdir exists so /scottadams/* writes succeed.
    // mkdir is a no-op if the dir already exists.
    if (!LittleFS.exists("/scottadams"))
    {
      LittleFS.mkdir("/scottadams");
    }
  }

  if (fio::beginSD(/*cs=*/10, /*sck=*/12, /*miso=*/13, /*mosi=*/11))
  {
    Serial.println("SD card mounted.");
  }
  else
  {
    Serial.println("SD card not present.");
  }

  initDisplay();
  initCharPatterns();
  resetCharColors();

  for (int r = 0; r < ROWS; r++)
  {
    memset(screenBuf[r], ' ', COLS);
    memset(prevScreenBuf[r], 0, COLS);
  }

  clearScreen();

  bleKbInit();
  showBootScreen();

  setTerminalColors();

  cursorRow = ROWS - 1;
  cursorCol = 0;

  char banner[40];
  snprintf(banner, sizeof(banner),
           "SCOTT ADAMS / FREE: %dK",
           (int)(ESP.getFreeHeap() / 1024));
  printLine(banner);
  printLine("TYPE HELP FOR COMMANDS.");
  printString(">");
  editorBeginLine();

  Serial.println("Scott Adams Adventures (scaffold)");
  Serial.println("Type HELP for commands.");
}

void loop()
{
  bleKbTask();
  checkInput();

  if (inputReady)
  {
    processInput(inputBuf);
    printString(">");
    editorBeginLine();
  }

  static uint32_t lastFlush = 0;
  uint32_t now = millis();
  if (now - lastFlush >= 16)
  {
    tft->flush();
    lastFlush = now;
  }
}
