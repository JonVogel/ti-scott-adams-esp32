// SPDX-License-Identifier: GPL-2.0-or-later
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
    uint8_t  curRoom        = 0;
    uint8_t  carryCount     = 0;   // cached count of items with curLoc==CARRIED
    uint32_t flags          = 0;   // 32 boolean flags (bits 0..31)
    int16_t  counter        = 0;   // main counter variable
    uint8_t  savedRoom      = 0;   // main swap-room slot (opcode 80)
    uint8_t  savedRooms[16] = {0}; // 16 saved rooms (opcode 87)
    int16_t  savedCounters[16] = {0}; // 16 saved counters (opcode 81)
    uint16_t lightLeft      = 0;   // turns until lamp dies
    bool     gameOver       = false;
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

  // -------------------------------------------------------------------
  // Two-word parser
  // -------------------------------------------------------------------

  struct Parsed
  {
    int verbIdx;   // 0 = no verb matched / no input
    int nounIdx;   // 0 = no noun (or no second word)
  };

  // Match `text` against a verb or noun list. Comparison is
  // case-insensitive over the first wordLen chars (per the .DAT
  // header). Returns the canonical (non-synonym) index, or 0 for no
  // match. Index 0 of each list is reserved (verb 0 = AUT, noun 0 =
  // ANY) and is never returned as a player match.
  inline int matchWord(const Word* words, int count, int wordLen,
                       const char* text)
  {
    if (!text || text[0] == '\0' || !words) return 0;

    for (int i = 1; i < count; i++)
    {
      const char* entry = words[i].text;
      if (entry[0] == '*') entry++;   // strip synonym marker for compare
      if (entry[0] == '\0') continue;

      bool match = true;
      for (int j = 0; j < wordLen; j++)
      {
        char a = (char)toupper((unsigned char)entry[j]);
        char b = (char)toupper((unsigned char)text[j]);
        // If either side runs out, everything we've compared so far
        // matched — treat as a successful prefix match. Lets the
        // player type "N" or "NO" for NORTH, "GE" for GET, etc.
        if (a == '\0' || b == '\0') break;
        if (a != b) { match = false; break; }
      }
      if (match)
      {
        // Walk back through synonyms to the canonical entry.
        int idx = i;
        while (idx > 0 && words[idx].text[0] == '*') idx--;
        return idx;
      }
    }
    return 0;
  }

  // Parse a player command. Tokenizes the first two whitespace-separated
  // words, uppercases them, looks each up in the verb/noun tables.
  // Single-word shorthand for directions ("N", "NORTH") is rewritten to
  // (GO, direction): per Scott Adams convention, verb 1 is GO and
  // nouns 1..6 are N S E W U D in that order.
  inline Parsed parseInput(const Game& g, const char* input)
  {
    Parsed p = {0, 0};
    if (!input) return p;

    char w1[16] = {0}, w2[16] = {0};
    const char* s = input;

    while (*s == ' ' || *s == '\t') s++;
    int i = 0;
    while (*s && *s != ' ' && *s != '\t' && i < 15)
    {
      w1[i++] = (char)toupper((unsigned char)*s++);
    }
    w1[i] = '\0';

    while (*s == ' ' || *s == '\t') s++;
    i = 0;
    while (*s && *s != ' ' && *s != '\t' && i < 15)
    {
      w2[i++] = (char)toupper((unsigned char)*s++);
    }
    w2[i] = '\0';

    if (w1[0] == '\0') return p;

    // Direction shorthand has the highest priority. Per spec §6.7,
    // single-letter cardinal directions (N S E W U D) ALWAYS map to
    // (GO, direction) and cannot be overridden by the story file's
    // verb list — so we match them before consulting verbs at all.
    // We also accept the full names (NORTH, etc.) and prefix forms
    // ("NO", "SOU") via the standard noun lookup below if the verb
    // search fails.
    if (w1[1] == '\0' && w2[0] == '\0')
    {
      static const char DIR_LETTERS[6] = { 'N', 'S', 'E', 'W', 'U', 'D' };
      for (int d = 0; d < 6; d++)
      {
        if (w1[0] == DIR_LETTERS[d])
        {
          p.verbIdx = 1;        // GO
          p.nounIdx = d + 1;    // 1..6 = N S E W U D
          return p;
        }
      }
    }

    // Universal single-letter shortcuts (spec §6.7):
    //   L → LOOK, I → INVENTORY, X → EXAMINE, Z → WAIT.
    // Recommended only when the letter isn't already a defined verb
    // in the story file — but in practice "L" causing LIGHT-like
    // collisions is the more common nuisance, so we apply these
    // unconditionally after the direction check.
    if (w1[1] == '\0' && w2[0] == '\0')
    {
      const char* full = nullptr;
      switch (w1[0])
      {
        case 'L': full = "LOOK"; break;
        case 'I': full = "INVE"; break;
        case 'X': full = "EXAM"; break;
        case 'Z': full = "WAIT"; break;
      }
      if (full)
      {
        int idx = matchWord(g.verbs, g.h.numWords + 1, g.h.wordLen, full);
        if (idx > 0) { p.verbIdx = idx; return p; }
      }
    }

    p.verbIdx = matchWord(g.verbs, g.h.numWords + 1, g.h.wordLen, w1);
    p.nounIdx = matchWord(g.nouns, g.h.numWords + 1, g.h.wordLen, w2);

    // Multi-char direction word ("NORTH", "NO", "SOU") with no verb:
    // re-route to (GO, direction). Single-letter forms were already
    // handled above.
    if (p.verbIdx == 0 && w2[0] == '\0')
    {
      int dirIdx = matchWord(g.nouns, g.h.numWords + 1, g.h.wordLen, w1);
      if (dirIdx >= 1 && dirIdx <= 6)
      {
        p.verbIdx = 1;
        p.nounIdx = dirIdx;
      }
    }

    return p;
  }

  // -------------------------------------------------------------------
  // Room rendering
  // -------------------------------------------------------------------

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
