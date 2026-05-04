/*
 * Scott Adams .DAT file format types and parser.
 *
 * The format is plain ASCII: whitespace-separated integer tokens
 * interleaved with double-quoted string tokens. Strings may span
 * multiple physical lines. There are no escape sequences in the
 * source — a literal newline byte inside the quotes is part of the
 * string and renders as a newline in the game.
 *
 * Layout:
 *   1. Header                — 12 ints
 *   2. Actions               — (numActions+1) × 8 ints
 *   3. Vocabulary            — (numWords+1) × 2 strings (verb, noun)
 *   4. Rooms                 — (numRooms+1) × (6 ints + 1 string)
 *   5. Messages              — (numMessages+1) strings
 *   6. Items                 — (numItems+1) × (1 string + 1 int)
 *   7. Action comments       — (numActions+1) strings (ignored)
 *   8. Trailer               — 3 ints (version, adv-number, magic)
 *
 * Implementation notes:
 *   - Storage is allocated from PSRAM (ps_malloc / ps_strdup-style).
 *     Adventures are 5-15 KB on disk and well under 100 KB in RAM,
 *     so PSRAM at 8 MB has zero pressure.
 *   - Words can be longer than the game's wordLen (e.g. "NORTH" in
 *     a wordLen=3 game). We store up to 7 source chars; the matcher
 *     compares the first wordLen of each side.
 *   - Synonyms have a leading '*' and inherit the meaning of the
 *     previous non-'*' word *of the same type* (verb-with-* binds to
 *     previous verb, noun-with-* to previous noun). The parser keeps
 *     the '*' prefix so the executor can resolve at lookup time.
 *
 * No GPL code from ScottFree or other interpreters has been copied.
 * The format details encoded here are taken from the openly-published
 * Scott Adams .DAT format spec and verified against actual .DAT bytes.
 */

#pragma once

#include <Arduino.h>
#include <FS.h>

namespace scott
{
  // -------------------------------------------------------------------
  // Data structures
  // -------------------------------------------------------------------

  struct Header
  {
    uint16_t unknown;         // first int — purpose unclear; not used
    uint16_t numItems;        // last item index; total = numItems + 1
    uint16_t numActions;      // last action index
    uint16_t numWords;        // last verb/noun index (verbs == nouns)
    uint16_t numRooms;        // last room index (room 0 is the "store")
    uint16_t maxCarry;        // max objects player can hold
    uint16_t startRoom;       // initial player room
    uint16_t numTreasures;    // for scoring; treasures' descriptions start with '*'
    uint16_t wordLen;         // # chars compared during parsing (typ. 3 or 4)
    uint16_t lightTime;       // turns lamp lasts
    uint16_t numMessages;     // last message index
    uint16_t treasureRoom;    // drop treasures here to score
  };

  struct Action
  {
    uint16_t verbNoun;        // verb*150 + noun (0,0 = auto-action)
    uint16_t cond[5];         // each = arg*20 + opcode (opcode 0..19)
    uint16_t actionPair[2];   // each = a*150 + b (4 sub-actions total)
  };

  struct Word
  {
    char text[8];             // up to 7 chars + NUL
  };

  struct Room
  {
    uint8_t exits[6];         // N, S, E, W, U, D — room number, 0 = no exit
    char*   description;      // PSRAM-allocated, NUL-terminated
  };

  struct Item
  {
    char*   description;      // PSRAM-allocated; if starts with '*' = treasure
    char    autoNoun[8];      // pulled from /WORD/ marker, used for AUTO GET/DROP
    uint8_t startLoc;         // initial room (or 255 = carried, 0 = store)
    uint8_t curLoc;           // current room (mutated during play)
  };

  // -------------------------------------------------------------------
  // Game — owns all parsed state and the PSRAM allocations
  // -------------------------------------------------------------------

  class Game
  {
  public:
    Header  h{};
    Action* actions  = nullptr;   // numActions + 1
    Word*   verbs    = nullptr;   // numWords + 1
    Word*   nouns    = nullptr;   // numWords + 1
    Room*   rooms    = nullptr;   // numRooms + 1
    char**  messages = nullptr;   // numMessages + 1
    Item*   items    = nullptr;   // numItems + 1

    // Trailer ints (informational)
    int     trailerVersion = 0;
    int     trailerAdvNum  = 0;
    int     trailerMagic   = 0;

    Game() {}
    ~Game() { freeAll(); }

    // Free everything and reset back to a defaulted state.
    void freeAll();

    // Parse the named .DAT file. Returns true on success; on failure,
    // err[] (if non-null) gets a short human-readable reason.
    bool load(fs::FS& fs, const char* path, char* err, size_t errSize);

  private:
    // No copy/move — owns heap pointers.
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;
  };

  // -------------------------------------------------------------------
  // Allocation helpers — all from PSRAM
  // -------------------------------------------------------------------

  inline void* psAlloc(size_t bytes)
  {
    void* p = ps_malloc(bytes);
    if (!p) p = malloc(bytes);   // fall back to internal heap
    return p;
  }

  inline char* psStrdup(const char* s, size_t n)
  {
    char* out = (char*)psAlloc(n + 1);
    if (!out) return nullptr;
    if (n > 0) memcpy(out, s, n);
    out[n] = '\0';
    return out;
  }

  // -------------------------------------------------------------------
  // Tokenizer over an open File
  // -------------------------------------------------------------------

  // Buffered read-one-char-at-a-time over a File. We hold a small
  // ahead-read buffer so peek() doesn't require an actual seek.
  class Reader
  {
  public:
    Reader(File& f) : m_f(f) {}

    int peek()
    {
      if (m_havePeek) return m_peekCh;
      int c = m_f.read();
      m_peekCh  = c;
      m_havePeek = true;
      return c;
    }

    int next()
    {
      if (m_havePeek)
      {
        m_havePeek = false;
        return m_peekCh;
      }
      return m_f.read();
    }

    bool eof() { return peek() < 0; }

  private:
    File& m_f;
    bool  m_havePeek = false;
    int   m_peekCh   = -1;
  };

  inline bool isWhite(int c)
  {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  }

  inline void skipWhite(Reader& r)
  {
    while (!r.eof() && isWhite(r.peek())) r.next();
  }

  // Read one signed integer. Returns true on success.
  inline bool readInt(Reader& r, int& out)
  {
    skipWhite(r);
    if (r.eof()) return false;
    int sign = 1;
    int c = r.peek();
    if (c == '-') { sign = -1; r.next(); }
    else if (c == '+') { r.next(); }

    if (!isdigit(r.peek())) return false;
    long v = 0;
    while (!r.eof() && isdigit(r.peek()))
    {
      v = v * 10 + (r.next() - '0');
    }
    out = (int)(v * sign);
    return true;
  }

  // Read a double-quoted string into a freshly-allocated PSRAM buffer.
  // Returns nullptr on error or if no quoted string is next. Strings
  // may contain literal newlines.
  inline char* readQuotedString(Reader& r)
  {
    skipWhite(r);
    if (r.eof() || r.peek() != '"') return nullptr;
    r.next();   // consume opening "

    // First pass: scan to find length so we can allocate exactly once.
    // Since File doesn't support cheap rewind on PSRAM-backed FS, we
    // grow a buffer instead.
    size_t cap  = 64;
    size_t used = 0;
    char* buf = (char*)psAlloc(cap);
    if (!buf) return nullptr;

    while (!r.eof())
    {
      int c = r.next();
      if (c == '"') break;
      if (used + 1 >= cap)
      {
        size_t nc = cap * 2;
        char* nb = (char*)psAlloc(nc);
        if (!nb) { free(buf); return nullptr; }
        memcpy(nb, buf, used);
        free(buf);
        buf = nb;
        cap = nc;
      }
      buf[used++] = (char)c;
    }
    buf[used] = '\0';
    return buf;
  }

  // Same as readQuotedString but copies into a fixed-size buffer.
  // Pads the destination with NUL. Returns false on error.
  inline bool readQuotedFixed(Reader& r, char* out, size_t outSize)
  {
    skipWhite(r);
    if (r.eof() || r.peek() != '"') return false;
    r.next();

    size_t i = 0;
    while (!r.eof())
    {
      int c = r.next();
      if (c == '"') break;
      if (i + 1 < outSize) out[i++] = (char)c;
      // else: keep consuming so we land past the closing quote
    }
    while (i < outSize) out[i++] = '\0';
    return true;
  }

  // Read a quoted string and discard the contents (used for action
  // comments). Returns false on error.
  inline bool skipQuotedString(Reader& r)
  {
    skipWhite(r);
    if (r.eof() || r.peek() != '"') return false;
    r.next();
    while (!r.eof())
    {
      int c = r.next();
      if (c == '"') return true;
    }
    return false;
  }

  // -------------------------------------------------------------------
  // Game::load
  // -------------------------------------------------------------------

  inline void Game::freeAll()
  {
    if (actions) { free(actions); actions = nullptr; }
    if (verbs)   { free(verbs);   verbs   = nullptr; }
    if (nouns)   { free(nouns);   nouns   = nullptr; }

    if (rooms)
    {
      for (int i = 0; i <= h.numRooms; i++)
      {
        if (rooms[i].description) free(rooms[i].description);
      }
      free(rooms);
      rooms = nullptr;
    }
    if (messages)
    {
      for (int i = 0; i <= h.numMessages; i++)
      {
        if (messages[i]) free(messages[i]);
      }
      free(messages);
      messages = nullptr;
    }
    if (items)
    {
      for (int i = 0; i <= h.numItems; i++)
      {
        if (items[i].description) free(items[i].description);
      }
      free(items);
      items = nullptr;
    }
    h = Header{};
    trailerVersion = trailerAdvNum = trailerMagic = 0;
  }

  // Pull /WORD/ marker out of an item description (used for AUTO GET).
  // Modifies desc in place by removing the marker; copies the word
  // (up to 7 chars) into outNoun.
  inline void extractAutoNoun(char* desc, char* outNoun)
  {
    outNoun[0] = '\0';
    if (!desc) return;
    char* slash1 = strchr(desc, '/');
    if (!slash1) return;
    char* slash2 = strchr(slash1 + 1, '/');
    if (!slash2) return;

    size_t wlen = (size_t)(slash2 - slash1 - 1);
    if (wlen == 0) return;
    if (wlen >= 8) wlen = 7;
    memcpy(outNoun, slash1 + 1, wlen);
    outNoun[wlen] = '\0';

    // Remove the "/WORD/" segment from the description.
    memmove(slash1, slash2 + 1, strlen(slash2 + 1) + 1);
  }

  inline bool Game::load(fs::FS& fs, const char* path,
                         char* err, size_t errSize)
  {
    auto fail = [&](const char* msg) -> bool {
      if (err && errSize > 0) snprintf(err, errSize, "%s", msg);
      freeAll();
      return false;
    };

    freeAll();

    File f = fs.open(path, FILE_READ);
    if (!f) return fail("open failed");
    if (f.isDirectory()) { f.close(); return fail("is a directory"); }

    Reader r(f);
    int v;

    // 1) Header — 12 ints
    int hdr[12];
    for (int i = 0; i < 12; i++)
    {
      if (!readInt(r, hdr[i])) { f.close(); return fail("short header"); }
    }
    h.unknown      = (uint16_t)hdr[0];
    h.numItems     = (uint16_t)hdr[1];
    h.numActions   = (uint16_t)hdr[2];
    h.numWords     = (uint16_t)hdr[3];
    h.numRooms     = (uint16_t)hdr[4];
    h.maxCarry     = (uint16_t)hdr[5];
    h.startRoom    = (uint16_t)hdr[6];
    h.numTreasures = (uint16_t)hdr[7];
    h.wordLen      = (uint16_t)hdr[8];
    h.lightTime    = (uint16_t)hdr[9];
    h.numMessages  = (uint16_t)hdr[10];
    h.treasureRoom = (uint16_t)hdr[11];

    // Sanity check on the wordLen — corrupt files would otherwise
    // make us spin reading garbage.
    if (h.wordLen < 1 || h.wordLen > 10) { f.close(); return fail("bad wordLen"); }
    if (h.numItems > 4096 || h.numActions > 8192 ||
        h.numWords > 1024 || h.numRooms > 1024 ||
        h.numMessages > 4096)
    {
      f.close();
      return fail("counts out of range");
    }

    // 2) Actions
    actions = (Action*)psAlloc(sizeof(Action) * (h.numActions + 1));
    if (!actions) { f.close(); return fail("oom actions"); }
    for (int i = 0; i <= h.numActions; i++)
    {
      if (!readInt(r, v)) { f.close(); return fail("short actions"); }
      actions[i].verbNoun = (uint16_t)v;
      for (int c = 0; c < 5; c++)
      {
        if (!readInt(r, v)) { f.close(); return fail("short conds"); }
        actions[i].cond[c] = (uint16_t)v;
      }
      for (int a = 0; a < 2; a++)
      {
        if (!readInt(r, v)) { f.close(); return fail("short acts"); }
        actions[i].actionPair[a] = (uint16_t)v;
      }
    }

    // 3) Vocabulary — interleaved (verb, noun) pairs
    verbs = (Word*)psAlloc(sizeof(Word) * (h.numWords + 1));
    nouns = (Word*)psAlloc(sizeof(Word) * (h.numWords + 1));
    if (!verbs || !nouns) { f.close(); return fail("oom words"); }
    for (int i = 0; i <= h.numWords; i++)
    {
      if (!readQuotedFixed(r, verbs[i].text, sizeof(verbs[i].text)))
      {
        f.close(); return fail("short verbs");
      }
      if (!readQuotedFixed(r, nouns[i].text, sizeof(nouns[i].text)))
      {
        f.close(); return fail("short nouns");
      }
    }

    // 4) Rooms — 6 exits + description string
    rooms = (Room*)psAlloc(sizeof(Room) * (h.numRooms + 1));
    if (!rooms) { f.close(); return fail("oom rooms"); }
    for (int i = 0; i <= h.numRooms; i++)
    {
      for (int e = 0; e < 6; e++)
      {
        if (!readInt(r, v)) { f.close(); return fail("short room exits"); }
        rooms[i].exits[e] = (uint8_t)v;
      }
      rooms[i].description = readQuotedString(r);
      if (!rooms[i].description) { f.close(); return fail("short room desc"); }
    }

    // 5) Messages
    messages = (char**)psAlloc(sizeof(char*) * (h.numMessages + 1));
    if (!messages) { f.close(); return fail("oom messages"); }
    for (int i = 0; i <= h.numMessages; i++) messages[i] = nullptr;
    for (int i = 0; i <= h.numMessages; i++)
    {
      messages[i] = readQuotedString(r);
      if (!messages[i]) { f.close(); return fail("short messages"); }
    }

    // 6) Items — description + initial location
    items = (Item*)psAlloc(sizeof(Item) * (h.numItems + 1));
    if (!items) { f.close(); return fail("oom items"); }
    for (int i = 0; i <= h.numItems; i++)
    {
      items[i].description = nullptr;
      items[i].autoNoun[0] = '\0';
      items[i].startLoc    = 0;
      items[i].curLoc      = 0;
    }
    for (int i = 0; i <= h.numItems; i++)
    {
      items[i].description = readQuotedString(r);
      if (!items[i].description) { f.close(); return fail("short item desc"); }
      extractAutoNoun(items[i].description, items[i].autoNoun);

      if (!readInt(r, v)) { f.close(); return fail("short item loc"); }
      items[i].startLoc = (uint8_t)v;
      items[i].curLoc   = (uint8_t)v;
    }

    // 7) Action title comments — read and discard
    for (int i = 0; i <= h.numActions; i++)
    {
      if (!skipQuotedString(r)) { f.close(); return fail("short action titles"); }
    }

    // 8) Trailer — 3 ints
    if (!readInt(r, trailerVersion)) { f.close(); return fail("short trailer"); }
    if (!readInt(r, trailerAdvNum))  { f.close(); return fail("short trailer"); }
    if (!readInt(r, trailerMagic))   { f.close(); return fail("short trailer"); }

    f.close();
    return true;
  }

}  // namespace scott
