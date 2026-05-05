// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Scott Adams save / restore.
 *
 * Format: a fixed-layout SaveHeader followed by (numItems+1) bytes
 * of item locations. Endianness is whatever the host writes —
 * ESP32-S3 is little-endian throughout, and saves don't move
 * between platforms, so we keep the layout simple.
 *
 * One slot per game. Save path is derived from the .DAT filename
 * by stripping the directory and extension, then appending ".SAV"
 * under /scottadams/saves/. Each game gets its own snapshot.
 */

#pragma once

#include <Arduino.h>
#include <FS.h>
#include "scott_dat.h"
#include "scott_play.h"

namespace scott
{
  static const uint8_t SAVE_VERSION = 1;

  #pragma pack(push, 1)
  struct SaveHeader
  {
    char     magic[4];          // "SAS\0"
    uint8_t  version;           // SAVE_VERSION
    uint8_t  reserved;
    uint16_t numItems;          // for sanity check vs. loaded game
    int16_t  advNumber;         // game identifier (.DAT trailer)
    uint8_t  curRoom;
    uint8_t  savedRoom;
    int16_t  counter;
    uint16_t lightLeft;
    uint32_t flags;
    uint8_t  savedRooms[16];
    int16_t  savedCounters[16];
    uint8_t  gameOver;
    uint8_t  pad;
  };
  #pragma pack(pop)

  // Write the current game state to `path` on `fs`. Returns true on
  // success. Caller is responsible for ensuring the parent directory
  // exists.
  inline bool saveGame(const Game& g, const PlayState& ps,
                       fs::FS& fs, const char* path)
  {
    File f = fs.open(path, FILE_WRITE);
    if (!f) return false;

    SaveHeader h{};
    h.magic[0] = 'S'; h.magic[1] = 'A'; h.magic[2] = 'S'; h.magic[3] = '\0';
    h.version    = SAVE_VERSION;
    h.numItems   = g.h.numItems;
    h.advNumber  = (int16_t)g.trailerAdvNum;
    h.curRoom    = ps.curRoom;
    h.savedRoom  = ps.savedRoom;
    h.counter    = ps.counter;
    h.lightLeft  = ps.lightLeft;
    h.flags      = ps.flags;
    memcpy(h.savedRooms,    ps.savedRooms,    sizeof(h.savedRooms));
    memcpy(h.savedCounters, ps.savedCounters, sizeof(h.savedCounters));
    h.gameOver   = ps.gameOver ? 1 : 0;

    if (f.write((const uint8_t*)&h, sizeof(h)) != sizeof(h))
    {
      f.close();
      return false;
    }
    for (int i = 0; i <= g.h.numItems; i++)
    {
      uint8_t loc = g.items[i].curLoc;
      if (f.write(&loc, 1) != 1)
      {
        f.close();
        return false;
      }
    }
    f.close();
    return true;
  }

  // Read a save file from `path`, applying it to `g` and `ps`.
  // The provided Game must already be loaded with the same .DAT
  // (we check numItems matches; mismatched files are rejected).
  inline bool restoreGame(Game& g, PlayState& ps,
                          fs::FS& fs, const char* path)
  {
    File f = fs.open(path, FILE_READ);
    if (!f) return false;

    SaveHeader h;
    if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h))
    {
      f.close();
      return false;
    }
    if (h.magic[0] != 'S' || h.magic[1] != 'A' || h.magic[2] != 'S' ||
        h.version  != SAVE_VERSION ||
        h.numItems != g.h.numItems)
    {
      f.close();
      return false;
    }

    ps.curRoom    = h.curRoom;
    ps.savedRoom  = h.savedRoom;
    ps.counter    = h.counter;
    ps.lightLeft  = h.lightLeft;
    ps.flags      = h.flags;
    memcpy(ps.savedRooms,    h.savedRooms,    sizeof(ps.savedRooms));
    memcpy(ps.savedCounters, h.savedCounters, sizeof(ps.savedCounters));
    ps.gameOver   = h.gameOver != 0;

    for (int i = 0; i <= g.h.numItems; i++)
    {
      uint8_t loc;
      if (f.read(&loc, 1) != 1)
      {
        f.close();
        return false;
      }
      g.items[i].curLoc = loc;
    }
    f.close();
    return true;
  }

}  // namespace scott
