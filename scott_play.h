/*
 * Scott Adams play state + room renderer.
 *
 * Game (in scott_dat.h) holds the static, parsed-from-.DAT data.
 * PlayState here holds the mutable runtime state — current room,
 * flags, counters, light timer, win/lose. Item locations live in
 * Game::items[].curLoc and are mutated as the player picks things
 * up; that stays with the static struct so a save/restore can
 * snapshot the whole world via one Game copy.
 *
 * Phase B scope: just enough to display the player's current room
 * (description, obvious exits, visible items) — no input parsing,
 * no opcode interpreter. PLAY in the shell does load → initPlay →
 * renderRoom.
 */

#pragma once

#include <Arduino.h>
#include "scott_dat.h"

namespace scott
{
  struct PlayState
  {
    uint8_t  curRoom    = 0;
    uint8_t  carryCount = 0;     // cached count of items with curLoc==CARRIED
    uint16_t flags      = 0;     // 16 boolean flags, bit i set = flag i true
    uint8_t  counter    = 0;     // primary counter (used by SET COUNT etc.)
    uint8_t  savedRoom  = 0;     // for SWAP ROOM/SAVED action
    uint16_t lightLeft  = 0;     // turns until lamp dies; 0 = light source dead
    bool     gameOver   = false;
  };

  // Conventional location values. Items not in any room either sit in the
  // "store room" (room 0, off-stage) or are CARRIED.
  static const uint8_t LOC_STORE   = 0;
  static const uint8_t LOC_CARRIED = 255;

  // Direction names in the order rooms store exits: N S E W U D.
  static const char* const DIR_NAMES[6] =
  {
    "NORTH", "SOUTH", "EAST", "WEST", "UP", "DOWN"
  };

  // Pluggable printer so the renderer can be unit-tested without the
  // sketch's display layer. The .ino passes its printString here.
  using PrintFn = void(*)(const char*);

  inline void initPlay(const Game& g, PlayState& ps)
  {
    ps.curRoom    = (uint8_t)g.h.startRoom;
    ps.carryCount = 0;
    ps.flags      = 0;
    ps.counter    = 0;
    ps.savedRoom  = 0;
    ps.lightLeft  = g.h.lightTime;
    ps.gameOver   = false;
    // Item curLoc fields were populated to startLoc by the parser.
  }

  // Render description, exits, and visible items for the player's
  // current room. Each section ends in '\n'. Output goes through
  // `printStr`, which is expected to handle '\n' as a hard line break.
  inline void renderRoom(const Game& g, const PlayState& ps,
                         PrintFn printStr)
  {
    if (!printStr) return;

    const Room& r = g.rooms[ps.curRoom];

    // Description. Scott Adams convention: a leading '*' in the
    // source means "use literally" — otherwise prepend "I'm in a ".
    if (r.description && r.description[0] == '*')
    {
      printStr(r.description + 1);
    }
    else if (r.description && r.description[0] != '\0')
    {
      printStr("I'm in a ");
      printStr(r.description);
    }
    else
    {
      printStr("(no description)");
    }
    printStr("\n");

    // Exits.
    bool anyExit = false;
    for (int i = 0; i < 6; i++)
    {
      if (r.exits[i] != 0)
      {
        printStr(anyExit ? ", " : "Obvious exits: ");
        printStr(DIR_NAMES[i]);
        anyExit = true;
      }
    }
    if (!anyExit) printStr("No obvious exits");
    printStr("\n");

    // Items present in this room.
    bool anyItem = false;
    for (int i = 0; i <= g.h.numItems; i++)
    {
      if (g.items[i].curLoc == ps.curRoom &&
          g.items[i].description &&
          g.items[i].description[0] != '\0')
      {
        printStr(anyItem ? ", " : "I can also see: ");
        printStr(g.items[i].description);
        anyItem = true;
      }
    }
    if (anyItem) printStr("\n");
  }

}  // namespace scott
