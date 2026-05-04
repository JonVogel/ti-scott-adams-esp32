/*
 * TI Extended BASIC Simulator v0.2
 * For ESP32-S3-USB-OTG
 *
 * Refactored architecture:
 *   - Execution Manager (EM): handles program flow
 *   - Token Parser (TP): state machine interprets tokens
 *   - Expression Parser: evaluates arithmetic/logical expressions
 *   - Variable Table: manages numeric and string variables
 *
 * Display: 28 columns x 24 rows (8x8 pixel characters)
 * Storage: Programs tokenized in RAM, saved as text to LittleFS
 *
 * Board settings (Arduino IDE):
 *   Board: "ESP32S3 Dev Module"
 *   Partition: Custom (8MB with SPIFFS)
 *   USB CDC On Boot: "Enabled"
 */

#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include "rgb_db.h"
#include "ti_font.h"
#include "ble_keyboard.h"
#include "exec_manager.h"
#include "file_io.h"
#include "sprites.h"
#include "line_editor.h"

// ---------------------------------------------------------------------------
// ESP32-8048S043C (Sunton 4.3" 800x480 RGB) pin map
// ---------------------------------------------------------------------------
#define TFT_BL 2

// Display geometry — authentic TI 32x24 at 16x16 pixels per char.
// 32 * 16 = 512 wide, 24 * 16 = 384 tall; centered on the 800x480 panel.
#define COLS       32
#define ROWS       24
#define CHAR_W     16
#define CHAR_H     16
#define SCREEN_W   800
#define SCREEN_H   480

#define DISPLAY_X_OFFSET ((SCREEN_W - COLS * CHAR_W) / 2)   // 144
// 8-px upward nudge from the math-centered position (was 48) to better
// center the TI grid on the physical 8048S043C with rotation 2.
#define DISPLAY_Y_OFFSET (((SCREEN_H - ROWS * CHAR_H) / 2) - 8)   // 40

// TI-style screen-color border is a thin 8-pixel frame around the grid.
// Everything outside that frame (except the status line) is black.
#define BORDER_W          8
#define GRID_BOTTOM_Y     (DISPLAY_Y_OFFSET + ROWS * CHAR_H)  // 432
#define STATUS_Y          456
#define STATUS_H          (SCREEN_H - STATUS_Y)               // 24

// ---------------------------------------------------------------------------
// Keyword table — maps text to tokens (used by tokenizer)
// ---------------------------------------------------------------------------

struct KeywordEntry
{
  const char* text;
  Token token;
};

// Token → canonical text. Indexed directly by token value (0-255).
//
//   0x00        → NULL (TOK_EOL, no output)
//   0x01-0x09   → immediate-command names (CONTINUE, LIST, ...)
//   0x0A-0x1F   → NULL (control chars, shouldn't appear in the stream)
//   0x20-0x7F   → single-character strings (raw ASCII — variable names,
//                 digits, punctuation)
//   0x80-0xFF   → keyword / function / operator names, or NULL for
//                 tokens handled specially (literals, !-comments) or
//                 unused token values
//
// Populated once at startup by initTokenNames(). The single-char strings
// for the ASCII range live in asciiOneChar[].
static const char* tokenNames[256] = { NULL };
static char asciiOneChar[128][2];

static void initTokenNames()
{
  // Fill the printable-ASCII range with one-char strings so identifiers
  // and punctuation bytes in the token stream round-trip directly.
  for (int i = 0x20; i < 0x80; i++)
  {
    asciiOneChar[i][0] = (char)i;
    asciiOneChar[i][1] = '\0';
    tokenNames[i] = asciiOneChar[i];
  }

  // Immediate commands 0x00-0x09
  tokenNames[TOK_CONTINUE] = "CONTINUE";
  tokenNames[TOK_LIST]     = "LIST";
  tokenNames[TOK_BYE]      = "BYE";
  tokenNames[TOK_NUM]      = "NUMBER";
  tokenNames[TOK_OLD]      = "OLD";
  tokenNames[TOK_RES]      = "RESEQUENCE";
  tokenNames[TOK_SAVE]     = "SAVE";
  tokenNames[TOK_MERGE]    = "MERGE";
  tokenNames[TOK_EDIT]     = "EDIT";

  // Statements 0x81-0xAA
  tokenNames[TOK_ELSE]     = "ELSE";
  tokenNames[TOK_DCOLON]   = "::";
  tokenNames[TOK_BANG]     = "!";
  tokenNames[TOK_IF]       = "IF";
  tokenNames[TOK_GO]       = "GO";
  tokenNames[TOK_GOTO]     = "GOTO";
  tokenNames[TOK_GOSUB]    = "GOSUB";
  tokenNames[TOK_RETURN]   = "RETURN";
  tokenNames[TOK_DEF]      = "DEF";
  tokenNames[TOK_DIM]      = "DIM";
  tokenNames[TOK_END]      = "END";
  tokenNames[TOK_FOR]      = "FOR";
  tokenNames[TOK_LET]      = "LET";
  tokenNames[TOK_BREAK]    = "BREAK";
  tokenNames[TOK_UNBREAK]  = "UNBREAK";
  tokenNames[TOK_TRACE]    = "TRACE";
  tokenNames[TOK_UNTRACE]  = "UNTRACE";
  tokenNames[TOK_INPUT]    = "INPUT";
  tokenNames[TOK_DATA]     = "DATA";
  tokenNames[TOK_RESTORE]  = "RESTORE";
  tokenNames[TOK_RANDOMIZE]= "RANDOMIZE";
  tokenNames[TOK_NEXT]     = "NEXT";
  tokenNames[TOK_READ]     = "READ";
  tokenNames[TOK_STOP]     = "STOP";
  tokenNames[TOK_DELETE]   = "DELETE";
  tokenNames[TOK_REM]      = "REM";
  tokenNames[TOK_ON]       = "ON";
  tokenNames[TOK_PRINT]    = "PRINT";
  tokenNames[TOK_CALL]     = "CALL";
  tokenNames[TOK_OPTION]   = "OPTION";
  tokenNames[TOK_OPEN]     = "OPEN";
  tokenNames[TOK_CLOSE]    = "CLOSE";
  tokenNames[TOK_SUB]      = "SUB";
  tokenNames[TOK_DISPLAY]  = "DISPLAY";
  tokenNames[TOK_IMAGE]    = "IMAGE";
  tokenNames[TOK_ACCEPT]   = "ACCEPT";
  tokenNames[TOK_ERROR]    = "ERROR";
  tokenNames[TOK_WARNING]  = "WARNING";
  tokenNames[TOK_SUBEXIT]  = "SUBEXIT";
  tokenNames[TOK_SUBEND]   = "SUBEND";
  tokenNames[TOK_RUN]      = "RUN";
  tokenNames[TOK_LINPUT]   = "LINPUT";

  // Secondary keywords + punctuation 0xB0-0xB8
  tokenNames[TOK_THEN]      = "THEN";
  tokenNames[TOK_TO]        = "TO";
  tokenNames[TOK_STEP]      = "STEP";
  tokenNames[TOK_COMMA]     = ",";
  tokenNames[TOK_SEMICOLON] = ";";
  tokenNames[TOK_COLON]     = ":";
  tokenNames[TOK_RPAREN]    = ")";
  tokenNames[TOK_LPAREN]    = "(";
  tokenNames[TOK_CONCAT]    = "&";

  // Operators 0xBA-0xC5
  tokenNames[TOK_OR]       = "OR";
  tokenNames[TOK_AND]      = "AND";
  tokenNames[TOK_XOR]      = "XOR";
  tokenNames[TOK_NOT]      = "NOT";
  tokenNames[TOK_EQUAL]    = "=";
  tokenNames[TOK_LESS]     = "<";
  tokenNames[TOK_GREATER]  = ">";
  tokenNames[TOK_PLUS]     = "+";
  tokenNames[TOK_MINUS]    = "-";
  tokenNames[TOK_MULTIPLY] = "*";
  tokenNames[TOK_DIVIDE]   = "/";
  tokenNames[TOK_POWER]    = "^";

  // Functions 0xCB-0xE1
  tokenNames[TOK_ABS] = "ABS"; tokenNames[TOK_ATN] = "ATN";
  tokenNames[TOK_COS] = "COS"; tokenNames[TOK_EXP] = "EXP";
  tokenNames[TOK_INT] = "INT"; tokenNames[TOK_LOG] = "LOG";
  tokenNames[TOK_SGN] = "SGN"; tokenNames[TOK_SIN] = "SIN";
  tokenNames[TOK_SQR] = "SQR"; tokenNames[TOK_TAN] = "TAN";
  tokenNames[TOK_LEN] = "LEN"; tokenNames[TOK_CHR] = "CHR$";
  tokenNames[TOK_RND] = "RND"; tokenNames[TOK_SEG] = "SEG$";
  tokenNames[TOK_POS] = "POS"; tokenNames[TOK_VAL] = "VAL";
  tokenNames[TOK_STR] = "STR$";tokenNames[TOK_ASC] = "ASC";
  tokenNames[TOK_PI]  = "PI";  tokenNames[TOK_REC] = "REC";
  tokenNames[TOK_MAX] = "MAX"; tokenNames[TOK_MIN] = "MIN";
  tokenNames[TOK_RPT] = "RPT$";

  // Extended BASIC keywords 0xE8-0xFE
  tokenNames[TOK_NUMERIC]     = "NUMERIC";
  tokenNames[TOK_DIGIT]       = "DIGIT";
  tokenNames[TOK_UALPHA]      = "UALPHA";
  tokenNames[TOK_SIZE]        = "SIZE";
  tokenNames[TOK_ALL]         = "ALL";
  tokenNames[TOK_USING]       = "USING";
  tokenNames[TOK_BEEP]        = "BEEP";
  tokenNames[TOK_ERASE]       = "ERASE";
  tokenNames[TOK_AT]          = "AT";
  tokenNames[TOK_BASE]        = "BASE";
  tokenNames[TOK_VARIABLE_KW] = "VARIABLE";
  tokenNames[TOK_RELATIVE]    = "RELATIVE";
  tokenNames[TOK_INTERNAL]    = "INTERNAL";
  tokenNames[TOK_SEQUENTIAL]  = "SEQUENTIAL";
  tokenNames[TOK_OUTPUT]      = "OUTPUT";
  tokenNames[TOK_UPDATE]      = "UPDATE";
  tokenNames[TOK_APPEND]      = "APPEND";
  tokenNames[TOK_FIXED]       = "FIXED";
  tokenNames[TOK_PERMANENT]   = "PERMANENT";
  tokenNames[TOK_TAB]         = "TAB";
  tokenNames[TOK_HASH]        = "#";
  tokenNames[TOK_VALIDATE]    = "VALIDATE";
}

// Tokenizer keyword table — text → token, including aliases.
// Used only for matching source text during tokenization.
static const KeywordEntry keywords[] =
{
  // RUN, NEW, DIR are handled as pre-tokenize string commands
  // (so they're not listed here)
  {"LIST",       TOK_LIST},
  {"OLD",        TOK_OLD},
  {"SAVE",       TOK_SAVE},
  {"BYE",        TOK_BYE},
  {"NUMBER",     TOK_NUM},
  {"NUM",        TOK_NUM},
  {"PRINT",      TOK_PRINT},
  {"USING",      TOK_USING},
  {"DISPLAY",    TOK_DISPLAY},
  {"ACCEPT",     TOK_ACCEPT},
  {"GOTO",       TOK_GOTO},
  {"GO TO",      TOK_GOTO},
  {"GOSUB",      TOK_GOSUB},
  {"RETURN",     TOK_RETURN},
  {"IF",         TOK_IF},
  {"THEN",       TOK_THEN},
  {"ELSE",       TOK_ELSE},
  {"FOR",        TOK_FOR},
  {"TO",         TOK_TO},
  {"STEP",       TOK_STEP},
  {"NEXT",       TOK_NEXT},
  {"LET",        TOK_LET},
  {"INPUT",      TOK_INPUT},
  {"LINPUT",     TOK_LINPUT},
  {"DIM",        TOK_DIM},
  {"REM",        TOK_REM},
  {"END",        TOK_END},
  {"STOP",       TOK_STOP},
  {"DATA",       TOK_DATA},
  {"READ",       TOK_READ},
  {"RESTORE",    TOK_RESTORE},
  {"RANDOMIZE",  TOK_RANDOMIZE},
  {"DEF",        TOK_DEF},
  {"ON",         TOK_ON},
  {"OPTION",     TOK_OPTION},
  {"BASE",       TOK_BASE},
  {"BREAK",      TOK_BREAK},
  {"UNBREAK",    TOK_UNBREAK},
  {"ERROR",      TOK_ERROR},
  {"WARNING",    TOK_WARNING},
  {"CONTINUE",   TOK_CONTINUE},
  {"CON",        TOK_CONTINUE},
  {"RESEQUENCE", TOK_RES},
  {"RES",        TOK_RES},
  {"SIZE",       TOK_SIZE},
  {"MERGE",      TOK_MERGE},
  {"CALL",       TOK_CALL},
  {"SUB",        TOK_SUB},
  {"SUBEND",     TOK_SUBEND},
  {"SUBEXIT",    TOK_SUBEXIT},
  {"OPEN",       TOK_OPEN},
  {"CLOSE",      TOK_CLOSE},
  {"OUTPUT",     TOK_OUTPUT},
  {"UPDATE",     TOK_UPDATE},
  {"APPEND",     TOK_APPEND},
  {"SEQUENTIAL", TOK_SEQUENTIAL},
  {"RELATIVE",   TOK_RELATIVE},
  {"INTERNAL",   TOK_INTERNAL},
  {"FIXED",      TOK_FIXED},
  {"PERMANENT",  TOK_PERMANENT},
  {"VARIABLE",   TOK_VARIABLE_KW},
  {"REC",        TOK_REC},
  {"DELETE",     TOK_DELETE},
  {"IMAGE",      TOK_IMAGE},
  {"TRACE",      TOK_TRACE},
  {"UNTRACE",    TOK_UNTRACE},
  {"AND",        TOK_AND},
  {"OR",         TOK_OR},
  {"XOR",        TOK_XOR},
  {"NOT",        TOK_NOT},
  {NULL, TOK_EOL}
};

// ---------------------------------------------------------------------------
// Tokenizer (converts text to tokens)
// ---------------------------------------------------------------------------

static int skipSpaces(const char* src, int pos)
{
  while (src[pos] == ' ')
  {
    pos++;
  }
  return pos;
}

static int matchKeyword(const char* src, int pos)
{
  int bestMatch = -1;
  int bestLen = 0;

  for (int i = 0; keywords[i].text != NULL; i++)
  {
    const char* kw = keywords[i].text;
    int klen = strlen(kw);
    bool match = true;

    for (int j = 0; j < klen; j++)
    {
      if (toupper(src[pos + j]) != kw[j])
      {
        match = false;
        break;
      }
    }

    if (match && klen > bestLen)
    {
      char next = src[pos + klen];
      if (next == '\0' || !isalnum(next))
      {
        bestMatch = i;
        bestLen = klen;
      }
    }
  }

  return bestMatch;
}

// True when the next number literal should be stored as TI-style
// TOK_LINENUM (0xC9 + big-endian word), e.g. the target of a GOTO.
static bool isLineRefToken(uint8_t tok)
{
  return tok == TOK_GOTO  || tok == TOK_GOSUB  || tok == TOK_THEN ||
         tok == TOK_ELSE  || tok == TOK_RESTORE|| tok == TOK_BREAK ||
         tok == TOK_UNBREAK || tok == TOK_RES  || tok == TOK_ERROR;
}

// Keywords that accept a single file-spec argument where the lexer
// should treat the remainder of the statement as one opaque string
// (TI-style: OLD DSK1.NAME does NOT need quotes).
static bool isFilenameKwToken(uint8_t tok)
{
  return tok == TOK_OLD   || tok == TOK_SAVE  ||
         tok == TOK_MERGE || tok == TOK_DELETE;
}

static int tokenizeLine(const char* src, uint8_t* tokens, int maxLen)
{
  int pos = 0;
  int out = 0;
  bool expectLineNum = false;

  while (src[pos] != '\0')
  {
    pos = skipSpaces(src, pos);
    if (src[pos] == '\0')
    {
      break;
    }

    // REM or IMAGE — rest of line is literal text. (IMAGE captures the
    // format string verbatim so it can be looked up later by
    // PRINT USING <lineN>.)
    if (out > 0 &&
        (tokens[out - 1] == TOK_REM || tokens[out - 1] == TOK_IMAGE))
    {
      int remLen = strlen(&src[pos]);
      if (out + 2 + remLen >= maxLen)
      {
        return -1;
      }
      tokens[out++] = TOK_STRING_LIT;
      tokens[out++] = (uint8_t)remLen;
      memcpy(&tokens[out], &src[pos], remLen);
      out += remLen;
      break;
    }

    // ! — TI Extended BASIC tail comment. Stored under its own token so
    // LIST round-trips back to `!` instead of REM.
    if (src[pos] == '!')
    {
      pos++;
      int remLen = strlen(&src[pos]);
      if (out + 3 + remLen >= maxLen)
      {
        return -1;
      }
      tokens[out++] = TOK_BANG;
      tokens[out++] = TOK_STRING_LIT;
      tokens[out++] = (uint8_t)remLen;
      memcpy(&tokens[out], &src[pos], remLen);
      out += remLen;
      break;
    }

    // Quoted string
    if (src[pos] == '"')
    {
      pos++;
      int start = pos;
      while (src[pos] != '\0' && src[pos] != '"')
      {
        pos++;
      }
      int slen = pos - start;
      if (src[pos] == '"')
      {
        pos++;
      }
      if (out + 2 + slen >= maxLen)
      {
        return -1;
      }
      tokens[out++] = TOK_STRING_LIT;
      tokens[out++] = (uint8_t)slen;
      memcpy(&tokens[out], &src[start], slen);
      out += slen;
      continue;
    }

    // Number literal. In a line-reference context (after GOTO / GOSUB /
    // THEN / ELSE / RESTORE / BREAK / UNBREAK / RESEQUENCE / ON ERROR,
    // and across commas in those lists) we encode it TI-style as
    // TOK_LINENUM + 2 bytes big-endian. Otherwise it's TOK_UNQUOTED_STR
    // + ASCII digits so the expression evaluator can parse floats.
    if (isdigit(src[pos]) || (src[pos] == '.' && isdigit(src[pos + 1])))
    {
      int start = pos;
      while (isdigit(src[pos]) || src[pos] == '.' || src[pos] == 'E' ||
             src[pos] == 'e' ||
             ((src[pos] == '+' || src[pos] == '-') &&
              (src[pos - 1] == 'E' || src[pos - 1] == 'e')))
      {
        pos++;
      }
      int slen = pos - start;
      if (slen > 255 || out + 2 + slen >= maxLen)
      {
        return -1;
      }

      // Reject floats in line-ref context (line numbers are integers).
      bool hasDot = false;
      for (int i = 0; i < slen; i++)
      {
        if (src[start + i] == '.' || src[start + i] == 'E' ||
            src[start + i] == 'e') { hasDot = true; break; }
      }

      if (expectLineNum && !hasDot)
      {
        char buf[8] = {0};
        int copy = (slen < 6) ? slen : 6;
        memcpy(buf, &src[start], copy);
        unsigned long ln = strtoul(buf, NULL, 10);
        if (ln > 65535) ln = 65535;
        if (out + 3 >= maxLen) return -1;
        tokens[out++] = TOK_LINENUM;
        tokens[out++] = (uint8_t)((ln >> 8) & 0xFF);
        tokens[out++] = (uint8_t)(ln & 0xFF);
        expectLineNum = false;
        continue;
      }

      tokens[out++] = TOK_UNQUOTED_STR;
      tokens[out++] = (uint8_t)slen;
      memcpy(&tokens[out], &src[start], slen);
      out += slen;
      expectLineNum = false;
      continue;
    }

    // Operators and punctuation
    bool foundOp = true;
    bool opKeepsLineRef = false;   // only TOK_COMMA preserves expectLineNum
    switch (src[pos])
    {
      case '+':  tokens[out++] = TOK_PLUS;       pos++; break;
      case '-':  tokens[out++] = TOK_MINUS;      pos++; break;
      case '*':  tokens[out++] = TOK_MULTIPLY;   pos++; break;
      case '/':  tokens[out++] = TOK_DIVIDE;     pos++; break;
      case '^':  tokens[out++] = TOK_POWER;      pos++; break;
      case '&':  tokens[out++] = TOK_CONCAT;     pos++; break;
      case '(':  tokens[out++] = TOK_LPAREN;     pos++; break;
      case ')':  tokens[out++] = TOK_RPAREN;     pos++; break;
      case ',':  tokens[out++] = TOK_COMMA;      pos++; opKeepsLineRef = true; break;
      case ';':  tokens[out++] = TOK_SEMICOLON;  pos++; break;
      case ':':  tokens[out++] = TOK_COLON;      pos++; break;
      case '#':  tokens[out++] = TOK_HASH;       pos++; break;
      case '=':  tokens[out++] = TOK_EQUAL;      pos++; break;
      // TI encodes compound comparisons as two separate tokens, e.g.
      // <=  →  TOK_LESS + TOK_EQUAL
      // <>  →  TOK_LESS + TOK_GREATER
      // >=  →  TOK_GREATER + TOK_EQUAL
      case '<':
        tokens[out++] = TOK_LESS;
        if (src[pos + 1] == '=')      { tokens[out++] = TOK_EQUAL;   pos += 2; }
        else if (src[pos + 1] == '>') { tokens[out++] = TOK_GREATER; pos += 2; }
        else                          {                              pos++;    }
        break;
      case '>':
        tokens[out++] = TOK_GREATER;
        if (src[pos + 1] == '=')      { tokens[out++] = TOK_EQUAL;   pos += 2; }
        else                          {                              pos++;    }
        break;
      default:
        foundOp = false;
        break;
    }
    if (foundOp)
    {
      expectLineNum = opKeepsLineRef ? expectLineNum : false;
      continue;
    }

    // Keyword match
    int kwIdx = matchKeyword(src, pos);
    if (kwIdx >= 0)
    {
      if (out >= maxLen)
      {
        return -1;
      }
      uint8_t emitted = keywords[kwIdx].token;
      tokens[out++] = emitted;
      pos += strlen(keywords[kwIdx].text);
      expectLineNum = isLineRefToken(emitted);

      // OLD / SAVE / MERGE / DELETE treat the rest of the statement as
      // a file-spec string, so `OLD DSK1.NAME` works without quotes
      // (TI behavior). If the user did quote it, skip — the normal
      // quoted-string branch above will have fired next iteration.
      if (isFilenameKwToken(emitted))
      {
        int sp = pos;
        while (src[sp] == ' ') sp++;
        if (src[sp] != '"' && src[sp] != '\0' &&
            src[sp] != ':' && src[sp] != '!')
        {
          int start = sp;
          while (src[sp] != '\0' && src[sp] != ':' && src[sp] != '!')
          {
            sp++;
          }
          // Trim trailing spaces
          int end = sp;
          while (end > start && src[end - 1] == ' ') end--;
          int slen = end - start;
          if (slen > 0 && out + 2 + slen < maxLen)
          {
            tokens[out++] = TOK_QUOTED_STR;
            tokens[out++] = (uint8_t)slen;
            memcpy(&tokens[out], &src[start], slen);
            out += slen;
          }
          pos = sp;
        }
      }
      continue;
    }

    // Variable name
    if (isalpha(src[pos]))
    {
      // Variable name — stored as raw ASCII bytes (TI format)
      int start = pos;
      while (isalnum(src[pos]) || src[pos] == '_')
      {
        pos++;
      }
      if (src[pos] == '$')
      {
        pos++;
      }
      int vlen = pos - start;
      if (out + vlen >= maxLen)
      {
        return -1;
      }
      for (int i = 0; i < vlen; i++)
      {
        tokens[out++] = toupper(src[start + i]);
      }
      expectLineNum = false;
      continue;
    }

    pos++;
  }

  if (out >= maxLen)
  {
    return -1;
  }
  tokens[out++] = TOK_EOL;
  return out;
}

// ---------------------------------------------------------------------------
// Detokenizer (converts tokens back to text for LIST/SAVE)
// ---------------------------------------------------------------------------

static void appendStr(char* buf, int& out, int bufSize, const char* str)
{
  int slen = strlen(str);
  int copyLen = (out + slen < bufSize - 1) ? slen : bufSize - 1 - out;
  memcpy(&buf[out], str, copyLen);
  out += copyLen;
}

static int detokenizeLine(const uint8_t* tokens, int length, char* buf,
                          int bufSize)
{
  int pos = 0;
  int out = 0;

  while (pos < length && tokens[pos] != TOK_EOL)
  {
    uint8_t tok = tokens[pos++];

    // String literal
    if (tok == TOK_QUOTED_STR)
    {
      uint8_t slen = tokens[pos++];
      if (out + slen + 2 >= bufSize) break;
      buf[out++] = '"';
      memcpy(&buf[out], &tokens[pos], slen);
      out += slen;
      pos += slen;
      buf[out++] = '"';
      continue;
    }

    // Number / unquoted string
    if (tok == TOK_UNQUOTED_STR)
    {
      uint8_t slen = tokens[pos++];
      int copyLen = (out + slen < bufSize - 1) ? slen : bufSize - 1 - out;
      memcpy(&buf[out], &tokens[pos], copyLen);
      out += copyLen;
      pos += slen;
      continue;
    }

    // TI-style line-number reference (after GOTO/GOSUB/THEN/etc.)
    if (tok == TOK_LINENUM)
    {
      if (pos + 1 >= length) break;
      uint16_t ln = ((uint16_t)tokens[pos] << 8) | tokens[pos + 1];
      pos += 2;
      char num[8];
      int n = snprintf(num, sizeof(num), "%u", (unsigned)ln);
      if (out + n < bufSize - 1)
      {
        memcpy(&buf[out], num, n);
        out += n;
      }
      continue;
    }

    // Tail comment: emit "! <text>" from TOK_BANG + TOK_STRING_LIT
    if (tok == TOK_BANG)
    {
      appendStr(buf, out, bufSize, "!");
      if (pos < length && tokens[pos] == TOK_QUOTED_STR)
      {
        pos++;
        uint8_t slen = tokens[pos++];
        int copyLen = (out + slen < bufSize - 1) ? slen : bufSize - 1 - out;
        memcpy(&buf[out], &tokens[pos], copyLen);
        out += copyLen;
        pos += slen;
      }
      continue;
    }

    // Everything else: direct O(1) token → text lookup.
    // Multi-char alphabetic names (keywords / functions) get surrounding
    // spaces; single-character entries (identifier bytes, operators,
    // punctuation) emit as-is so identifiers like "ABC" concatenate.
    const char* name = tokenNames[tok];
    if (name == NULL)
    {
      continue;
    }
    int klen = strlen(name);
    bool isMultiCharAlpha = (klen > 1 && name[0] >= 'A' && name[0] <= 'Z');
    if (out + klen + 2 >= bufSize) continue;
    if (isMultiCharAlpha && out > 0 && buf[out - 1] != ' ') buf[out++] = ' ';
    memcpy(&buf[out], name, klen);
    out += klen;
    if (isMultiCharAlpha) buf[out++] = ' ';
  }

  buf[out] = '\0';
  return out;
}

// ---------------------------------------------------------------------------
// Display driver
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Forked RGB panel driver — DOUBLE-BUFFERED.
// rgb_db.h/.cpp contain a minimal fork of Arduino_GFX's RGB panel that
// enables the ESP_LCD double_fb mode. CPU writes land in the back
// buffer; commitFrame() (called via tft->flush()) swaps front/back at
// vsync for tear-free output. See rgb_db.h for full rationale.
// ---------------------------------------------------------------------------
static Arduino_ESP32RGBPanelDB *rgbBus = new Arduino_ESP32RGBPanelDB(
    40 /* DE */, 41 /* VSYNC */, 39 /* HSYNC */, 42 /* PCLK */,
    45 /* R0 */, 48 /* R1 */, 47 /* R2 */, 21 /* R3 */, 14 /* R4 */,
    5  /* G0 */, 6  /* G1 */, 7  /* G2 */, 15 /* G3 */, 16 /* G4 */, 4 /* G5 */,
    8  /* B0 */, 3  /* B1 */, 46 /* B2 */, 9  /* B3 */, 1  /* B4 */,
    0, 8, 4, 8,      // hsync: polarity, front porch, pulse width, back porch
    0, 8, 4, 8,      // vsync: polarity, front porch, pulse width, back porch
    1, 14000000,     // pclk_active_neg, prefer_speed (14 MHz)
    false,           // useBigEndian
    0, 0,            // de_idle_high, pclk_idle_high
    20 * 800         // bounce_buffer_size_px (20 lines × 800 px = 32 KB SRAM)
);

static RGBDisplayDB *tft = new RGBDisplayDB(800, 480, rgbBus);

static int cursorCol = 0;
static int cursorRow = 0;

// TI Extended BASIC colors (black text on cyan background)
static uint16_t fgColor = 0x0000;  // black
static uint16_t bgColor = 0x07FF;  // cyan

// Display framebuffer
static char screenBuf[ROWS][COLS];
static char prevScreenBuf[ROWS][COLS];

// TI color palette (indices 1-16) → RGB565
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

// Per-character color palette indices (1-16; 1=transparent=use screen color)
static uint8_t charFgIdx[256];
static uint8_t charBgIdx[256];
static uint8_t screenColorIdx = 8;   // cyan by default

// Paint an 8-px TI-style screen-color frame hugging the 32x24 grid.
// Top / bottom span the full border ring width; left / right strips
// cover the grid height plus the top and bottom strips (so the ring
// joins at the corners without gaps).
static void paintBorder()
{
  int frameX = DISPLAY_X_OFFSET - BORDER_W;
  int frameY = DISPLAY_Y_OFFSET - BORDER_W;
  int frameW = COLS * CHAR_W + 2 * BORDER_W;
  int frameH = ROWS * CHAR_H + 2 * BORDER_W;
  // Top
  tft->fillRect(frameX, frameY, frameW, BORDER_W, bgColor);
  // Bottom
  tft->fillRect(frameX, GRID_BOTTOM_Y, frameW, BORDER_W, bgColor);
  // Left
  tft->fillRect(frameX, DISPLAY_Y_OFFSET, BORDER_W, ROWS * CHAR_H,
                bgColor);
  // Right
  int rightX = DISPLAY_X_OFFSET + COLS * CHAR_W;
  tft->fillRect(rightX, DISPLAY_Y_OFFSET, BORDER_W, ROWS * CHAR_H,
                bgColor);
  (void)frameH;   // reserved for future use
}

static void initDisplay()
{
  // Keep backlight off until display is ready to prevent showing garbage
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);

  tft->begin();
  // 180° rotation puts the SD-card slot / connectors at the top of
  // the visible image. RGB panels usually only honor rotation 0
  // natively; if 2 doesn't take, drawing will look unrotated and we
  // can fall back to app-level coordinate flipping.
  tft->setRotation(2);
  tft->fillScreen(0x0000);   // black surround until fillBackground runs
  tft->flush();              // commit so panel actually shows it

  digitalWrite(TFT_BL, HIGH);
}

// Resolve a palette index to RGB565, with transparency (1) falling through
// to the screen color.
static uint16_t resolveColor(uint8_t idx)
{
  if (idx < 1 || idx > 16) return 0;
  if (idx == 1)
  {
    return tiPalette[screenColorIdx];
  }
  return tiPalette[idx];
}

// Draw one 8x8 TI character scaled 2x into the 16x16 screen cell.
// Each source pixel becomes a 2x2 block in the output buffer.
static void drawCell(int col, int row)
{
  uint8_t ch = (uint8_t)screenBuf[row][col];
  int px = col * CHAR_W + DISPLAY_X_OFFSET;
  int py = row * CHAR_H + DISPLAY_Y_OFFSET;

  uint16_t fg = resolveColor(charFgIdx[ch]);
  uint16_t bg = resolveColor(charBgIdx[ch]);

  uint16_t pixBuf[CHAR_W * CHAR_H];   // 16 * 16 = 256
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

// --- Software sprite layer ---
//
// The TI VDP rendered sprites at pixel resolution; we emulate that in
// software on top of the character grid. spriteCellBounds() returns
// the 32x24 cells that the sprite overlaps so we can restore those
// cells (via drawCell) when a sprite moves or is deleted.
static void spriteCellBounds(const sprites::Sprite& s,
                             int& r0, int& c0, int& r1, int& c1)
{
  int body = sprites::bodySize(s.magnify);
  int scale = (s.magnify == sprites::MAG_2 ||
               s.magnify == sprites::MAG_4) ? 2 : 1;
  int pxH = body * scale;   // sprite size in TI pixels
  int pxW = body * scale;
  // TI pixel coordinates are 1-based (row=1 top). Each TI pixel is a
  // 2x2 block on our 800x480 panel. Each character cell is 16 physical
  // pixels = 8 TI pixels wide and tall.
  int topTi  = s.row - 1;
  int leftTi = s.col - 1;
  r0 = topTi / 8;
  c0 = leftTi / 8;
  r1 = (topTi  + pxH - 1) / 8;
  c1 = (leftTi + pxW - 1) / 8;
  if (r0 < 0) r0 = 0;
  if (c0 < 0) c0 = 0;
  if (r1 > ROWS - 1) r1 = ROWS - 1;
  if (c1 > COLS - 1) c1 = COLS - 1;
}

// --- Save-under buffer for sprite erase ---
//
// Re-rendering N character cells per erase is the visible-flicker
// bottleneck. Instead, when a sprite is drawn we render the cells it
// covers into a per-sprite PSRAM buffer ("save-under"), then on erase
// we blit that buffer back in a single draw16bitRGBBitmap call —
// orders of magnitude faster than walking 1-9 cells through the panel
// pixel by pixel.
//
// Footprint upper bound is mag-4 = 64x64 display pixels, which can
// straddle up to 5x5 = 80x80 cell-aligned region. 80*80*2 = 12.8 KB
// per sprite × 28 sprites = ~360 KB in PSRAM (lazy-allocated when a
// slot is first used).
static const int SPRITE_SAVE_MAX_DIM = 80;
static const int SPRITE_SAVE_MAX_PX  = SPRITE_SAVE_MAX_DIM * SPRITE_SAVE_MAX_DIM;
struct SpriteSave
{
  uint16_t* pixels = nullptr;
  int  x = 0, y = 0, w = 0, h = 0;
  bool valid = false;
};
static SpriteSave g_spriteSave[sprites::MAX_SPRITES + 1];

// Shared scratch used to composite (chars + sprite pixels) before
// blitting in a single draw16bitRGBBitmap call. Per-pixel writes to
// the panel are the dominant source of tearing during scanout.
static uint16_t* g_spriteCompBuf = nullptr;
static void ensureSpriteCompBuf()
{
  if (g_spriteCompBuf) return;
  g_spriteCompBuf = (uint16_t*)heap_caps_malloc(
      SPRITE_SAVE_MAX_PX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
}

static int spriteIndexOf(const sprites::Sprite& s)
{
  return (int)(&s - &sprites::g_sprites[0]);
}

static void ensureSpriteSaveBuf(int slot)
{
  if (slot < 0 || slot > sprites::MAX_SPRITES) return;
  if (g_spriteSave[slot].pixels) return;
  g_spriteSave[slot].pixels = (uint16_t*)heap_caps_malloc(
      SPRITE_SAVE_MAX_PX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
}

// Render the chars in cell rect (c0..c1, r0..r1) into dst as a tightly
// packed `w`×`h` RGB565 bitmap. Same per-pixel logic as drawCell but
// targets memory instead of the panel.
static void renderCellsToBuffer(int r0, int c0, int r1, int c1,
                                uint16_t* dst, int w)
{
  for (int rOff = 0; rOff <= r1 - r0; rOff++)
  {
    for (int cOff = 0; cOff <= c1 - c0; cOff++)
    {
      int cellR = r0 + rOff;
      int cellC = c0 + cOff;
      if (cellR < 0 || cellR >= ROWS || cellC < 0 || cellC >= COLS) continue;
      uint8_t ch = (uint8_t)screenBuf[cellR][cellC];
      uint16_t fg = resolveColor(charFgIdx[ch]);
      uint16_t bg = resolveColor(charBgIdx[ch]);
      int dstX0 = cOff * CHAR_W;
      int dstY0 = rOff * CHAR_H;
      for (int py = 0; py < 8; py++)
      {
        uint8_t bits = charPatterns[ch][py];
        uint16_t* row0 = &dst[(dstY0 + py * 2)     * w + dstX0];
        uint16_t* row1 = &dst[(dstY0 + py * 2 + 1) * w + dstX0];
        for (int px = 0; px < 8; px++)
        {
          uint16_t c = (bits & 0x80) ? fg : bg;
          row0[px * 2]     = c;
          row0[px * 2 + 1] = c;
          row1[px * 2]     = c;
          row1[px * 2 + 1] = c;
          bits <<= 1;
        }
      }
    }
  }
}

// Repaint the character cells under the sprite so the char grid is
// visible where the sprite used to be. Fast path: blit the save-under
// buffer captured by spriteDraw. Fallback: per-cell drawCell (used
// when no save buf is allocated yet, e.g. initial draw after CALL
// SPRITE).
static void spriteErase(const sprites::Sprite& s)
{
  if (!s.active) return;
  int slot = spriteIndexOf(s);
  SpriteSave& sv = g_spriteSave[slot];
  if (sv.valid && sv.pixels)
  {
    tft->draw16bitRGBBitmap(sv.x, sv.y, sv.pixels, sv.w, sv.h);
    sv.valid = false;
    return;
  }
  int r0, c0, r1, c1;
  spriteCellBounds(s, r0, c0, r1, c1);
  for (int r = r0; r <= r1; r++)
  {
    for (int c = c0; c <= c1; c++)
    {
      drawCell(c, r);
    }
  }
}

// Draw one sprite at its current position. Transparent pixels (pattern
// bit 0) leave whatever was on screen beneath.
static void spriteDraw(const sprites::Sprite& s)
{
  if (!s.active) return;

  int slot = spriteIndexOf(s);
  int r0, c0, r1, c1;
  spriteCellBounds(s, r0, c0, r1, c1);
  int saveW = (c1 - c0 + 1) * CHAR_W;
  int saveH = (r1 - r0 + 1) * CHAR_H;
  int saveX = c0 * CHAR_W + DISPLAY_X_OFFSET;
  int saveY = r0 * CHAR_H + DISPLAY_Y_OFFSET;

  // Hard fallback: if footprint is bigger than our scratch buffer
  // (shouldn't happen — even mag-4 fits in 80×80 cell-aligned), or
  // we're out of PSRAM, fall back to per-pixel paint.
  if (saveW <= 0 || saveH <= 0 || saveW * saveH > SPRITE_SAVE_MAX_PX)
  {
    int body  = sprites::bodySize(s.magnify);
    int scale = (s.magnify == sprites::MAG_2 ||
                 s.magnify == sprites::MAG_4) ? 2 : 1;
    uint16_t fg = resolveColor(s.colorIdx);
    int baseY = DISPLAY_Y_OFFSET + (s.row - 1) * 2;
    int baseX = DISPLAY_X_OFFSET + (s.col - 1) * 2;
    for (int sr = 0; sr < body; sr++)
    {
      for (int sc = 0; sc < body; sc++)
      {
        if (!sprites::pixelOn(s.charCode, s.magnify, sr, sc, charPatterns))
        {
          continue;
        }
        int reps = scale * 2;
        for (int dy = 0; dy < reps; dy++)
        {
          for (int dx = 0; dx < reps; dx++)
          {
            int py = baseY + sr * reps + dy;
            int px = baseX + sc * reps + dx;
            if (py >= DISPLAY_Y_OFFSET &&
                py < DISPLAY_Y_OFFSET + ROWS * CHAR_H &&
                px >= DISPLAY_X_OFFSET &&
                px < DISPLAY_X_OFFSET + COLS * CHAR_W)
            {
              tft->writePixel(px, py, fg);
            }
          }
        }
      }
    }
    return;
  }

  // Fast path: composite chars + sprite pixels into one buffer, then
  // blit in a single draw16bitRGBBitmap call. This is the key tearing
  // fix — instead of hundreds of writePixel calls landing during the
  // panel's bounce-buffer scan, we issue one tight contiguous DMA-
  // friendly write per sprite update.
  ensureSpriteSaveBuf(slot);
  ensureSpriteCompBuf();
  SpriteSave& sv = g_spriteSave[slot];
  if (!sv.pixels || !g_spriteCompBuf)
  {
    return;   // PSRAM alloc failed — silent skip is fine for now
  }

  // 1. Render the chars under the sprite into the per-sprite save
  //    buffer (used by next spriteErase).
  renderCellsToBuffer(r0, c0, r1, c1, sv.pixels, saveW);
  sv.x = saveX; sv.y = saveY; sv.w = saveW; sv.h = saveH;
  sv.valid = true;

  // 2. Copy save buf → composition scratch and overlay sprite pixels.
  //    Sprite is the only "ON pixels stamp" — transparent pixels keep
  //    whatever's underneath (just chars in this single-sprite frame).
  memcpy(g_spriteCompBuf, sv.pixels, (size_t)saveW * saveH * sizeof(uint16_t));

  int body  = sprites::bodySize(s.magnify);
  int scale = (s.magnify == sprites::MAG_2 ||
               s.magnify == sprites::MAG_4) ? 2 : 1;
  uint16_t fg = resolveColor(s.colorIdx);
  int baseY = DISPLAY_Y_OFFSET + (s.row - 1) * 2;
  int baseX = DISPLAY_X_OFFSET + (s.col - 1) * 2;
  int reps = scale * 2;
  int yMax = DISPLAY_Y_OFFSET + ROWS * CHAR_H;
  int xMax = DISPLAY_X_OFFSET + COLS * CHAR_W;
  for (int sr = 0; sr < body; sr++)
  {
    for (int sc = 0; sc < body; sc++)
    {
      if (!sprites::pixelOn(s.charCode, s.magnify, sr, sc, charPatterns))
      {
        continue;
      }
      for (int dy = 0; dy < reps; dy++)
      {
        int py = baseY + sr * reps + dy;
        if (py < DISPLAY_Y_OFFSET || py >= yMax) continue;
        int bufY = py - saveY;
        if (bufY < 0 || bufY >= saveH) continue;
        uint16_t* row = &g_spriteCompBuf[bufY * saveW];
        for (int dx = 0; dx < reps; dx++)
        {
          int px = baseX + sc * reps + dx;
          if (px < DISPLAY_X_OFFSET || px >= xMax) continue;
          int bufX = px - saveX;
          if (bufX < 0 || bufX >= saveW) continue;
          row[bufX] = fg;
        }
      }
    }
  }

  // 3. Blit the composed frame in one shot.
  tft->draw16bitRGBBitmap(saveX, saveY, g_spriteCompBuf, saveW, saveH);
}

// Redraw every active sprite. Order matters: TI Extended BASIC
// gives sprite #1 the highest priority (it sits on top of #2, which
// sits on top of #3, ...). Drawing in *reverse* index order means
// the highest-numbered sprite paints first and the lowest-numbered
// paints last, so #1 ends up on top.
static void spriteRedrawAll()
{
  for (int i = sprites::MAX_SPRITES; i >= 1; i--)
  {
    spriteDraw(sprites::g_sprites[i]);
  }
}

// 60 Hz integration of sprite velocity. Each velocity unit is 1/8 of a
// TI pixel per frame, so a 16 ms tick advances by vel/8. Sprites that
// actually crossed a pixel boundary get erased (char grid restored)
// and redrawn at their new position. Wraps at TI screen edges
// (row in [1,192], col in [1,256]) — TI behavior.
static void spriteTick()
{
  static unsigned long lastTick = 0;
  unsigned long now = millis();
  // First call after boot: prime lastTick so we don't immediately
  // integrate the entire millis() since power-on as a "catch-up".
  if (lastTick == 0) { lastTick = now; return; }
  if (now - lastTick < 16) return;     // ~60 Hz gate
  unsigned long elapsed = now - lastTick;
  lastTick = now;
  int frames = (int)(elapsed / 16);
  if (frames < 1) frames = 1;
  if (frames > 8) frames = 8;          // catch-up cap to avoid teleport

  // Per-sprite atomic update. After each sprite's erase+draw, redraw
  // any higher-priority sprites (lower indices) that are active so
  // they stay on top of the one that just moved. This keeps the
  // "missing pixel" window short (one sprite's worth of erase before
  // its own redraw) instead of erasing every moving sprite first.
  bool anyMoved = false;
  for (int i = 1; i <= sprites::MAX_SPRITES; i++)
  {
    sprites::Sprite& s = sprites::g_sprites[i];
    if (!s.active) continue;
    if (s.rowVel == 0 && s.colVel == 0) continue;

    int16_t prevRow = s.row, prevCol = s.col;
    s.subRow += (int32_t)s.rowVel * frames;
    s.subCol += (int32_t)s.colVel * frames;

    int16_t dr = (int16_t)(s.subRow / 8);
    int16_t dc = (int16_t)(s.subCol / 8);
    s.subRow -= (int32_t)dr * 8;
    s.subCol -= (int32_t)dc * 8;

    if (dr == 0 && dc == 0) continue;

    int16_t nr = s.row + dr;
    int16_t nc = s.col + dc;
    // TI VDP sprite registers are 8-bit and naturally wrap at 256.
    // pico9918 confirms this: real hardware has no clipping or
    // clamping, just register overflow. We mirror that. BASIC
    // programs that want sprites to bounce off screen edges are
    // responsible for the bounds check themselves — same contract
    // as on real TI.
    while (nr < 1)   nr += 256;
    while (nr > 256) nr -= 256;
    while (nc < 1)   nc += 256;
    while (nc > 256) nc -= 256;

    if (nr != prevRow || nc != prevCol)
    {
      spriteErase(s);
      s.row = nr;
      s.col = nc;
      spriteDraw(s);
      // Restore higher-priority sprites that may overlap #i's new
      // footprint. Sprite #1 is highest priority and must paint last.
      for (int p = i - 1; p >= 1; p--)
      {
        if (sprites::g_sprites[p].active) spriteDraw(sprites::g_sprites[p]);
      }
      anyMoved = true;
    }
  }

  // Snapshot every active sprite's position so BASIC sees a coherent
  // frame. POSITION / COINC / DISTANCE read snapRow/snapCol; physics
  // updates row/col live each tick. This is the "virtual vsync":
  // queries within one BASIC iteration always reflect the same
  // moment in time, even if a tick fires mid-iteration.
  for (int i = 1; i <= sprites::MAX_SPRITES; i++)
  {
    sprites::Sprite& s = sprites::g_sprites[i];
    if (!s.active) continue;
    s.snapRow     = s.row;
    s.snapCol     = s.col;
    s.snapMagnify = s.magnify;
  }

  // Commit the back buffer to the panel — swap takes effect at next
  // vsync, so the user sees the complete frame all at once instead
  // of a half-updated one. Skip if nothing moved (commit costs a
  // ~10 ms full-screen memcpy to refill the new back buffer).
  if (anyMoved) tft->flush();
}

static void scrollUp()
{
  memcpy(&screenBuf[0][0], &screenBuf[1][0], COLS * (ROWS - 1));
  memset(&screenBuf[ROWS - 1][0], 0x20, COLS);
  refreshScreen();
  spriteRedrawAll();
  int y = (ROWS - 1) * CHAR_H + DISPLAY_Y_OFFSET;
  tft->fillRect(DISPLAY_X_OFFSET, y, COLS * CHAR_W, CHAR_H, bgColor);
}

static void printChar(char c)
{
  // Mirror output to serial terminal for copy/paste
  Serial.write(c);
  if (c == '\n')
  {
    Serial.write('\r');
  }

  // TI behavior: cursor always on bottom row (ROWS-1 = 23).
  // '\n' scrolls up one row and resets column to 0.
  if (c == '\n')
  {
    scrollUp();
    cursorRow = ROWS - 1;
    cursorCol = 0;
    return;
  }

  // Column wrap — scroll up and start fresh at col 0
  if (cursorCol >= COLS)
  {
    scrollUp();
    cursorRow = ROWS - 1;
    cursorCol = 0;
  }

  screenBuf[cursorRow][cursorCol] = c;
  drawCell(cursorCol, cursorRow);
  prevScreenBuf[cursorRow][cursorCol] = c;
  cursorCol++;
}

static void printString(const char* str)
{
  while (*str)
  {
    printChar(*str++);
  }
  tft->flush();  
}

static void printLine(const char* str)
{
  printString(str);
  printChar('\n');
  tft->flush();
}

// TI-style error print: blank line, error message, blank line, plus a
// BEL (0x07) to the serial terminal so monitors that honor it beep.
static void printError(const char* str)
{
  printLine("");
  printLine(str);
  printLine("");
}

static void clearScreen()
{
  memset(screenBuf, ' ', COLS * ROWS);
  fillBackground(bgColor);
  // TI behavior: cursor on bottom row after CLEAR
  cursorCol = 0;
  cursorRow = ROWS - 1;
}

// Move cursor to bottom row for INPUT (TI behavior — INPUT always at row 24).
// With the new print model, we just need to ensure we're at col 0.
static void gfxPrepareInput()
{
  if (cursorCol > 0)
  {
    printChar('\n');
  }
}

// Reset graphics to editor defaults (called when program ends)
// Reset all character colors to default: fg=black(2), bg=transparent(1).
// Screen color defaults to cyan(8), so transparent bg shows cyan.
static void gfxResetColors()
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

static void gfxReset()
{
  gfxResetColors();
  initCharPatterns();
  sprites::clearAll();
  // Border repaints to default cyan along with the cells.
  paintBorder();
  redrawScreen();
}

// Graphics callbacks for CALL HCHAR, VCHAR, GCHAR, SCREEN, COLOR

static void gfxSetChar(int row, int col, char ch)
{
  if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return;
  screenBuf[row][col] = ch;
  drawCell(col, row);
  prevScreenBuf[row][col] = ch;
}

static char gfxGetChar(int row, int col)
{
  if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return 32;
  return screenBuf[row][col];
}

static void gfxSetScreenColor(int colorIdx)
{
  if (colorIdx < 1 || colorIdx > 16) return;
  screenColorIdx = colorIdx;
  bgColor = tiPalette[colorIdx];

  // Paint the TI-style border plus the grid area in one burst; then
  // redraw only the cells that hold non-blank / color-overriding content.
  paintBorder();
  tft->fillRect(DISPLAY_X_OFFSET, DISPLAY_Y_OFFSET,
                COLS * CHAR_W, ROWS * CHAR_H, bgColor);
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      uint8_t ch = (uint8_t)screenBuf[r][c];
      // Skip cells that are pure space with transparent bg — the
      // fillRect already painted them correctly.
      if (ch == ' ' && charBgIdx[ch] == 1) continue;
      drawCell(c, r);
      prevScreenBuf[r][c] = ch;
    }
  }
}

// CALL COLOR(set, fg, bg) — sets colors for a group of 8 characters.
// Extended BASIC character sets:
//   Set 1 = chars 32-39, Set 2 = 40-47, ... Set 16 = 152-159
// Move cursor to a specific position (for DISPLAY AT, ACCEPT AT).
// row, col are 0-based.
static void gfxMoveCursor(int row, int col)
{
  if (row < 0) row = 0;
  if (row >= ROWS) row = ROWS - 1;
  if (col < 0) col = 0;
  if (col >= COLS) col = COLS - 1;
  cursorRow = row;
  cursorCol = col;
}

// CALL KEY: read one key from Serial or BLE keyboard without blocking.
// Returns 0 if no key available, else the character code.
static int gfxReadKey()
{
  if (Serial.available())
  {
    return Serial.read() & 0xFF;
  }
  if (bleKbAvailable())
  {
    return bleKbRead() & 0xFF;
  }
  return 0;
}

// CALL CHAR: redefine a character's 8x8 bitmap pattern
static void gfxSetCharPattern(int charCode, const uint8_t* pattern)
{
  if (charCode < 0 || charCode > 255) return;
  memcpy(charPatterns[charCode], pattern, 8);
  // Redraw any cells using this character
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      if ((uint8_t)screenBuf[r][c] == (uint8_t)charCode)
      {
        drawCell(c, r);
      }
    }
  }
}

// CALL CHARPAT: read a character's current 8×8 pattern
static void gfxGetCharPattern(int charCode, uint8_t* out)
{
  if (charCode < 0 || charCode > 255)
  {
    memset(out, 0, 8);
    return;
  }
  memcpy(out, charPatterns[charCode], 8);
}

// CALL CHARSET: reset characters 32-127 to their ROM default patterns.
// Leaves user-defined graphics slots (128+) alone.
static void gfxResetCharset()
{
  for (int i = 32; i < 128; i++)
  {
    memcpy_P(charPatterns[i], tiFont[i], 8);
  }
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      uint8_t ch = (uint8_t)screenBuf[r][c];
      if (ch >= 32 && ch < 128) drawCell(c, r);
    }
  }
}

static void gfxSetCharColor(int charSet, int fg, int bg)
{
  if (charSet < 1 || charSet > 16) return;
  if (fg < 1 || fg > 16) return;
  if (bg < 1 || bg > 16) return;

  int firstChar = 32 + (charSet - 1) * 8;
  for (int i = 0; i < 8; i++)
  {
    int ch = firstChar + i;
    if (ch >= 0 && ch < 256)
    {
      charFgIdx[ch] = (uint8_t)fg;
      charBgIdx[ch] = (uint8_t)bg;
    }
  }

  // Redraw any cells using characters in this set
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      uint8_t ch = (uint8_t)screenBuf[r][c];
      if (ch >= firstChar && ch < firstChar + 8)
      {
        drawCell(c, r);
      }
    }
  }
}

// Fill the display: char area = bgColor, borders (outside 24 rows) = black.
static void fillBackground(uint16_t bg)
{
  // Paint the whole display black, then draw the 8-px screen-color
  // frame around the grid, then fill the grid and the status strip.
  tft->fillScreen(0x0000);
  uint16_t savedBg = bgColor;
  bgColor = bg;
  paintBorder();
  bgColor = savedBg;
  tft->fillRect(DISPLAY_X_OFFSET, DISPLAY_Y_OFFSET,
                COLS * CHAR_W, ROWS * CHAR_H, bg);
  // Status strip disabled while diagnosing post-rotation garbage —
  // see showStatus().
}

// TI-Texas logo — 3×3 character grid taken straight from the TI title
// screen. Each entry is the CALL CHAR pattern for one cell (row,col).
// Cells are laid out row-major: 129=(1,1) 130=(1,2) 131=(1,3)
//                                132=(2,1) 133=(2,2) 134=(2,3)
//                                135=(3,1) 136=(3,2) 137=(3,3)
static const uint8_t tiLogoChars[9][8] =
{
  {0x00, 0x01, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03},  // (1,1)
  {0x00, 0xFC, 0x04, 0x05, 0x05, 0x04, 0x06, 0x02},  // (1,2)
  {0x00, 0x00, 0x80, 0x40, 0x40, 0x80, 0x00, 0x0C},  // (1,3)
  {0x03, 0xFF, 0x80, 0xC0, 0x40, 0x60, 0x38, 0x1C},  // (2,1)
  {0x0C, 0x19, 0x21, 0x21, 0x3D, 0x05, 0x05, 0x05},  // (2,2)
  {0x12, 0xBA, 0x8A, 0x8A, 0xBA, 0xA1, 0xA1, 0xA1},  // (2,3)
  {0x0E, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00},  // (3,1)
  {0xC4, 0xE2, 0x31, 0x10, 0x18, 0x0C, 0x07, 0x03},  // (3,2)
  {0x22, 0x4C, 0x90, 0x20, 0x40, 0x40, 0x20, 0xE0},  // (3,3)
};

// Redefine char codes 129..137 with the logo patterns and place them in
// a 3×3 grid on screen with the top-left at (startRow, startCol).
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

// 8x8 copyright glyph (©) used in the splash-screen copyright line.
static const uint8_t copyrightBitmap[8] =
{
  0x3C,  // ..####..
  0x42,  // .#....#.
  0x99,  // #..##..#
  0xA1,  // #.#....#
  0xA1,  // #.#....#
  0x99,  // #..##..#
  0x42,  // .#....#.
  0x3C,  // ..####..
};

// TI-99/4A boot screen: colored stripes top and bottom, centered text.
// Pattern approximates the 1981 TI home computer startup screen.
static void showBootScreen()
{
  // Clear display to cyan in the char area, black outside
  fillBackground(tiPalette[8]);

  // Redefine char 128 as the © copyright symbol
  memcpy(charPatterns[128], copyrightBitmap, 8);

  // Stripe colors (approximating the TI pattern left to right)
  const uint8_t stripes[] = {
    9, 4, 2, 12, 13, 14,        // left group
    5, 3, 14, 9, 15, 6, 10, 12, 9   // right group
  };
  const int numStripes = sizeof(stripes);

  // 15 stripes + 1 gap = 16 slots across the full character area width.
  // Char area = COLS * CHAR_W; each slot = that / 16.
  const int stripeW = (COLS * CHAR_W) / 16;
  const int stripeH = 3 * CHAR_H;    // 3 rows
  const int gapEnd  = 7 * stripeW;   // gap = slot 6 (after the 6 left stripes)

  // Top band — TI rows 1-3
  int topY = DISPLAY_Y_OFFSET;
  for (int i = 0; i < numStripes; i++)
  {
    int x = DISPLAY_X_OFFSET +
            ((i < 6) ? i * stripeW : (i - 6) * stripeW + gapEnd);
    tft->fillRect(x, topY, stripeW, stripeH, tiPalette[stripes[i]]);
  }

  // Bottom band — TI rows 19-21 (0-indexed 18-20)
  int bottomY = DISPLAY_Y_OFFSET + 18 * CHAR_H;
  for (int i = 0; i < numStripes; i++)
  {
    int x = DISPLAY_X_OFFSET +
            ((i < 6) ? i * stripeW : (i - 6) * stripeW + gapEnd);
    tft->fillRect(x, bottomY, stripeW, stripeH, tiPalette[stripes[i]]);
  }

  // Draw centered text directly via the framebuffer.
  // Our display is 28 cols; "TEXAS INSTRUMENTS" (17) → col 5 start.
  auto drawText = [](const char* text, int row) {
    int len = strlen(text);
    int col = (COLS - len) / 2;
    if (col < 0) col = 0;
    for (int i = 0; i < len && col + i < COLS; i++)
    {
      screenBuf[row][col + i] = text[i];
      drawCell(col + i, row);
    }
  };

  // Texas logo — 3×3 char grid at TI rows 6-8 (0-indexed 5-7)
  drawTexasLogo(5, (COLS - 3) / 2);

  drawText("TEXAS INSTRUMENTS",             9);   // TI row 10
  drawText("HOME COMPUTER",                11);   // TI row 12
  drawText("READY-PRESS ANY KEY TO BEGIN", 16);   // TI row 17
  drawText("\x80" "1981    TEXAS INSTRUMENTS", 22);   // TI row 23 with ©

  Serial.println("PRESS ANY KEY TO CONTINUE");

  // Commit the title screen so the user actually sees it before the
  // wait loop. With double-buffering the screen stays black until
  // flush() is called.
  tft->flush();

  // Wait for any key — from Serial or BLE keyboard. Keep BLE scanning
  // alive so reconnect can complete while we're sitting here.
  while (!Serial.available() && !bleKbAvailable())
  {
    bleKbTask();
    yield();
    delay(10);
  }
  while (Serial.available()) { Serial.read(); delay(2); }
  while (bleKbAvailable())   { bleKbRead();  delay(2); }

  // Clear and show the menu screen
  fillBackground(tiPalette[8]);
  for (int r = 0; r < ROWS; r++)
  {
    memset(screenBuf[r], ' ', COLS);
    memset(prevScreenBuf[r], 0, COLS);
  }

  auto drawText2 = [](const char* text, int row, int col) {
    int len = strlen(text);
    for (int i = 0; i < len && col + i < COLS; i++)
    {
      screenBuf[row][col + i] = text[i];
      drawCell(col + i, row);
    }
  };

  drawText2("TEXAS INSTRUMENTS",     0, 5);
  drawText2("HOME COMPUTER",         1, 7);
  drawText2("PRESS",                 3, 2);
  drawText2("1 FOR TI BASIC",        5, 2);
  drawText2("2 FOR TI EXTENDED BASIC", 7, 2);

  Serial.println("PRESS 1 OR 2 TO CONTINUE");

  // Commit menu screen before the wait loop.
  tft->flush();

  while (!Serial.available() && !bleKbAvailable())
  {
    bleKbTask();
    yield();
    delay(10);
  }
  while (Serial.available()) { Serial.read(); delay(2); }
  while (bleKbAvailable())   { bleKbRead();  delay(2); }

  clearScreen();
}

// Render a string into the status bar using TI charPatterns at native
// 8x8 (one display pixel per source pixel — small text). Replaces
// Arduino_GFX's setTextColor/setCursor/print (not in our minimal
// RGBDisplayDB).
static void drawStatusText(int x, int y, const char* s,
                           uint16_t fg, uint16_t bg)
{
  int cursorX = x;
  while (*s)
  {
    uint8_t ch = (uint8_t)*s++;
    uint16_t pixBuf[8 * 8];
    for (int py = 0; py < 8; py++)
    {
      uint8_t bits = charPatterns[ch][py];
      for (int px = 0; px < 8; px++)
      {
        pixBuf[py * 8 + px] = (bits & 0x80) ? fg : bg;
        bits <<= 1;
      }
    }
    tft->draw16bitRGBBitmap(cursorX, y, pixBuf, 8, 8);
    cursorX += 8;
    if (cursorX > SCREEN_W) break;
  }
}

static void showStatus(const char* msg)
{
  // Temporarily disabled while diagnosing the post-rotation status-bar
  // garbage. The bar lives at logical y=STATUS_Y (=456), which after
  // rotation 2 lands in the FB rows panel scans first — possibly
  // colliding with the back-buffer pre-fill memcpy. Removing it lets
  // us isolate whether the disruption was status-bar specific.
  (void)msg;
}

// ---------------------------------------------------------------------------
// Execution Manager instance (declared early so the line editor can look up
// program lines for line-number + UP-arrow recall)
// ---------------------------------------------------------------------------
static ExecManager em;

// ---------------------------------------------------------------------------
// Line editor (shared by getInputLine and checkInput)
//
// Supports TI-style keys: DEL (7), INS toggle (4), ERASE (2), CLEAR (12 =
// break), REDO (14), arrows (8/9/10/11), line-number + UP for recall.
// Single-row editing only — long lines aren't wrapped visually but are
// kept in the buffer and submitted intact on Enter.
// ---------------------------------------------------------------------------

static char inputBuf[MAX_INPUT_LEN + 1];
static int  inputPos = 0;          // len of input so far (for main loop)
static bool inputReady = false;

static char lastCommandLine[MAX_INPUT_LEN + 1] = {0};
static bool editInsertMode = false;

// NUMBER mode: when active, editorBeginLine pre-fills the prompt with the
// next auto-incrementing line number. Set by cmdNumber(); cleared when the
// user presses Enter on an empty line.
static int  numModeStart  = 0;
static int  numModeIncr   = 0;
static int  numModeNext   = 0;
static bool numModeActive = false;

// Current editor mode (see line_editor.h). Starts in ENTRY on every new
// line, flips to EDIT on successful recall (REDO or <N>+UP/<N>+DOWN).
static EditMode editMode = EM_ENTRY;

// The program line currently under edit — used by EDIT-mode UP/DOWN
// to move to the previous/next program line. -1 when not in EDIT mode.
static int lastRecalledLineNum = -1;

// Forward decls (needed by line-number recall and UP/DOWN commit)
static int detokenizeLine(const uint8_t* tokens, int length, char* buf,
                          int bufSize);
static int tokenizeLine(const char* src, uint8_t* tokens, int maxLen);

// Draw a solid block cursor at the current position (inverted colors)
static void drawCursor(bool visible)
{
  int px = cursorCol * CHAR_W + DISPLAY_X_OFFSET;
  int py = cursorRow * CHAR_H + DISPLAY_Y_OFFSET;
  if (visible)
  {
    tft->fillRect(px, py, CHAR_W, CHAR_H, tiPalette[2]);
  }
  else
  {
    drawCell(cursorCol, cursorRow);
  }
}

// Sync global cursorCol/cursorRow with the edit state's logical position
static void editSyncCursor(const LineEdit& s)
{
  int col = s.startCol + s.pos;
  if (col >= COLS) col = COLS - 1;
  cursorCol = col;
  cursorRow = s.startRow;
}

// Redraw buffer content starting at `fromPos`, plus `eraseExtra` trailing
// cells (used after a shrink). Single-row only.
static void redrawLineTail(const LineEdit& s, int fromPos, int eraseExtra)
{
  for (int i = fromPos; i < s.len; i++)
  {
    int col = s.startCol + i;
    if (col >= COLS) break;
    screenBuf[s.startRow][col] = s.buf[i];
    drawCell(col, s.startRow);
  }
  for (int i = 0; i < eraseExtra; i++)
  {
    int col = s.startCol + s.len + i;
    if (col >= COLS) break;
    screenBuf[s.startRow][col] = ' ';
    drawCell(col, s.startRow);
  }
}

// Double-buffered serial input for fast paste.
//
// Serial.available()/Serial.read() is called by the editor at the editor's
// own pace — about one character per display refresh. That's far slower
// than USB CDC can deliver bytes during a paste, so the Arduino-CDC ring
// buffer (~4 KB) overflows and lines get dropped.
//
// pasteDrainSerial() sucks every byte out of Serial into a much larger
// local ring buffer as often as we can call it (top of loop, inside the
// editor's blink/yield points, inside getInputLine, etc.). editorReadChar
// then reads from this buffer, decoupling the producer (USB) from the
// consumer (display-bound editor) entirely.
#define PASTE_BUF_SIZE 16384
static uint8_t pasteBuf[PASTE_BUF_SIZE];
static int pasteHead = 0;
static int pasteTail = 0;

static void pasteDrainSerial()
{
  while (Serial.available())
  {
    int next = (pasteHead + 1) % PASTE_BUF_SIZE;
    if (next == pasteTail) break;   // full — back-pressure onto Serial
    pasteBuf[pasteHead] = (uint8_t)Serial.read();
    pasteHead = next;
  }
}

static bool pasteAvailable()
{
  return pasteHead != pasteTail;
}

static int pasteRead()
{
  if (pasteHead == pasteTail) return -1;
  uint8_t c = pasteBuf[pasteTail];
  pasteTail = (pasteTail + 1) % PASTE_BUF_SIZE;
  return c;
}

// Reads the next editor byte from the paste buffer (Serial side) or BLE,
// normalizing Serial line endings (\r\n → one Enter; lone \n → \r) and
// dropping tabs (since \t = RIGHT-arrow in the TI encoding).
// Returns -1 if nothing is available.
static int editorReadChar()
{
  static bool skipNextLf = false;

  // Top up from Serial before each read so we don't block on the editor's
  // pace while USB has more bytes waiting.
  pasteDrainSerial();

  while (pasteAvailable())
  {
    uint8_t c = (uint8_t)pasteRead();
    if (c == '\r') { skipNextLf = true;  return '\r'; }
    if (c == '\n')
    {
      if (skipNextLf) { skipNextLf = false; continue; }
      return '\r';
    }
    skipNextLf = false;
    if (c == '\t') continue;
    return c;
  }

  if (bleKbAvailable())
  {
    return bleKbRead();
  }
  return -1;
}

// Find the program index of a line with this lineNum, or -1 if absent.
static int findProgramLineIndex(int lineNum)
{
  for (int i = 0; i < em.programSize(); i++)
  {
    if (em.getLine(i)->lineNum == lineNum) return i;
  }
  return -1;
}

// Replace buffer contents with `src`. Used by REDO and line-number recall.
static void editReplaceLine(LineEdit& s, const char* src)
{
  int oldLen = s.len;
  int n = 0;
  while (src[n] && n < s.maxLen)
  {
    s.buf[n] = src[n];
    n++;
  }
  s.buf[n] = '\0';
  s.len = n;
  s.pos = 0;
  redrawLineTail(s, 0, (oldLen > s.len) ? (oldLen - s.len) : 0);
  editSyncCursor(s);
}

// Re-tokenize the current edit buffer and store it back into the program.
// Called before UP/DOWN navigation so edits to the line are preserved as
// if Enter had been pressed. Returns false only on tokenize failure.
static bool commitEditedLine(const LineEdit& s)
{
  int p = 0;
  while (p < s.len && s.buf[p] == ' ') p++;
  if (p >= s.len) return true;
  if (!isdigit((unsigned char)s.buf[p])) return true;

  int lineNum = 0;
  while (p < s.len && isdigit((unsigned char)s.buf[p]))
  {
    lineNum = lineNum * 10 + (s.buf[p] - '0');
    p++;
  }
  while (p < s.len && s.buf[p] == ' ') p++;

  if (p >= s.len)
  {
    em.deleteLine((uint16_t)lineNum);
    return true;
  }

  uint8_t toks[MAX_LINE_TOKENS];
  int len = tokenizeLine(&s.buf[p], toks, MAX_LINE_TOKENS);
  if (len < 0) return false;
  em.storeLine((uint16_t)lineNum, toks, len);
  return true;
}

// Load the program line at `idx` into the edit buffer, flip to EDIT mode,
// and remember the line number so subsequent UP/DOWN browse prev/next.
static void loadProgramLineToEdit(LineEdit& s, int idx)
{
  ProgramLine* pl = em.getLine(idx);
  if (!pl) return;
  char tmp[MAX_INPUT_LEN + 1];
  int n = snprintf(tmp, sizeof(tmp), "%d ", pl->lineNum);
  detokenizeLine(pl->tokens, pl->length, &tmp[n], sizeof(tmp) - n);
  editReplaceLine(s, tmp);
  lastRecalledLineNum = pl->lineNum;
  editMode = EM_EDIT;
}

// Test whether the current buffer contains nothing but decimal digits.
static bool editBufferIsAllDigits(const LineEdit& s)
{
  if (s.len == 0) return false;
  for (int i = 0; i < s.len; i++)
  {
    if (!isdigit((unsigned char)s.buf[i])) return false;
  }
  return true;
}

// Remove the char at `s.pos` (if any) and redraw the tail.
static void editDeleteAtCursor(LineEdit& s)
{
  if (s.pos >= s.len) return;
  for (int i = s.pos; i < s.len - 1; i++) s.buf[i] = s.buf[i + 1];
  s.len--;
  s.buf[s.len] = '\0';
  redrawLineTail(s, s.pos, 1);
  editSyncCursor(s);
}

// Backspace: move cursor left then delete the char now under it.
static void editBackspace(LineEdit& s)
{
  if (s.pos == 0) return;
  s.pos--;
  editDeleteAtCursor(s);
}

// Insert or overwrite `c` at the cursor and advance.
static void editTypeChar(LineEdit& s, uint8_t c)
{
  if (s.len >= s.maxLen) return;

  if (editInsertMode && s.pos < s.len)
  {
    for (int i = s.len; i > s.pos; i--) s.buf[i] = s.buf[i - 1];
    s.buf[s.pos] = c;
    s.len++;
    s.buf[s.len] = '\0';
    redrawLineTail(s, s.pos, 0);
    s.pos++;
    editSyncCursor(s);
  }
  else
  {
    s.buf[s.pos] = c;
    if (s.pos == s.len)
    {
      s.len++;
      s.buf[s.len] = '\0';
    }
    int col = s.startCol + s.pos;
    if (col < COLS)
    {
      screenBuf[s.startRow][col] = c;
      drawCell(col, s.startRow);
    }
    Serial.write(c);       // mirror to serial for paste visibility
    s.pos++;
    editSyncCursor(s);
  }
}

// Wipe the current line and return to ENTRY mode.
static void editEraseLine(LineEdit& s)
{
  int oldLen = s.len;
  s.len = 0;
  s.pos = 0;
  s.buf[0] = '\0';
  redrawLineTail(s, 0, oldLen);
  editSyncCursor(s);
  editMode = EM_ENTRY;
  lastRecalledLineNum = -1;
}

static EditResult processEditChar(uint8_t c, LineEdit& s)
{
  // ----- handled identically in both modes -----

  // Enter: commit line. Only match '\r' — '\n' (10) is DOWN on TI.
  // Serial's '\n' is normalized to '\r' at the read site.
  if (c == '\r')
  {
    // NUMBER mode: if user pressed Enter without adding anything past the
    // auto-fill, exit NUMBER mode and throw away the buffer so we don't
    // accidentally delete the line with that number.
    if (numModeActive && s.historyEnabled && editorBufferIsAutoFillOnly(s))
    {
      numModeActive = false;
      s.len = 0;
      s.pos = 0;
      s.buf[0] = '\0';
    }
    s.buf[s.len] = '\0';
    if (s.historyEnabled)
    {
      strncpy(lastCommandLine, s.buf, sizeof(lastCommandLine) - 1);
      lastCommandLine[sizeof(lastCommandLine) - 1] = '\0';
    }
    cursorCol = s.startCol + s.len;
    if (cursorCol >= COLS) cursorCol = COLS - 1;
    cursorRow = s.startRow;
    printChar('\n');
    editMode = EM_ENTRY;
    lastRecalledLineNum = -1;
    return EDIT_SUBMITTED;
  }

  // CLEAR — break
  if (c == 12) return EDIT_BROKEN;

  // ERASE (FCTN+3) — wipe line in either mode, drop back to ENTRY
  if (c == 2)
  {
    editEraseLine(s);
    return EDIT_CONTINUE;
  }

  // INS (FCTN+2) — toggle insert mode (global flag, both edit modes)
  if (c == 4)
  {
    editInsertMode = !editInsertMode;
    return EDIT_CONTINUE;
  }

  // REDO (FCTN+8) — reload last-entered line, flip to EDIT
  if (c == 14)
  {
    if (s.historyEnabled && lastCommandLine[0] != '\0')
    {
      editReplaceLine(s, lastCommandLine);
      editMode = EM_EDIT;
    }
    return EDIT_CONTINUE;
  }

  // BKSP (127) — delete previous char. Works in both modes since cursor
  // naturally sits at end during ENTRY.
  if (c == 127)
  {
    editBackspace(s);
    return EDIT_CONTINUE;
  }

  // Printable — typing always feeds the buffer
  if (c >= 32 && c < 127)
  {
    editTypeChar(s, c);
    return EDIT_CONTINUE;
  }

  // ----- Cursor movement & DEL work in every edit context -----

  // LEFT (8, FCTN+S) — cursor left
  if (c == 8)
  {
    if (s.pos > 0) { s.pos--; editSyncCursor(s); }
    return EDIT_CONTINUE;
  }

  // RIGHT (9, FCTN+D) — cursor right
  if (c == 9)
  {
    if (s.pos < s.len) { s.pos++; editSyncCursor(s); }
    return EDIT_CONTINUE;
  }

  // DEL (7, FCTN+1) — delete char at cursor
  if (c == 7)
  {
    editDeleteAtCursor(s);
    return EDIT_CONTINUE;
  }

  // ----- UP/DOWN are mode-aware -----
  //
  //   INPUT (historyEnabled=false): no-op
  //   ENTRY (editor prompt, not yet recalled): if the buffer is all digits,
  //     jump to EDIT mode on that program line
  //   EDIT  (a line is currently under edit): commit the current buffer,
  //     then move to the previous/next program line; past the boundary
  //     exits EDIT mode

  if (c == 11)   // UP (FCTN+E)
  {
    if (!s.historyEnabled) return EDIT_CONTINUE;

    if (editMode == EM_ENTRY)
    {
      if (editBufferIsAllDigits(s))
      {
        int idx = findProgramLineIndex(atoi(s.buf));
        if (idx >= 0) loadProgramLineToEdit(s, idx);
      }
      return EDIT_CONTINUE;
    }

    // EM_EDIT — commit and navigate to previous line
    int oldLine = lastRecalledLineNum;
    if (!commitEditedLine(s))
    {
      printError("* SYNTAX ERROR");
      editEraseLine(s);
      return EDIT_CONTINUE;
    }
    int idx = findProgramLineIndex(oldLine);
    if (idx < 0) { editEraseLine(s); return EDIT_CONTINUE; }
    if (idx > 0)
    {
      printChar('\n');
      s.startCol = cursorCol;
      s.startRow = cursorRow;
      s.len = 0; s.pos = 0; s.buf[0] = '\0';
      loadProgramLineToEdit(s, idx - 1);
    }
    else
    {
      editEraseLine(s);
    }
    return EDIT_CONTINUE;
  }

  if (c == 10)   // DOWN (FCTN+X)
  {
    if (!s.historyEnabled) return EDIT_CONTINUE;

    if (editMode == EM_ENTRY)
    {
      if (editBufferIsAllDigits(s))
      {
        int idx = findProgramLineIndex(atoi(s.buf));
        if (idx >= 0) loadProgramLineToEdit(s, idx);
      }
      return EDIT_CONTINUE;
    }

    // EM_EDIT — commit and navigate to next line
    int oldLine = lastRecalledLineNum;
    if (!commitEditedLine(s))
    {
      printError("* SYNTAX ERROR");
      editEraseLine(s);
      return EDIT_CONTINUE;
    }
    int idx = findProgramLineIndex(oldLine);
    if (idx < 0) { editEraseLine(s); return EDIT_CONTINUE; }
    if (idx < em.programSize() - 1)
    {
      printChar('\n');
      s.startCol = cursorCol;
      s.startRow = cursorRow;
      s.len = 0; s.pos = 0; s.buf[0] = '\0';
      loadProgramLineToEdit(s, idx + 1);
    }
    else
    {
      editEraseLine(s);
    }
    return EDIT_CONTINUE;
  }

  return EDIT_CONTINUE;
}

static bool getInputLine(char* buf, int bufSize)
{
  LineEdit s = { buf, bufSize - 1, 0, 0, cursorCol, cursorRow, false };
  buf[0] = '\0';

  bool cursorVisible = false;
  unsigned long lastBlink = 0;
  const unsigned long BLINK_MS = 400;

  while (true)
  {
    unsigned long now = millis();
    if (now - lastBlink >= BLINK_MS)
    {
      cursorVisible = !cursorVisible;
      editSyncCursor(s);
      drawCursor(cursorVisible);
      lastBlink = now;
    }

    bleKbTask();

    int c;
    while ((c = editorReadChar()) >= 0)
    {
      if (cursorVisible)
      {
        editSyncCursor(s);
        drawCursor(false);
        cursorVisible = false;
      }

      EditResult r = processEditChar((uint8_t)c, s);
      if (r == EDIT_SUBMITTED) return true;
      if (r == EDIT_BROKEN)    return false;   // INPUT aborted
    }
    yield();
  }
}

// ---------------------------------------------------------------------------
// Editor prompt (non-blocking, called from loop)
// ---------------------------------------------------------------------------
static bool editorCursorVisible = false;
static unsigned long editorLastBlink = 0;
static const unsigned long EDITOR_BLINK_MS = 400;

static LineEdit editorState = { inputBuf, MAX_INPUT_LEN, 0, 0, 0, 0, true };
static bool editorLineActive = false;   // true between start-of-line and Enter

static void editorBeginLine()
{
  editorState.buf = inputBuf;
  editorState.maxLen = MAX_INPUT_LEN;
  editorState.len = 0;
  editorState.pos = 0;
  editorState.startCol = cursorCol;
  editorState.startRow = cursorRow;
  editorState.historyEnabled = true;
  inputBuf[0] = '\0';
  inputPos = 0;
  editorLineActive = true;

  // NUMBER mode: pre-fill with the next auto line number + space
  if (numModeActive)
  {
    char numStr[8];
    int n = snprintf(numStr, sizeof(numStr), "%d ", numModeNext);
    for (int i = 0; i < n; i++)
    {
      editTypeChar(editorState, (uint8_t)numStr[i]);
    }
    numModeNext += numModeIncr;
  }
}

// True if the buffer is exactly "<digits> " (at least one trailing space)
// with nothing else — the NUMBER-mode auto-fill form. Lets us detect
// "Enter without adding anything" so we can exit NUMBER mode cleanly
// instead of deleting the line.
static bool editorBufferIsAutoFillOnly(const LineEdit& s)
{
  if (s.len == 0) return false;
  int p = 0;
  if (!isdigit((unsigned char)s.buf[p])) return false;
  while (p < s.len && isdigit((unsigned char)s.buf[p])) p++;
  if (p >= s.len || s.buf[p] != ' ') return false;
  while (p < s.len && s.buf[p] == ' ') p++;
  return p >= s.len;
}

static void editorCursorTick()
{
  if (!editorLineActive) return;
  unsigned long now = millis();
  if (now - editorLastBlink >= EDITOR_BLINK_MS)
  {
    editorCursorVisible = !editorCursorVisible;
    editSyncCursor(editorState);
    drawCursor(editorCursorVisible);
    editorLastBlink = now;
  }
}

static void checkInput()
{
  if (!editorLineActive)
  {
    editorBeginLine();
  }

  editorCursorTick();

  int c;
  while ((c = editorReadChar()) >= 0)
  {
    if (editorCursorVisible)
    {
      editSyncCursor(editorState);
      drawCursor(false);
      editorCursorVisible = false;
    }

    EditResult r = processEditChar((uint8_t)c, editorState);
    inputPos = editorState.len;

    if (r == EDIT_SUBMITTED)
    {
      inputReady = true;
      editorLineActive = false;
      return;
    }
    if (r == EDIT_BROKEN)
    {
      // No running program at the prompt — CLEAR just stays where it is
      continue;
    }
  }
}

// ---------------------------------------------------------------------------
// Forward declaration for recursive use by cmdOld
// ---------------------------------------------------------------------------
static void processInput(const char* input);

// ---------------------------------------------------------------------------
// Command callbacks (invoked by Token Parser for immediate commands)
// ---------------------------------------------------------------------------

static void cmdNew()
{
  em.clearProgram();
  fio::closeAll();
  sprites::clearAll();
  clearScreen();
  printLine("** READY **");
  showStatus("NEW program");
}

static void cmdList(int startLine, int endLine)
{
  char buf[256];
  for (int i = 0; i < em.programSize(); i++)
  {
    ProgramLine* line = em.getLine(i);
    if (startLine >= 0 && line->lineNum < startLine) continue;
    if (endLine   >= 0 && line->lineNum > endLine)   break;
    int n = snprintf(buf, sizeof(buf), "%d ", line->lineNum);
    detokenizeLine(line->tokens, line->length, &buf[n], sizeof(buf) - n);
    printLine(buf);
  }
}

static void cmdRun()
{
  em.run();
}

// EB-style autorun: at boot, search the conventional storage tiers
// for a program named "LOAD" and run it. Order:
//   1. DSK1..DSK<MAX_DSK>           (TI-faithful — PROGRAM-format)
//   2. FLASH  /LOAD.bas             (our text format)
//   3. SDCARD /LOAD.bas
// Returns true if one was found. There's no boot-time escape hatch:
// CLEAR (FCTN+4 / Ctrl+C / ESC) reliably stops a runaway LOAD program
// since BASIC has no path to disable that key.
static bool scanForLoadProgram()
{
  for (int d = 1; d <= fio::MAX_DSK; d++)
  {
    dsk::DskImage* img = fio::dskImage(d);
    if (!img) continue;
    dsk::FileInfo info;
    if (!img->findFile("LOAD", info)) continue;
    char spec[16];
    snprintf(spec, sizeof(spec), "DSK%c.LOAD", fio::driveToChar(d));
    cmdOld(spec);
    cmdRun();
    return true;
  }
  if (LittleFS.exists("/LOAD.bas"))
  {
    cmdOld("FLASH.LOAD");
    cmdRun();
    return true;
  }
  if (fio::g_sdOk && SD.exists("/LOAD.bas"))
  {
    cmdOld("SDCARD.LOAD");
    cmdRun();
    return true;
  }
  return false;
}

static void cmdBye()
{
  clearScreen();
  printLine("** GOODBYE **");
  delay(500);
  ESP.restart();
}

// Parse TI PROGRAM-file bytes and load each line into the ExecManager.
// File layout (big-endian words) — verified against real TI XB images:
//   0-1  : XOR checksum (ignored on load)
//   2-3  : LNT top — highest VDP address of the line-number table
//   4-5  : LNT bottom — lowest VDP address of the LNT (first entry)
//   6-7  : program top — highest VDP address used by the program text
// After the 8-byte header the file is a verbatim VDP-memory image
// starting at lntBot and running through progTop. VDP address A maps
// to file offset 8 + (A - lntBot).
// LNT storage is low-to-high in memory but sorted *descending* by line
// number. Each 4-byte entry: [line hi][line lo][tok-ptr hi][tok-ptr lo].
// Each program line's tokens: [length byte][length bytes of tokens].
static bool loadProgramBytes(const uint8_t* buf, int size)
{
  if (size < 12) return false;
  uint16_t lntTop  = ((uint16_t)buf[2] << 8) | buf[3];
  uint16_t lntBot  = ((uint16_t)buf[4] << 8) | buf[5];
  uint16_t progTop = ((uint16_t)buf[6] << 8) | buf[7];
  if (lntBot > lntTop || progTop < lntTop) return false;

  auto vdp2file = [&](uint16_t addr) -> int
  {
    if (addr < lntBot || addr > progTop) return -1;
    int off = 8 + (int)(addr - lntBot);
    return (off < size) ? off : -1;
  };

  em.clearProgram();

  for (uint16_t ent = lntBot; ent + 3 <= lntTop; ent += 4)
  {
    int off = vdp2file(ent);
    if (off < 0 || off + 4 > size) break;
    uint16_t lineNum = ((uint16_t)buf[off] << 8) | buf[off + 1];
    uint16_t tokPtr  = ((uint16_t)buf[off + 2] << 8) | buf[off + 3];
    // TI stores each line as: [length byte][tokens...][0x00].
    // The LNT pointer points to the FIRST token, one byte past the
    // length. So length is at tokPtr - 1.
    int tokOff = vdp2file(tokPtr);
    if (tokOff < 1 || tokOff >= size) continue;
    uint8_t len = buf[tokOff - 1];
    if (len == 0 || tokOff + len > size) continue;
    // Some TI writers store length including the trailing 0x00; others
    // don't. Strip a trailing 0x00 if present to keep our token stream
    // internally consistent (we'll add our own TOK_EOL).
    int copyLen = len;
    if (copyLen > 0 && buf[tokOff + copyLen - 1] == 0x00) copyLen--;
    if (copyLen + 1 > MAX_LINE_TOKENS) continue;

    uint8_t lineToks[MAX_LINE_TOKENS];
    memcpy(lineToks, &buf[tokOff], copyLen);
    lineToks[copyLen] = TOK_EOL;
    em.storeLine(lineNum, lineToks, copyLen + 1);
  }
  return em.programSize() > 0;
}

// Serialize the current program into a PROGRAM-format byte stream
// matching the layout parsed above. Returns number of bytes written.
// Caller supplies a buffer large enough to hold all tokens + LNT + header
// (typical small XB programs are well under 4 KB).
static int saveProgramBytes(uint8_t* buf, int bufSize)
{
  int n = em.programSize();
  if (n <= 0) return 0;

  // Compute sizes first. TI line format = [length][tokens...][0x00], so
  // each line consumes 2 + tok_count bytes.
  int textSize = 0;
  for (int i = 0; i < n; i++)
  {
    ProgramLine* line = em.getLine(i);
    int len = line->length;
    if (len > 0 && line->tokens[len - 1] == TOK_EOL) len--;
    textSize += 2 + len;
  }
  int lntSize = n * 4;
  int total = 8 + textSize + lntSize;
  if (total > bufSize) return -1;

  // Layout the file VDP-image starting at lntBot = 0x0008 (maps to
  // file offset 8 after the header). LNT goes first (low VDP), then
  // program text.
  uint16_t lntBot   = 0x0008;
  uint16_t lntTop   = lntBot + lntSize - 1;
  uint16_t textBase = lntTop + 1;
  uint16_t progTop  = textBase + textSize - 1;

  // Header: cksum, LNT top, LNT bottom, program top (all big-endian).
  uint16_t cksum = lntTop ^ lntBot ^ progTop;
  buf[0] = (cksum   >> 8) & 0xFF; buf[1] = cksum   & 0xFF;
  buf[2] = (lntTop  >> 8) & 0xFF; buf[3] = lntTop  & 0xFF;
  buf[4] = (lntBot  >> 8) & 0xFF; buf[5] = lntBot  & 0xFF;
  buf[6] = (progTop >> 8) & 0xFF; buf[7] = progTop & 0xFF;

  // Write program text into position starting at file offset 8 + lntSize.
  // TI convention: each line is stored as [length][tokens...][0x00];
  // the LNT pointer points to the FIRST TOKEN (past the length byte).
  int textFileOff = 8 + lntSize;
  uint16_t textWrite = textBase;
  uint16_t* linePtrs = (uint16_t*)malloc(sizeof(uint16_t) * n);
  if (!linePtrs) return -1;
  int off = textFileOff;
  for (int i = 0; i < n; i++)
  {
    ProgramLine* line = em.getLine(i);
    int len = line->length;
    if (len > 0 && line->tokens[len - 1] == TOK_EOL) len--;
    buf[off++] = (uint8_t)(len + 1);   // length includes trailing 0x00
    linePtrs[i] = textWrite + 1;       // pointer skips the length byte
    memcpy(&buf[off], line->tokens, len);
    off += len;
    buf[off++] = 0x00;                 // TI-style terminator
    textWrite += 2 + len;
  }

  // Write LNT at file offset 8, sorted descending by line number to
  // match TI convention (LNT grows downward in VDP as lines are added).
  int lntOff = 8;
  for (int i = n - 1; i >= 0; i--)
  {
    ProgramLine* line = em.getLine(i);
    buf[lntOff++] = (line->lineNum >> 8) & 0xFF;
    buf[lntOff++] = line->lineNum & 0xFF;
    buf[lntOff++] = (linePtrs[i] >> 8) & 0xFF;
    buf[lntOff++] = linePtrs[i] & 0xFF;
  }

  free(linePtrs);
  return total;
}

// Detect DSK<n>.NAME → return drive 1..35 and the TI filename.
// Returns 0 if not a DSK spec.
static int parseDskFileSpec(const char* in, char* nameOut, int nameSize)
{
  if (strncasecmp(in, "DSK", 3) != 0) return 0;
  int drive = fio::driveFromChar(in[3]);
  if (drive == 0 || in[4] != '.') return 0;
  snprintf(nameOut, nameSize, "%s", in + 5);
  return drive;
}

static void cmdSave(const char* filename)
{
  char tiName[16];
  int drive = parseDskFileSpec(filename, tiName, sizeof(tiName));
  if (drive > 0)
  {
    dsk::DskImage* img = fio::dskImage(drive);
    if (!img) { printError("* NOT MOUNTED"); return; }
    if (img->readOnly()) { printError("* WRITE PROTECTED"); return; }
    uint8_t* progBuf = (uint8_t*)malloc(8192);
    if (!progBuf) { printError("* OUT OF MEMORY"); return; }
    int size = saveProgramBytes(progBuf, 8192);
    if (size <= 0)
    {
      free(progBuf);
      printError("* PROGRAM TOO BIG");
      return;
    }
    bool ok = img->writeRawFile(tiName, progBuf, size, 0x01);
    free(progBuf);
    if (!ok)
    {
      printError("* FILE ERROR");
      return;
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "SAVED: DSK%c.%s (%d bytes)",
             fio::driveToChar(drive), tiName, size);
    printLine(msg);
    return;
  }

  // FLASH.name / SDCARD.name / bare name → text .bas file.
  fs::FS* targetFs = &LittleFS;
  const char* devLabel = "FLASH";
  const char* nameStart = filename;
  if (strncasecmp(filename, "FLASH.", 6) == 0)
  {
    targetFs = &LittleFS;
    devLabel = "FLASH";
    nameStart = filename + 6;
  }
  else if (strncasecmp(filename, "SDCARD.", 7) == 0)
  {
    if (!fio::g_sdOk) { printError("* DEVICE NOT PRESENT"); return; }
    targetFs = &SD;
    devLabel = "SDCARD";
    nameStart = filename + 7;
  }
  // else: bare name → defaults to FLASH for back-compat with old .bas files

  char path[48];
  snprintf(path, sizeof(path), "/%s.bas", nameStart);
  File f = targetFs->open(path, "w");
  if (!f)
  {
    printError("* FILE ERROR");
    return;
  }

  char buf[256];
  for (int i = 0; i < em.programSize(); i++)
  {
    ProgramLine* line = em.getLine(i);
    int n = snprintf(buf, sizeof(buf), "%d ", line->lineNum);
    detokenizeLine(line->tokens, line->length, &buf[n], sizeof(buf) - n);
    f.println(buf);
  }
  f.close();

  char msg[64];
  snprintf(msg, sizeof(msg), "SAVED: %s%s", devLabel, path);
  printLine(msg);
}

static void cmdOld(const char* filename)
{
  char tiName[16];
  int drive = parseDskFileSpec(filename, tiName, sizeof(tiName));
  if (drive > 0)
  {
    dsk::DskImage* img = fio::dskImage(drive);
    if (!img) { printError("* NOT MOUNTED"); return; }
    uint8_t* progBuf = (uint8_t*)malloc(8192);
    if (!progBuf) { printError("* OUT OF MEMORY"); return; }
    int size = img->readRawFile(tiName, progBuf, 8192);
    if (size < 12)
    {
      free(progBuf);
      printError("* FILE ERROR");
      return;
    }
    // Dump the first 16 bytes on failure so we can decode the header.
    bool ok = loadProgramBytes(progBuf, size);
    if (!ok)
    {
      Serial.print("PROGRAM header:");
      for (int i = 0; i < 16 && i < size; i++)
      {
        Serial.printf(" %02X", progBuf[i]);
      }
      Serial.println();
    }
    free(progBuf);
    if (!ok)
    {
      printError("* BAD PROGRAM");
      return;
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "LOADED: DSK%c.%s (%d lines)",
             fio::driveToChar(drive), tiName, em.programSize());
    printLine(msg);
    return;
  }

  // FLASH.name / SDCARD.name / bare name → text .bas file.
  fs::FS* sourceFs = &LittleFS;
  const char* devLabel = "FLASH";
  const char* nameStart = filename;
  if (strncasecmp(filename, "FLASH.", 6) == 0)
  {
    sourceFs = &LittleFS;
    devLabel = "FLASH";
    nameStart = filename + 6;
  }
  else if (strncasecmp(filename, "SDCARD.", 7) == 0)
  {
    if (!fio::g_sdOk) { printError("* DEVICE NOT PRESENT"); return; }
    sourceFs = &SD;
    devLabel = "SDCARD";
    nameStart = filename + 7;
  }

  char path[48];
  snprintf(path, sizeof(path), "/%s.bas", nameStart);
  File f = sourceFs->open(path, "r");
  if (!f)
  {
    printError("* FILE ERROR");
    return;
  }

  em.clearProgram();
  while (f.available())
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0)
    {
      processInput(line.c_str());
    }
  }
  f.close();

  char msg[64];
  snprintf(msg, sizeof(msg), "LOADED: %s%s", devLabel, path);
  printLine(msg);
}

// MERGE "filename" — load a text-format program from LittleFS and fold
// its lines into the current program. Line-number collisions overwrite
// (matching TI's MERGE behavior). Unlike OLD, the existing program
// is NOT cleared first.
static void cmdMerge(const char* filename)
{
  char path[48];
  snprintf(path, sizeof(path), "/%s.bas", filename);
  File f = LittleFS.open(path, "r");
  if (!f)
  {
    printError("* FILE ERROR");
    return;
  }

  int merged = 0;
  while (f.available())
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0)
    {
      processInput(line.c_str());
      merged++;
    }
  }
  f.close();

  char msg[48];
  snprintf(msg, sizeof(msg), "MERGED: %s (%d lines)", path, merged);
  printLine(msg);
}

static void cmdContinue()
{
  em.cont();
}

// DELETE <spec>
//   FLASH.NAME      → remove /NAME from LittleFS
//   SDCARD.NAME     → remove /NAME from SD
//   DSKn.NAME       → remove a TI file from the mounted .dsk image
//   NAME            → legacy: remove /NAME.bas from LittleFS (SAVE/OLD form)
static void cmdDelete(const char* filename)
{
  if (!filename || filename[0] == '\0')
  {
    printError("* BAD FILE NAME");
    return;
  }

  // Device-qualified form?
  fio::Device dev = fio::DEV_NONE;
  char innerPath[48];
  int drive = 0;
  if (fio::parseSpec(filename, dev, innerPath, sizeof(innerPath), drive))
  {
    bool ok = false;
    char label[56];
    if (dev == fio::DEV_FLASH)
    {
      ok = LittleFS.exists(innerPath) && LittleFS.remove(innerPath);
      snprintf(label, sizeof(label), "FLASH%s", innerPath);
    }
    else if (dev == fio::DEV_SD)
    {
      if (!fio::g_sdOk)
      {
        printError("* DEVICE NOT PRESENT");
        return;
      }
      ok = SD.exists(innerPath) && SD.remove(innerPath);
      snprintf(label, sizeof(label), "SDCARD%s", innerPath);
    }
    else if (dev == fio::DEV_DSK)
    {
      dsk::DskImage* img = fio::dskImage(drive);
      if (!img)
      {
        printError("* NOT MOUNTED");
        return;
      }
      if (img->readOnly())
      {
        printError("* WRITE PROTECTED");
        return;
      }
      ok = img->deleteFile(innerPath);
      snprintf(label, sizeof(label), "DSK%c.%s",
               fio::driveToChar(drive), innerPath);
    }
    if (!ok)
    {
      printError("* FILE ERROR");
      return;
    }
    char msg[72];
    snprintf(msg, sizeof(msg), "DELETED: %s", label);
    printLine(msg);
    return;
  }

  // Legacy: bare name → /NAME.bas on LittleFS (matches SAVE / OLD)
  char path[48];
  snprintf(path, sizeof(path), "/%s.bas", filename);
  if (!LittleFS.exists(path) || !LittleFS.remove(path))
  {
    printError("* FILE ERROR");
    return;
  }
  char msg[64];
  snprintf(msg, sizeof(msg), "DELETED: %s", path);
  printLine(msg);
}

// --- V9T9 .dsk mount table persistence ---
// The mount table lives in LittleFS as "/mounts.cfg" — three lines, one
// per drive; blank line = unmounted. Loaded at setup() after SD comes up,
// saved whenever a MOUNT / UNMOUNT happens.
static void saveMounts()
{
  File f = LittleFS.open("/mounts.cfg", "w");
  if (!f) return;
  for (int d = 1; d <= fio::MAX_DSK; d++)
  {
    f.println(fio::dskImagePath(d));
  }
  f.close();
}

static void loadMounts()
{
  File f = LittleFS.open("/mounts.cfg", "r");
  if (!f) return;
  for (int d = 1; d <= fio::MAX_DSK && f.available(); d++)
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0)
    {
      fio::mountDskImage(d, line.c_str());
    }
  }
  f.close();
}

// MOUNT DSK<n> <spec>
//   <spec> forms accepted:
//     FLASH.FILE.DSK    — internal LittleFS
//     SDCARD.FILE.DSK   — external SD card
//     /FILE.DSK         — absolute SD path (legacy)
//     FILE              — bare name, .DSK auto-appended, SD card
// All parsing is done by fio::resolveMountSpec so the two entry points
// (MOUNT and the persisted mounts.cfg loader) agree on routing.
static void cmdMount(int drive, const char* imageName)
{
  if (drive < 1 || drive > fio::MAX_DSK)
  {
    printError("* BAD DEVICE");
    return;
  }
  if (!fio::mountDskImage(drive, imageName))
  {
    int reason = fio::g_mounts[drive].img.openReason;
    const char* why = (reason == 1) ? "* OPEN FAILED" :
                      (reason == 2) ? "* READ FAILED" :
                      (reason == 3) ? "* BAD VIB" :
                                      "* MOUNT FAILED";
    printError(why);
    return;
  }
  saveMounts();
  char msg[64];
  snprintf(msg, sizeof(msg), "DSK%c = %s  [%s  %d sectors]",
           fio::driveToChar(drive), imageName,
           fio::g_mounts[drive].img.vib().name,
           fio::g_mounts[drive].img.vib().totalSectors);
  printLine(msg);
}

// NEWDISK <device.name> "VOLNAME" [SSSD|DSSD|DSDD]
// Creates a fresh V9T9 disk image at the given LittleFS or SD location.
static void cmdNewDisk(const char* spec, const char* volName,
                       int totalSectors)
{
  if (!spec || !spec[0])
  {
    printError("* BAD FILE NAME");
    return;
  }

  bool fromFlash;
  char fsPath[48];
  if (!fio::resolveMountSpec(spec, fromFlash, fsPath, sizeof(fsPath)))
  {
    printError("* BAD DEVICE");
    return;
  }
  if (!fromFlash && !fio::g_sdOk)
  {
    printError("* DEVICE NOT PRESENT");
    return;
  }
  fs::FS& fs = fromFlash ? (fs::FS&)LittleFS : (fs::FS&)SD;

  // Guard against clobbering an existing image. Prompt for Y/N.
  if (fs.exists(fsPath))
  {
    char warn[64];
    snprintf(warn, sizeof(warn), "* %s EXISTS. OVERWRITE? (Y/N)", spec);
    printLine(warn);
    int c = 0;
    do
    {
      c = editorReadChar();
      bleKbTask();
      yield();
    } while (c < 0);
    if (c != 'Y' && c != 'y')
    {
      printLine("CANCELLED");
      return;
    }
    // Remove existing so create() starts clean
    fs.remove(fsPath);
  }

  if (!dsk::DskImage::create(fs, fsPath, volName, totalSectors))
  {
    printError("* CREATE FAILED");
    return;
  }
  // Flash erases mask interrupts briefly; repaint so any glitched
  // rows from that window don't stick.
  paintBorder();
  redrawScreen();
  char msg[64];
  const char* sizeLabel = (totalSectors == 360)  ? "SSSD" :
                          (totalSectors == 720)  ? "DSSD" : "DSDD";
  snprintf(msg, sizeof(msg), "CREATED %s %s [%s  %d sectors]",
           sizeLabel, spec, volName, totalSectors);
  printLine(msg);
}

// COPY <src-spec> <dst-spec>
// Line-level copy between FLASH, SDCARD, and mounted DSK drives.
// Uses the existing file-I/O layer so every direction works the same.
static void cmdCopy(const char* src, const char* dst)
{
  if (fio::openFile(5, src, fio::MODE_INPUT) != 0)
  {
    printError("* SOURCE OPEN FAILED");
    return;
  }
  if (fio::openFile(6, dst, fio::MODE_OUTPUT) != 0)
  {
    fio::closeFile(5);
    printError("* DEST OPEN FAILED");
    return;
  }
  int lines = 0;
  char line[MAX_STR_LEN];
  while (!fio::isEof(5))
  {
    if (fio::readLineFrom(5, line, sizeof(line)) != 0) break;
    fio::printLineTo(6, line);
    lines++;
  }
  fio::closeFile(6);
  fio::closeFile(5);
  char msg[48];
  snprintf(msg, sizeof(msg), "COPIED %d LINES", lines);
  printLine(msg);
}

static void cmdUnmount(int drive)
{
  if (drive < 1 || drive > fio::MAX_DSK)
  {
    printError("* BAD DEVICE");
    return;
  }
  fio::unmountDskImage(drive);
  saveMounts();
  char msg[32];
  snprintf(msg, sizeof(msg), "DSK%c UNMOUNTED", fio::driveToChar(drive));
  printLine(msg);
}

// --- File I/O shims (wired into TokenParser via setFileCallbacks) ---
static int shimFileOpen(int unit, const char* spec, int mode,
                        int flags, int recLen)
{
  return fio::openFile(unit, spec, (fio::Mode)mode, flags, recLen);
}
static int shimFileClose(int unit) { return fio::closeFile(unit); }
static int shimFilePrint(int unit, const char* text)
{
  return fio::printLineTo(unit, text);
}
static int shimFileReadLine(int unit, char* buf, int bufSize)
{
  return fio::readLineFrom(unit, buf, bufSize);
}
static bool shimFileEof(int unit) { return fio::isEof(unit); }
static bool shimFileSeekRec(int unit, long rec)
{
  return fio::seekRecord(unit, rec);
}
static bool shimFileRewind(int unit)
{
  return fio::rewindFile(unit);
}

// TokenParser's CmdDirFn is void(), so register this wrapper for the
// (currently unused) tokenized TOK_DIR path. Pre-tokenize dispatch in
// processInput calls cmdDirOn directly with the parsed device name.
static void cmdDir() { cmdDirOn("FLASH"); }

// DIR [FLASH|DSK1] — list files on a device. If no argument given,
// list FLASH (the built-in LittleFS). Use DIR DSK1 to list the SD card.
// Print a CAT/DIR line with TI-style paging: after 23 lines, prompt
// "PRESS ANY KEY" and block until the user hits a key. Reset on return.
static int g_catLines = 0;
static bool g_catCancelled = false;
static void catPrintLine(const char* s)
{
  if (g_catCancelled) return;
  printLine(s);
  g_catLines++;
  if (g_catLines >= 23)
  {
    printLine("* PRESS ANY KEY TO CONTINUE *");
    int c = -1;
    while (c < 0)
    {
      bleKbTask();
      yield();
      c = editorReadChar();
    }
    if (c == 0x1B || c == 0x03 || c == 12) g_catCancelled = true;
    g_catLines = 0;
  }
}

static void cmdDirOn(const char* device)
{
  g_catLines = 0;
  g_catCancelled = false;
  // DSK<n>: list files inside the mounted V9T9 .dsk image, matching
  // the TI Disk Manager cartridge's catalog columns. Drives 1-9 and
  // A-Z are all valid.
  if (strncasecmp(device, "DSK", 3) == 0 &&
      (device[4] == '\0' || device[4] == ' ') &&
      fio::driveFromChar(device[3]) > 0)
  {
    int drive = fio::driveFromChar(device[3]);
    dsk::DskImage* img = fio::dskImage(drive);
    if (!img)
    {
      printError("* NOT MOUNTED");
      return;
    }
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "DSK%c  %s  %d FREE",
             fio::driveToChar(drive), img->vib().name,
             img->freeSectors());
    catPrintLine(hdr);
    catPrintLine("FILENAME    TYPE      SIZE");

    dsk::DskImage::CatEntry ents[64];
    int n = img->listCatalog(ents, 64);
    for (int i = 0; i < n && !g_catCancelled; i++)
    {
      const auto& e = ents[i];
      char typeStr[12];
      if (e.flags & 0x01)
      {
        snprintf(typeStr, sizeof(typeStr), "PROGRAM");
      }
      else
      {
        const char* k1 = (e.flags & 0x02) ? "INT" : "DIS";
        const char* k2 = (e.flags & 0x40) ? "VAR" : "FIX";
        snprintf(typeStr, sizeof(typeStr), "%s/%s %d", k1, k2, e.recLen);
      }
      char lock = (e.flags & 0x08) ? 'P' : ' ';
      char buf[40];
      snprintf(buf, sizeof(buf), "%-10s %c %-10s %4d",
               e.name, lock, typeStr, e.totalSectors);
      catPrintLine(buf);
    }
    if (n == 0) catPrintLine("  (empty)");
    return;
  }

  fs::FS* fsRef = nullptr;
  const char* label = "FLASH";
  if (strcasecmp(device, "SDCARD") == 0 || strcasecmp(device, "SD") == 0)
  {
    if (!fio::g_sdOk)
    {
      printError("* DEVICE NOT PRESENT");
      return;
    }
    fsRef = &SD;
    label = "SDCARD";
  }
  else
  {
    fsRef = &LittleFS;
    label = "FLASH";
  }

  char hdr[32];
  snprintf(hdr, sizeof(hdr), "%s:", label);
  catPrintLine(hdr);

  File root = fsRef->open("/");
  File f = root.openNextFile();
  int shown = 0;
  while (f && !g_catCancelled)
  {
    const char* name = f.name();
    // Skip Windows-internal folders and dotfiles; skip directories
    // entirely (we only do flat storage).
    bool hide = f.isDirectory() || name[0] == '.' ||
                strcasecmp(name, "System Volume Information") == 0;
    if (!hide)
    {
      char buf[48];
      snprintf(buf, sizeof(buf), "  %-20s %d", name, (int)f.size());
      catPrintLine(buf);
      shown++;
    }
    f = root.openNextFile();
  }
  if (shown == 0) catPrintLine("  (no files)");
}

// SIZE — print free memory, TI-style. Real TI reported stack + program
// space separately; we approximate with free heap (for stack/vars) and
// bytes remaining in the tokenized-program buffer (for program).
static void cmdSize()
{
  char buf[48];
  int programUsed = 0;
  for (int i = 0; i < em.programSize(); i++)
  {
    ProgramLine* pl = em.getLine(i);
    if (pl) programUsed += pl->length + 4;    // rough per-line overhead
  }
  int programFree = (MAX_LINES * 140) - programUsed;  // approximate
  if (programFree < 0) programFree = 0;

  snprintf(buf, sizeof(buf), "%d BYTES OF STACK FREE",
           (int)ESP.getFreeHeap());
  printLine(buf);
  snprintf(buf, sizeof(buf), "%d BYTES OF PROGRAM SPACE FREE", programFree);
  printLine(buf);
}

// TRACE / UNTRACE — forward to the execution manager so it can print
// line numbers as they execute.
static void cmdTrace(bool enable)
{
  em.setTrace(enable);
}

// Breakpoint list — stored in exec_manager. cmdBreak is called with
// add=true for BREAK and add=false for UNBREAK. A zero-length list
// means "all" (BREAK alone does nothing at prompt, UNBREAK alone clears).
static void cmdBreak(const int* lines, int count, bool add)
{
  if (count == 0)
  {
    if (!add) em.clearBreakpoints();
    return;
  }
  for (int i = 0; i < count; i++)
  {
    if (add) em.addBreakpoint(lines[i]);
    else     em.removeBreakpoint(lines[i]);
  }
}

// NUMBER [start, increment] — enters auto-line-number mode. Handled by
// the editor loop; we just set the state flags and first line number.
static void cmdNumber(int startLine, int increment)
{
  numModeStart = startLine;
  numModeIncr  = (increment > 0) ? increment : 10;
  numModeNext  = startLine;
  numModeActive = true;
}

// RESEQUENCE [start, increment] — renumber program lines. Also updates
// GOTO / GOSUB / THEN / ELSE line-number references inside each line.
static void cmdResequence(int startLine, int increment)
{
  if (increment <= 0) increment = 10;
  if (startLine <= 0) startLine = 100;

  int n = em.programSize();
  if (n == 0) return;

  // Build oldLineNum → newLineNum mapping
  static uint16_t mapOld[MAX_LINES];
  static uint16_t mapNew[MAX_LINES];
  for (int i = 0; i < n; i++)
  {
    mapOld[i] = em.getLine(i)->lineNum;
    mapNew[i] = (uint16_t)(startLine + i * increment);
  }

  // Rewrite each line's tokens: replace old line-number references
  // with the new numbers. Tokens following GOTO/GOSUB/THEN/ELSE/
  // RESTORE/BREAK/UNBREAK/RESEQUENCE/ON ERROR hold line numbers —
  // the tokenizer now encodes them as TOK_LINENUM (0xC9 + 2 bytes)
  // so the rewriter is a simple in-place byte patch.
  auto remapLine = [&](uint16_t oldNum) -> uint16_t
  {
    for (int j = 0; j < n; j++)
    {
      if (mapOld[j] == oldNum) return mapNew[j];
    }
    return oldNum;
  };

  for (int i = 0; i < n; i++)
  {
    ProgramLine* pl = em.getLine(i);
    int p = 0;
    while (p < pl->length && pl->tokens[p] != TOK_EOL)
    {
      uint8_t t = pl->tokens[p];
      if (t == TOK_LINENUM && p + 2 < pl->length)
      {
        uint16_t oldNum = ((uint16_t)pl->tokens[p + 1] << 8) |
                          pl->tokens[p + 2];
        uint16_t newNum = remapLine(oldNum);
        pl->tokens[p + 1] = (newNum >> 8) & 0xFF;
        pl->tokens[p + 2] = newNum & 0xFF;
        p += 3;
      }
      else if (t == TOK_QUOTED_STR || t == TOK_UNQUOTED_STR)
      {
        // Skip past literal payload
        uint8_t slen = (p + 1 < pl->length) ? pl->tokens[p + 1] : 0;
        p += 2 + slen;
      }
      else
      {
        p++;
      }
    }
  }

  // Finally, rewrite the line numbers themselves
  for (int i = 0; i < n; i++)
  {
    em.getLine(i)->lineNum = mapNew[i];
  }
}

// ---------------------------------------------------------------------------
// Command processor (handles typed input)
// ---------------------------------------------------------------------------

static void processInput(const char* input)
{
  int pos = 0;
  while (input[pos] == ' ')
  {
    pos++;
  }

  if (input[pos] == '\0')
  {
    return;
  }

  // Pre-tokenize commands — string-matched immediate commands
  // These don't get tokens; they're handled directly.
  if (strncasecmp(&input[pos], "NEW", 3) == 0 &&
      (input[pos + 3] == '\0' || input[pos + 3] == ' '))
  {
    cmdNew();
    return;
  }
  if (strncasecmp(&input[pos], "RUN", 3) == 0 &&
      (input[pos + 3] == '\0' || input[pos + 3] == ' '))
  {
    cmdRun();
    return;
  }
  // CAT[ALOG] / DIR — list files on a device (defaults to FLASH).
  // TI-style: CATALOG is the conventional name; DIR kept as a habit alias.
  {
    int kwLen = 0;
    if (strncasecmp(&input[pos], "CATALOG", 7) == 0 &&
        (input[pos + 7] == '\0' || input[pos + 7] == ' '))
    {
      kwLen = 7;
    }
    else if (strncasecmp(&input[pos], "CAT", 3) == 0 &&
             (input[pos + 3] == '\0' || input[pos + 3] == ' '))
    {
      kwLen = 3;
    }
    else if (strncasecmp(&input[pos], "DIR", 3) == 0 &&
             (input[pos + 3] == '\0' || input[pos + 3] == ' '))
    {
      kwLen = 3;
    }
    if (kwLen > 0)
    {
      int p = pos + kwLen;
      while (input[p] == ' ') p++;
      cmdDirOn(input[p] == '\0' ? "FLASH" : &input[p]);
      return;
    }
  }
  // MOUNT DSK<n> <image>
  if (strncasecmp(&input[pos], "MOUNT", 5) == 0 &&
      (input[pos + 5] == '\0' || input[pos + 5] == ' '))
  {
    int p = pos + 5;
    while (input[p] == ' ') p++;
    // Bare `MOUNT` with no args → list every mounted drive.
    if (input[p] == '\0')
    {
      int shown = 0;
      for (int d = 1; d <= fio::MAX_DSK; d++)
      {
        if (!fio::g_mounts[d].mounted) continue;
        char buf[64];
        snprintf(buf, sizeof(buf), "DSK%c = %s",
                 fio::driveToChar(d), fio::g_mounts[d].spec);
        printLine(buf);
        shown++;
      }
      if (shown == 0) printLine("(no disks mounted)");
      return;
    }
    int drive = 0;
    if (strncasecmp(&input[p], "DSK", 3) == 0)
    {
      drive = fio::driveFromChar(input[p + 3]);
    }
    if (drive == 0)
    {
      printError("* BAD DEVICE");
      return;
    }
    p += 4;
    while (input[p] == ' ') p++;
    if (input[p] == '"')
    {
      p++;
      char name[48]; int n = 0;
      while (input[p] && input[p] != '"' && n < (int)sizeof(name) - 1)
      {
        name[n++] = input[p++];
      }
      name[n] = '\0';
      cmdMount(drive, name);
    }
    else if (input[p] != '\0')
    {
      cmdMount(drive, &input[p]);
    }
    else
    {
      printError("* BAD FILE NAME");
    }
    return;
  }
  // UNMOUNT DSK<n>
  if (strncasecmp(&input[pos], "UNMOUNT", 7) == 0 &&
      (input[pos + 7] == '\0' || input[pos + 7] == ' '))
  {
    int p = pos + 7;
    while (input[p] == ' ') p++;
    int drive = 0;
    if (strncasecmp(&input[p], "DSK", 3) == 0)
    {
      drive = fio::driveFromChar(input[p + 3]);
    }
    if (drive == 0)
    {
      printError("* BAD DEVICE");
      return;
    }
    cmdUnmount(drive);
    return;
  }
  // NEWDISK <spec> "VOLNAME" [SSSD|DSSD|DSDD]
  if (strncasecmp(&input[pos], "NEWDISK", 7) == 0 &&
      (input[pos + 7] == '\0' || input[pos + 7] == ' '))
  {
    int p = pos + 7;
    while (input[p] == ' ') p++;

    char spec[48] = {0};
    int n = 0;
    while (input[p] && input[p] != ' ' && input[p] != ',' &&
           n < (int)sizeof(spec) - 1)
    {
      spec[n++] = input[p++];
    }
    spec[n] = '\0';

    char volName[12] = "NEWDISK";
    while (input[p] == ' ' || input[p] == ',') p++;
    if (input[p] == '"')
    {
      p++;
      n = 0;
      while (input[p] && input[p] != '"' && n < (int)sizeof(volName) - 1)
      {
        char c = input[p++];
        if (c >= 'a' && c <= 'z') c -= 32;
        volName[n++] = c;
      }
      volName[n] = '\0';
      if (input[p] == '"') p++;
    }

    int totalSectors = 360;   // SSSD default
    while (input[p] == ' ' || input[p] == ',') p++;
    if (strncasecmp(&input[p], "DSSD", 4) == 0) totalSectors = 720;
    else if (strncasecmp(&input[p], "DSDD", 4) == 0) totalSectors = 1440;

    cmdNewDisk(spec, volName, totalSectors);
    return;
  }
  // COPY <src> <dst>
  if (strncasecmp(&input[pos], "COPY", 4) == 0 &&
      (input[pos + 4] == '\0' || input[pos + 4] == ' '))
  {
    int p = pos + 4;
    while (input[p] == ' ') p++;

    char src[48] = {0};
    int n = 0;
    while (input[p] && input[p] != ' ' && input[p] != ',' &&
           n < (int)sizeof(src) - 1)
    {
      src[n++] = input[p++];
    }
    src[n] = '\0';

    while (input[p] == ' ' || input[p] == ',') p++;

    char dst[48] = {0};
    n = 0;
    while (input[p] && input[p] != ' ' && n < (int)sizeof(dst) - 1)
    {
      dst[n++] = input[p++];
    }
    dst[n] = '\0';

    if (!src[0] || !dst[0])
    {
      printError("* BAD FILE NAME");
      return;
    }
    cmdCopy(src, dst);
    return;
  }

  // Numbered line — store in program
  if (isdigit(input[pos]))
  {
    char* endp;
    uint16_t lineNum = (uint16_t)strtol(&input[pos], &endp, 10);
    pos = endp - input;

    while (input[pos] == ' ')
    {
      pos++;
    }

    // Just a line number — delete line
    if (input[pos] == '\0')
    {
      em.deleteLine(lineNum);
      return;
    }

    // Tokenize and store
    uint8_t tokens[MAX_LINE_TOKENS];
    int len = tokenizeLine(&input[pos], tokens, MAX_LINE_TOKENS);
    if (len < 0)
    {
      printError("* SYNTAX ERROR");
      return;
    }

    if (!em.storeLine(lineNum, tokens, len))
    {
      printError("* MEMORY FULL");
      return;
    }

    char status[40];
    snprintf(status, sizeof(status), "Lines: %d  Free: %dK",
             em.programSize(), (int)(ESP.getFreeHeap() / 1024));
    showStatus(status);
    return;
  }

  // Tokenize and execute immediately through the TP
  uint8_t tokens[MAX_LINE_TOKENS];
  int len = tokenizeLine(&input[pos], tokens, MAX_LINE_TOKENS);
  if (len < 0)
  {
    printLine("* SYNTAX ERROR");
    return;
  }

  em.runImmediate(tokens, len);
}

// ---------------------------------------------------------------------------
// Setup and main loop
// ---------------------------------------------------------------------------

void setup()
{
  // Bump the RX buffer up from the 256-byte default so long program
  // pastes aren't dropped before the editor can drain them.
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  delay(500);

  // (No onboard status LEDs on the 8048S043C)

  // Initialize LittleFS
  if (!LittleFS.begin(true))
  {
    Serial.println("LittleFS mount failed!");
  }
  else
  {
    Serial.println("LittleFS mounted.");
  }

  // Bring up the SD card on the 8048S043's TF slot. Failure is non-fatal
  // (no card inserted, etc.) — DSK1..3 just won't mount in that case.
  if (fio::beginSD(/*cs=*/10, /*sck=*/12, /*miso=*/13, /*mosi=*/11))
  {
    Serial.println("SD card mounted.");
    loadMounts();   // auto-remount any persisted DSK images
  }
  else
  {
    Serial.println("SD card not present (DSK1..3 disabled).");
  }

  initTokenNames();
  initDisplay();
  initCharPatterns();
  gfxResetColors();

  // Initialize framebuffer
  for (int r = 0; r < ROWS; r++)
  {
    memset(screenBuf[r], ' ', COLS);
    memset(prevScreenBuf[r], 0, COLS);
  }

  clearScreen();

  // Bring up BLE HID keyboard input BEFORE the boot screen so the scan
  // can start reconnecting while the user is still on the splash screens.
  // (F12 or BOOT button = pairing mode.)
  bleKbInit();

  // Show TI boot screen and wait for a key
  showBootScreen();

  // Cursor starts at bottom row (TI behavior)
  cursorRow = ROWS - 1;
  cursorCol = 0;
  printLine("* READY *");
  printString(">");

  // Connect display callbacks to the Token Parser
  em.tp()->setCallbacks(printChar, printString, clearScreen);
  em.tp()->setCommandCallbacks(cmdNew, cmdList, cmdRun, cmdSave,
                               cmdOld, cmdBye, cmdDir);
  em.tp()->setGraphicsCallbacks(gfxSetChar, gfxGetChar,
                                gfxSetScreenColor, gfxSetCharColor,
                                gfxSetCharPattern);
  em.tp()->setReadKey(gfxReadKey);
  em.tp()->setReadJoystick([](int unit, int* outX, int* outY) {
    *outX = bleGpJoystickX(unit);
    *outY = bleGpJoystickY(unit);
  });
  em.tp()->setMoveCursor(gfxMoveCursor);
  em.tp()->setGetCharPattern(gfxGetCharPattern);
  em.tp()->setResetCharset(gfxResetCharset);
  em.tp()->setCmdSize(cmdSize);
  em.tp()->setCmdTrace(cmdTrace);
  em.tp()->setCmdBreak(cmdBreak);
  em.tp()->setCmdRes(cmdResequence);
  em.tp()->setCmdNum(cmdNumber);
  em.tp()->setCmdMerge(cmdMerge);
  em.tp()->setCmdDelete(cmdDelete);
  em.tp()->setCmdContinue(cmdContinue);
  em.tp()->setFileCallbacks(shimFileOpen, shimFileClose, shimFilePrint,
                            shimFileReadLine, shimFileEof);
  em.tp()->setFileSeekRec(shimFileSeekRec);
  em.tp()->setFileRewind(shimFileRewind);

  em.tp()->setSpriteCallbacks(
      // Drawing one sprite from BASIC (CALL SPRITE / CALL PATTERN /
      // CALL LOCATE) must not paint it over higher-priority sprites
      // it overlaps. After drawing #n, redraw #n-1..#1 so any of
      // them that overlaps stays on top.
      /*draw=*/[](int n)
      {
        if (!sprites::validSlot(n)) return;
        spriteDraw(sprites::g_sprites[n]);
        for (int p = n - 1; p >= 1; p--)
        {
          if (sprites::g_sprites[p].active) spriteDraw(sprites::g_sprites[p]);
        }
      },
      /*erase=*/[](int n) { if (sprites::validSlot(n)) spriteErase(sprites::g_sprites[n]); });

  // CALL SPEED routes here. Mirror to both EM (per-line throttle) and
  // TP (per-statement throttle, fires on every `::` as well as line end).
  em.tp()->setThrottleCallback([](unsigned long us) {
    em.m_throttleUs = us;
    em.tp()->setThrottleUs(us);
  });
  em.setProgramEnded(gfxReset);
  em.setPerLineTick(spriteTick);

  // Look up an IMAGE line by line number. Walks the program, finds
  // the line, and if it starts with TOK_IMAGE + TOK_QUOTED_STR returns
  // a pointer to the format string and its length.
  em.tp()->setImageLookup([](uint16_t lineNum,
                             const char** outStr, int* outLen) -> bool {
    int idx = em.findLineIndex(lineNum);
    if (idx >= em.programSize()) return false;
    ProgramLine* pl = em.getLine(idx);
    if (!pl || pl->lineNum != lineNum) return false;
    if (pl->length < 3) return false;
    if (pl->tokens[0] != TOK_IMAGE) return false;
    if (pl->tokens[1] != TOK_QUOTED_STR &&
        pl->tokens[1] != TOK_UNQUOTED_STR) return false;
    int sLen = pl->tokens[2];
    *outStr = (const char*)&pl->tokens[3];
    *outLen = sLen;
    return true;
  });
  em.setPrepareInput(gfxPrepareInput);
  em.setPrintLine(printLine);
  em.setPrintError(printError);
  em.setPrintString(printString);
  em.setGetLine(getInputLine);

  char statusBuf[40];
  snprintf(statusBuf, sizeof(statusBuf), "TI BASIC Sim | Free: %dK",
           (int)(ESP.getFreeHeap() / 1024));
  showStatus(statusBuf);

  Serial.println("TI Extended BASIC Simulator v0.2");
  Serial.println("Type BASIC commands. Serial input active.");

  // EB autorun convention: a program named LOAD on DSK1..n / FLASH /
  // SDCARD is loaded and run automatically on boot.
  scanForLoadProgram();
}

void loop()
{
  pasteDrainSerial();
  bleKbTask();
  spriteTick();
  checkInput();

  if (inputReady)
  {
    processInput(inputBuf);
    printString(">");
    inputPos = 0;
    inputReady = false;
  }

  // Commit pending writes ~60 Hz max so non-sprite changes (typed
  // characters, REPL output, screen clears, etc.) reach the panel.
  // Rate-limited because each commit is a full-screen memcpy on
  // PSRAM (~10 ms) — running every loop iteration would waste CPU.
  // Sprite motion already flushes inside spriteTick when a sprite
  // actually moved, so most active frames are covered there.
  static uint32_t lastFlush = 0;
  uint32_t now = millis();
  if (now - lastFlush >= 16)
  {
    tft->flush();
    lastFlush = now;
  }
}
