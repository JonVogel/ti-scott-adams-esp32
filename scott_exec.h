/*
 * Scott Adams action interpreter — condition opcodes, action opcodes,
 * action-table dispatch, and a small set of built-in fallbacks for the
 * conventional verbs (GO, GET, DROP, INV, LOOK, QUIT).
 *
 * Per the published .DAT format spec:
 *
 *   Each Action has 5 condition slots and 4 packed sub-action slots.
 *   Condition slot   = arg * 20 + opcode      (opcode 0..19)
 *   Sub-action pair  = sub_a * 150 + sub_b    (each sub 0..149)
 *
 *   Opcode 0 in a condition slot is "PAR" — not a real test, but a
 *   parameter carrier. Its arg is pushed onto a small queue that
 *   later sub-actions pop from.
 *
 *   Sub-action ranges:
 *     0          no-op
 *     1..51      print message N
 *     52..101    real opcodes (GET / DROP / GOTO / etc.)
 *     102..149   print message (N - 50)
 *
 * No GPL code from ScottFree or other interpreters has been copied.
 */

#pragma once

#include <Arduino.h>
#include "scott_dat.h"
#include "scott_play.h"

namespace scott
{
  // -------------------------------------------------------------------
  // Conventions baked into every standard Scott Adams .DAT
  // -------------------------------------------------------------------

  static const int VERB_AUTO  = 0;   // "AUT" — auto-action trigger
  static const int VERB_GO    = 1;   // verb 1 is always GO
  static const int VERB_GET   = 10;  // commonly GET (also TAKE / PICK / *AX synonyms)
  static const int NOUN_ANY   = 0;   // matches anything in action verb-noun key

  // Game flags that the format reserves a specific bit for.
  // (Other flags 0..14 are game-defined.)
  static const int FLAG_DARK = 15;   // "night" / "day" actions toggle this

  // -------------------------------------------------------------------
  // Per-turn execution result
  // -------------------------------------------------------------------

  struct ExecResult
  {
    bool acted        = false;  // some action ran (not silent fall-through)
    bool redrawRoom   = false;  // current room changed — caller should re-render
    bool gameOver     = false;  // game forced finish (action 63) or death
    bool quit         = false;  // user-requested QUIT
  };

  // -------------------------------------------------------------------
  // Helpers — flag bits, item moves, inventory listing
  // -------------------------------------------------------------------

  inline bool flagGet(const PlayState& ps, int bit)
  {
    if (bit < 0 || bit > 15) return false;
    return (ps.flags & (1u << bit)) != 0;
  }
  inline void flagSet(PlayState& ps, int bit)
  {
    if (bit >= 0 && bit <= 15) ps.flags |= (1u << bit);
  }
  inline void flagClear(PlayState& ps, int bit)
  {
    if (bit >= 0 && bit <= 15) ps.flags &= ~(1u << bit);
  }

  inline int countCarried(const Game& g)
  {
    int n = 0;
    for (int i = 0; i <= g.h.numItems; i++)
    {
      if (g.items[i].curLoc == LOC_CARRIED) n++;
    }
    return n;
  }

  inline void moveItemTo(Game& g, int item, uint8_t loc)
  {
    if (item < 0 || item > g.h.numItems) return;
    g.items[item].curLoc = loc;
  }

  // Find an item the player can refer to by noun. We match against the
  // /WORD/ marker recorded in the parser, comparing the first wordLen
  // chars to the noun text. Searches only items in the given location
  // (e.g., LOC_CARRIED for DROP, current room for GET).
  inline int findItemAt(const Game& g, const char* nounText,
                        uint8_t loc, int wordLen)
  {
    if (!nounText || nounText[0] == '\0') return -1;
    for (int i = 0; i <= g.h.numItems; i++)
    {
      if (g.items[i].curLoc != loc) continue;
      const char* a = g.items[i].autoNoun;
      if (a[0] == '\0') continue;
      bool match = true;
      for (int j = 0; j < wordLen; j++)
      {
        char ca = (char)toupper((unsigned char)a[j]);
        char cb = (char)toupper((unsigned char)nounText[j]);
        if (ca != cb) { match = false; break; }
        if (ca == '\0') break;
      }
      if (match) return i;
    }
    return -1;
  }

  inline void printInventory(const Game& g, PrintFn printStr)
  {
    if (!printStr) return;
    bool any = false;
    for (int i = 0; i <= g.h.numItems; i++)
    {
      if (g.items[i].curLoc != LOC_CARRIED) continue;
      printStr(any ? ", " : "I am carrying: ");
      printStr(g.items[i].description ? g.items[i].description : "?");
      any = true;
    }
    if (!any) printStr("I am carrying nothing");
    printStr("\n");
  }

  inline void printScore(const Game& g, const PlayState& ps,
                         PrintFn printStr)
  {
    if (!printStr) return;
    int found = 0;
    for (int i = 0; i <= g.h.numItems; i++)
    {
      const char* d = g.items[i].description;
      if (d && d[0] == '*' && g.items[i].curLoc == g.h.treasureRoom)
      {
        found++;
      }
    }
    char buf[40];
    int total = g.h.numTreasures > 0 ? g.h.numTreasures : 1;
    snprintf(buf, sizeof(buf), "I've stored %d treasures of %d.",
             found, total);
    printStr(buf);
    printStr("\n");
    int pct = (found * 100) / total;
    snprintf(buf, sizeof(buf), "On a scale of 0 to 100 that's %d.", pct);
    printStr(buf);
    printStr("\n");
    (void)ps;
  }

  // -------------------------------------------------------------------
  // Condition evaluation
  // -------------------------------------------------------------------

  // Returns true if all 5 conditions in `act` pass. PAR (opcode 0)
  // conditions always pass and push their arg onto `params[]` for
  // later sub-actions to consume; *paramCount is set to how many
  // parameters were collected.
  inline bool evalConditions(const Game& g, const PlayState& ps,
                             const Action& act,
                             int* params, int& paramCount)
  {
    paramCount = 0;
    for (int i = 0; i < 5; i++)
    {
      uint16_t c = act.cond[i];
      int op  = c % 20;
      int arg = c / 20;

      switch (op)
      {
        case  0:  // PAR — parameter carrier; always passes
          if (paramCount < 5) params[paramCount++] = arg;
          break;
        case  1:  // HAS — player carries item arg
          if (g.items[arg].curLoc != LOC_CARRIED) return false;
          break;
        case  2:  // IN  — item arg is in player's room (visible)
          if (g.items[arg].curLoc != ps.curRoom) return false;
          break;
        case  3:  // AVL — carried OR in current room
          if (g.items[arg].curLoc != LOC_CARRIED &&
              g.items[arg].curLoc != ps.curRoom) return false;
          break;
        case  4:  // IN_ — player is in room arg
          if (ps.curRoom != arg) return false;
          break;
        case  5:  // -HAVE — player does NOT carry item arg
          if (g.items[arg].curLoc == LOC_CARRIED) return false;
          break;
        case  6:  // -IN — item arg is NOT in player's room
          if (g.items[arg].curLoc == ps.curRoom) return false;
          break;
        case  7:  // -IN_ — player is NOT in room arg
          if (ps.curRoom == arg) return false;
          break;
        case  8:  // BIT — flag arg is set
          if (!flagGet(ps, arg)) return false;
          break;
        case  9:  // -BIT — flag arg is clear
          if (flagGet(ps, arg)) return false;
          break;
        case 10:  // ANY — player carries any item
          if (countCarried(g) == 0) return false;
          break;
        case 11:  // -ANY — player carries nothing
          if (countCarried(g) != 0) return false;
          break;
        case 12:  // -AVL — item arg is neither carried nor in room
          if (g.items[arg].curLoc == LOC_CARRIED ||
              g.items[arg].curLoc == ps.curRoom) return false;
          break;
        case 13:  // -RM0 — item arg is NOT in store room (it's in play)
          if (g.items[arg].curLoc == LOC_STORE) return false;
          break;
        case 14:  // RM0 — item arg is in store room (not yet placed)
          if (g.items[arg].curLoc != LOC_STORE) return false;
          break;
        case 15:  // CT< — counter < arg
          if (!(ps.counter < arg)) return false;
          break;
        case 16:  // CT> — counter > arg (strictly greater)
          if (!(ps.counter > arg)) return false;
          break;
        case 17:  // ORG — item arg is at its original starting location
          if (g.items[arg].curLoc != g.items[arg].startLoc) return false;
          break;
        case 18:  // -ORG — item arg has been moved from its start
          if (g.items[arg].curLoc == g.items[arg].startLoc) return false;
          break;
        case 19:  // CT= — counter equals arg
          if (ps.counter != arg) return false;
          break;
        default:
          return false;
      }
    }
    return true;
  }

  // -------------------------------------------------------------------
  // Sub-action execution
  // -------------------------------------------------------------------

  // Run one sub-action opcode. Returns true if the action requested a
  // game-over (opcode 63 finish or 61 dead). `params` is the queue
  // populated by PAR conditions; `pIdx` is the consumer cursor.
  inline void execSubaction(Game& g, PlayState& ps, int sub,
                            int* params, int& pIdx,
                            ExecResult& res, PrintFn printStr)
  {
    if (sub == 0) return;

    // 1..51: print message N
    if (sub >= 1 && sub <= 51)
    {
      const char* m = (sub <= g.h.numMessages) ? g.messages[sub] : nullptr;
      if (m && printStr) { printStr(m); printStr("\n"); }
      return;
    }
    // 102..149: print message (N - 50)
    if (sub >= 102 && sub <= 149)
    {
      int idx = sub - 50;
      const char* m = (idx <= g.h.numMessages) ? g.messages[idx] : nullptr;
      if (m && printStr) { printStr(m); printStr("\n"); }
      return;
    }

    auto popParam = [&](int dflt = 0) -> int {
      if (pIdx < 5) return params[pIdx++];
      return dflt;
    };

    switch (sub)
    {
      case 52:  // GET — pick up item arg (param)
      {
        int it = popParam();
        if (countCarried(g) >= g.h.maxCarry)
        {
          if (printStr) { printStr("I've too much to carry."); printStr("\n"); }
        }
        else
        {
          moveItemTo(g, it, LOC_CARRIED);
        }
        break;
      }
      case 53:  // DROP — drop item arg (param) in current room
      {
        int it = popParam();
        moveItemTo(g, it, ps.curRoom);
        break;
      }
      case 54:  // GOTO room arg (param)
      {
        int rm = popParam();
        if (rm >= 0 && rm <= g.h.numRooms)
        {
          ps.curRoom = (uint8_t)rm;
          res.redrawRoom = true;
        }
        break;
      }
      case 55:  // x->RM0 — remove item arg from play
      {
        int it = popParam();
        moveItemTo(g, it, LOC_STORE);
        res.redrawRoom = true;
        break;
      }
      case 56:  // night — set DARK_FLAG
        flagSet(ps, FLAG_DARK);
        break;
      case 57:  // day — clear DARK_FLAG
        flagClear(ps, FLAG_DARK);
        break;
      case 58:  // set flag arg
        flagSet(ps, popParam());
        break;
      case 59:  // x->RM0 (alias for 55 in some games)
      {
        int it = popParam();
        moveItemTo(g, it, LOC_STORE);
        res.redrawRoom = true;
        break;
      }
      case 60:  // clear flag arg
        flagClear(ps, popParam());
        break;
      case 61:  // dead
        if (printStr) { printStr("I am dead."); printStr("\n"); }
        flagClear(ps, FLAG_DARK);
        ps.curRoom = (uint8_t)g.h.numRooms;   // last room is conventionally limbo
        res.redrawRoom = true;
        break;
      case 62:  // x->y — move item arg1 to room arg2
      {
        int it = popParam();
        int rm = popParam();
        moveItemTo(g, it, (uint8_t)rm);
        res.redrawRoom = true;
        break;
      }
      case 63:  // finish — end of game
        if (printStr) { printStr("The game is now over."); printStr("\n"); }
        ps.gameOver  = true;
        res.gameOver = true;
        break;
      case 64:  // DspRM — re-display room
      case 76:
        res.redrawRoom = true;
        break;
      case 65:  // SCORE
        printScore(g, ps, printStr);
        break;
      case 66:  // INV
        printInventory(g, printStr);
        break;
      case 67:  // set flag 0
        flagSet(ps, 0);
        break;
      case 68:  // clear flag 0
        flagClear(ps, 0);
        break;
      case 69:  // refill lamp + flag 0 set
        ps.lightLeft = g.h.lightTime;
        flagClear(ps, 16);   // some games use bit 16, ignored if out of range
        flagSet(ps, 0);
        break;
      case 70:  // clear screen — we just emit a few newlines
        if (printStr) printStr("\n\n");
        res.redrawRoom = true;
        break;
      case 71:  // SaveGame — not implemented yet
        if (printStr) { printStr("Save not yet implemented."); printStr("\n"); }
        break;
      case 72:  // x<->y — swap two items' locations
      {
        int a = popParam();
        int b = popParam();
        if (a >= 0 && a <= g.h.numItems &&
            b >= 0 && b <= g.h.numItems)
        {
          uint8_t la = g.items[a].curLoc;
          g.items[a].curLoc = g.items[b].curLoc;
          g.items[b].curLoc = la;
          res.redrawRoom = true;
        }
        break;
      }
      case 73:  // continue — process next action; we just no-op here
        break;
      case 74:  // get without slot check (force take)
      {
        int it = popParam();
        moveItemTo(g, it, LOC_CARRIED);
        break;
      }
      case 75:  // x<->z — put item x where item z is
      {
        int a = popParam();
        int b = popParam();
        if (a >= 0 && a <= g.h.numItems &&
            b >= 0 && b <= g.h.numItems)
        {
          g.items[a].curLoc = g.items[b].curLoc;
          res.redrawRoom = true;
        }
        break;
      }
      case 77:  // counter -= 1
        if (ps.counter > 0) ps.counter--;
        break;
      case 78:  // counter += 1
        if (ps.counter < 255) ps.counter++;
        break;
      case 79:  // print counter
      {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", ps.counter);
        if (printStr) printStr(buf);
        break;
      }
      case 80:  // counter <- arg
        ps.counter = (uint8_t)popParam();
        break;
      case 81:  // ROOM<->saved — swap current room with savedRoom
      {
        uint8_t tmp = ps.curRoom;
        ps.curRoom = ps.savedRoom;
        ps.savedRoom = tmp;
        res.redrawRoom = true;
        break;
      }
      case 82:  // counter += arg
        ps.counter = (uint8_t)(ps.counter + popParam());
        break;
      case 83:  // counter -= arg
      {
        int sub2 = popParam();
        ps.counter = (uint8_t)(ps.counter - sub2);
        break;
      }
      case 84:  // print noun (we don't keep the typed noun string yet)
        break;
      case 85:  // print noun + newline
        if (printStr) printStr("\n");
        break;
      case 86:  // print newline
        if (printStr) printStr("\n");
        break;
      case 87:  // swap savedRoom slot (single slot model — same as 81)
      {
        uint8_t tmp = ps.curRoom;
        ps.curRoom = ps.savedRoom;
        ps.savedRoom = tmp;
        res.redrawRoom = true;
        break;
      }
      case 88:  // pause — wait n seconds (we keep it brief)
        delay(700);
        break;
      case 89:  // SAGA graphics — text version, ignore
        break;
      default:
        // Unknown / extended opcode — ignore quietly so we don't crash
        // a game that uses an opcode outside the documented set.
        break;
    }
  }

  // Run all four sub-actions of an action.
  inline void execActionPair(Game& g, PlayState& ps, const Action& act,
                             int* params, int& pIdx,
                             ExecResult& res, PrintFn printStr)
  {
    int subs[4] = {
      act.actionPair[0] / 150,
      act.actionPair[0] % 150,
      act.actionPair[1] / 150,
      act.actionPair[1] % 150,
    };
    for (int i = 0; i < 4; i++)
    {
      execSubaction(g, ps, subs[i], params, pIdx, res, printStr);
      if (res.gameOver || res.quit) return;
    }
  }

  // -------------------------------------------------------------------
  // Action-table dispatch
  // -------------------------------------------------------------------

  // Try to run any matching player action. Walks the actions table in
  // order; the first action whose verb-noun key matches AND whose
  // conditions all pass wins. Returns true if an action ran.
  inline bool tryPlayerAction(Game& g, PlayState& ps,
                              int verbIdx, int nounIdx,
                              ExecResult& res, PrintFn printStr)
  {
    if (verbIdx <= 0) return false;
    int wantKey = verbIdx * 150 + nounIdx;
    int wantAny = verbIdx * 150;   // matches verb with NOUN_ANY (0)
    for (int i = 0; i <= g.h.numActions; i++)
    {
      const Action& act = g.actions[i];
      if (act.verbNoun == 0) continue;          // auto-action
      if (act.verbNoun != wantKey &&
          act.verbNoun != wantAny) continue;
      int params[5] = {0,0,0,0,0};
      int paramCount = 0;
      if (!evalConditions(g, ps, act, params, paramCount)) continue;
      int pIdx = 0;
      execActionPair(g, ps, act, params, pIdx, res, printStr);
      res.acted = true;
      return true;
    }
    return false;
  }

  // Run all auto-actions (verb 0). They fire whenever their conditions
  // pass — typically used for "if it's dark and you have no lamp,
  // describe the room as dark", or "if counter expired, kill the
  // player", etc. Multiple may fire per turn.
  inline void runAutoActions(Game& g, PlayState& ps,
                             ExecResult& res, PrintFn printStr)
  {
    for (int i = 0; i <= g.h.numActions; i++)
    {
      const Action& act = g.actions[i];
      if (act.verbNoun != 0) continue;
      int params[5] = {0,0,0,0,0};
      int paramCount = 0;
      if (!evalConditions(g, ps, act, params, paramCount)) continue;
      int pIdx = 0;
      execActionPair(g, ps, act, params, pIdx, res, printStr);
      if (res.gameOver || res.quit) return;
    }
  }

  // -------------------------------------------------------------------
  // Built-in fallbacks for verbs the action table didn't handle
  // -------------------------------------------------------------------

  inline void builtinGo(const Game& g, PlayState& ps, int nounIdx,
                        ExecResult& res, PrintFn printStr)
  {
    if (nounIdx < 1 || nounIdx > 6)
    {
      if (printStr) { printStr("Go where?"); printStr("\n"); }
      return;
    }
    if (flagGet(ps, FLAG_DARK))
    {
      // In the dark, you might fall and break your neck — but that's
      // a game-action concern. Without one, just allow the move.
    }
    int dest = g.rooms[ps.curRoom].exits[nounIdx - 1];
    if (dest == 0)
    {
      if (printStr) { printStr("I can't go that way."); printStr("\n"); }
      return;
    }
    ps.curRoom = (uint8_t)dest;
    res.redrawRoom = true;
    res.acted = true;
  }

  inline void builtinGet(Game& g, const PlayState& ps,
                         const Word& noun, int wordLen,
                         ExecResult& res, PrintFn printStr)
  {
    int it = findItemAt(g, noun.text, ps.curRoom, wordLen);
    if (it < 0)
    {
      if (printStr) { printStr("I don't see that here."); printStr("\n"); }
      return;
    }
    if (countCarried(g) >= g.h.maxCarry)
    {
      if (printStr) { printStr("I've too much to carry."); printStr("\n"); }
      return;
    }
    moveItemTo(g, it, LOC_CARRIED);
    if (printStr) { printStr("OK."); printStr("\n"); }
    res.redrawRoom = true;
    res.acted = true;
  }

  inline void builtinDrop(Game& g, const PlayState& ps,
                          const Word& noun, int wordLen,
                          ExecResult& res, PrintFn printStr)
  {
    int it = findItemAt(g, noun.text, LOC_CARRIED, wordLen);
    if (it < 0)
    {
      if (printStr) { printStr("I'm not carrying that."); printStr("\n"); }
      return;
    }
    moveItemTo(g, it, ps.curRoom);
    if (printStr) { printStr("OK."); printStr("\n"); }
    res.redrawRoom = true;
    res.acted = true;
  }

  // -------------------------------------------------------------------
  // Per-turn entry point
  // -------------------------------------------------------------------

  inline ExecResult execTurn(Game& g, PlayState& ps,
                             int verbIdx, int nounIdx,
                             PrintFn printStr)
  {
    ExecResult res;

    // Auto-actions fire first, every turn, regardless of input.
    runAutoActions(g, ps, res, printStr);
    if (res.gameOver || res.quit) return res;

    // No player input? Just rendering check.
    if (verbIdx <= 0) return res;

    // Try the action table.
    bool handled = tryPlayerAction(g, ps, verbIdx, nounIdx, res, printStr);
    if (handled) return res;

    // Built-in fallbacks for the conventional verbs.
    if (verbIdx == VERB_GO)
    {
      builtinGo(g, ps, nounIdx, res, printStr);
      return res;
    }
    // Common verbs (GET / DROP / INV / LOOK / SCORE / QUIT) — recognize
    // by canonical-verb-text rather than index, since the index varies
    // between games.
    const char* vt = g.verbs[verbIdx].text;
    auto eq = [&](const char* kw) -> bool {
      for (int i = 0; i < g.h.wordLen; i++)
      {
        char a = (char)toupper((unsigned char)vt[i]);
        char b = (char)toupper((unsigned char)kw[i]);
        if (a != b) return false;
        if (a == '\0') return true;
      }
      return true;
    };

    if (eq("GET") || eq("TAK"))
    {
      if (nounIdx <= 0)
      {
        if (printStr) { printStr("Get what?"); printStr("\n"); }
        return res;
      }
      builtinGet(g, ps, g.nouns[nounIdx], g.h.wordLen, res, printStr);
      return res;
    }
    if (eq("DRO"))
    {
      if (nounIdx <= 0)
      {
        if (printStr) { printStr("Drop what?"); printStr("\n"); }
        return res;
      }
      builtinDrop(g, ps, g.nouns[nounIdx], g.h.wordLen, res, printStr);
      return res;
    }
    if (eq("INV"))
    {
      printInventory(g, printStr);
      res.acted = true;
      return res;
    }
    if (eq("LOO"))
    {
      res.redrawRoom = true;
      res.acted = true;
      return res;
    }
    if (eq("SCO"))
    {
      printScore(g, ps, printStr);
      res.acted = true;
      return res;
    }
    if (eq("QUI"))
    {
      res.quit = true;
      return res;
    }

    if (printStr) { printStr("I don't understand."); printStr("\n"); }
    return res;
  }

  // -------------------------------------------------------------------
  // Light-source bookkeeping (called once per turn after execTurn)
  // -------------------------------------------------------------------

  // Decrement light timer if the lamp is lit (flag 0). When it
  // expires, print a warning and clear the lit flag — a subsequent
  // auto-action in dark games handles the "blackout" behavior.
  inline void tickLight(Game& g, PlayState& ps, PrintFn printStr)
  {
    if (!flagGet(ps, 0)) return;
    if (ps.lightLeft == 0) return;
    ps.lightLeft--;
    if (ps.lightLeft == 0)
    {
      flagClear(ps, 0);
      if (printStr) { printStr("Light has run out."); printStr("\n"); }
    }
    else if (ps.lightLeft < 25 && (ps.lightLeft % 5) == 0)
    {
      char buf[40];
      snprintf(buf, sizeof(buf), "Light runs out in %u turns.",
               (unsigned)ps.lightLeft);
      if (printStr) { printStr(buf); printStr("\n"); }
    }
    (void)g;
  }

}  // namespace scott
