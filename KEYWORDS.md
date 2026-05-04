# TI Extended BASIC Keyword Status

Status legend:
- **Impl** — implementation in place in the simulator
- **Test** — verified working on device with at least one program
- *(blank)* — not yet implemented / not yet tested

---

## Immediate-mode commands

| Keyword        | Impl | Test | Notes                                          |
|----------------|:----:|:----:|------------------------------------------------|
| NEW            |  ✅  |  ✅  | Clears program and screen                      |
| RUN            |  ✅  |  ✅  |                                                |
| LIST           |  ✅  |  ✅  | Full, single line, range: LIST n / n- / -m / n-m |
| OLD            |  ✅  |  ✅  | Loads from LittleFS as `/NAME.bas`             |
| SAVE           |  ✅  |  ✅  | Saves text form to LittleFS                    |
| MERGE          |  ✅  |  ✅  | Fold file into current program; collisions win |
| BYE            |  ✅  |  ✅  | Restarts the ESP32                             |
| CAT / CATALOG  |  ✅  |  ✅  | `CAT [FLASH\|SDCARD\|DSK1..3]`; `DIR` is an alias |
| SIZE           |  ✅  |  ✅  | Prints free heap + estimated program space     |
| NUMBER / NUM   |  ✅  |  ✅  | Auto line-number input mode                    |
| RESEQUENCE/RES |  ✅  |  ✅  | Renumbers lines + GOTO/GOSUB/THEN/ELSE targets |
| BREAK          |  ✅  |  ✅  | Set breakpoint line list                       |
| UNBREAK        |  ✅  |  ✅  | Clear breakpoints                              |
| TRACE          |  ✅  |  ✅  | Prints `<lineN>` before each line              |
| UNTRACE        |  ✅  |  ✅  |                                                |
| CON/CONTINUE   |  ✅  |  ✅  | Resumes after STOP / breakpoint / BREAK        |
| DELETE (file)  |  ✅  |  ✅  | `DELETE NAME` or `DELETE "NAME"`, removes .bas |

## Program control flow

| Keyword        | Impl | Test | Notes                                      |
|----------------|:----:|:----:|--------------------------------------------|
| IF / THEN      |  ✅  |  ✅  |                                            |
| ELSE           |  ✅  |  ✅  |                                            |
| FOR / TO       |  ✅  |  ✅  |                                            |
| STEP           |  ✅  |  ✅  |                                            |
| NEXT           |  ✅  |  ✅  |                                            |
| GOTO / GO TO   |  ✅  |  ✅  |                                            |
| GOSUB          |  ✅  |  ✅  |                                            |
| RETURN         |  ✅  |  ✅  |                                            |
| ON ... GOTO    |  ✅  |  ✅  |                                            |
| ON ... GOSUB   |  ✅  |  ✅  |                                            |
| END            |  ✅  |  ✅  |                                            |
| STOP           |  ✅  |  ✅  |                                            |
| REM            |  ✅  |  ✅  |                                            |
| `!` tail comment |  ✅  |  ✅  | TOK_BANG 0x83; stops statement + line processing |
| ON BREAK       |  ✅  |  ✅  | STOP (default) / NEXT                      |
| ON ERROR       |  ✅  |  ✅  | `<line>` / STOP (default) / NEXT; disarms on entry |
| ON WARNING     |  ✅  |  ✅  | STOP (→error) / PRINT (default) / NEXT         |

## Variables, assignment, I/O

| Keyword        | Impl | Test | Notes                                        |
|----------------|:----:|:----:|----------------------------------------------|
| LET (implicit) |  ✅  |  ✅  | Bare `VAR = expr` supported                  |
| DIM            |  ✅  |  ✅  | 1D/2D/3D arrays, numeric and string          |
| OPTION BASE    |  ✅  |  ✅  | 0 or 1                                       |
| PRINT          |  ✅  |  ✅  | `;` `,` `TAB()` supported                    |
| INPUT          |  ✅  |  ✅  |                                              |
| LINPUT         |  ✅  |  ✅  |                                              |
| DISPLAY AT     |  ✅  |  ✅  |                                              |
| ACCEPT AT      |  ✅  |  ✅  |                                              |
| READ           |  ✅  |  ✅  |                                              |
| DATA           |  ✅  |  ✅  |                                              |
| RESTORE        |  ✅  |  ✅  |                                              |
| RANDOMIZE      |  ✅  |  ✅  |                                              |
| DEF            |  ✅  |  ✅  | `DEF FNx(y)=expr` single-line, numeric + `FNx$` |
| SUB            |  ✅  |  ✅  | Numeric+string params, pass-by-value-result  |
| SUBEND         |  ✅  |  ✅  |                                              |
| SUBEXIT        |  ✅  |  ✅  | Early return from subprogram                 |
| OPEN           |  ✅  |  ✅  | `OPEN #n:"FLASH.NAME"`/`"SDCARD.NAME"`/`"DSKn.NAME"`; DIS/VAR + FIXED + RELATIVE; INTERNAL parsed (treated as DISPLAY) |
| CLOSE          |  ✅  |  ✅  | `CLOSE #n [,#m ...]`                         |
| PRINT #        |  ✅  |  ✅  | One line per statement; `;` and `,` work; `,REC k` for relative access |
| INPUT #        |  ✅  |  ✅  | Comma-split from one line; `,REC k` for relative access |
| LINPUT #       |  ✅  |  ✅  | Whole line into first string var; `,REC k` for relative access |
| EOF(n)         |  ✅  |  ✅  | -1 at end-of-file, 0 otherwise               |
| RESTORE #      |  ✅  |  ✅  | `RESTORE #n` — rewind file to first record without closing |
| FIXED N        |  ✅  |  ✅  | `OPEN ...,FIXED 80` — N-byte records, no LF terminator |
| RELATIVE       |  ✅  |  ✅  | `OPEN ...,RELATIVE` — pair with `REC k` on PRINT/INPUT |
| INTERNAL       | (✅) |      | **Known limitation:** parsed but stored as DISPLAY (no radix-100). Not interoperable with real TI INTERNAL files. See EXTENSIONS.md. |
| IMAGE          |  ✅  |  ✅  | Stored verbatim; looked up by line# from PRINT USING |
| PRINT USING    |  ✅  |  ✅  | `PRINT USING <lineN>:` or `PRINT USING <str>:`; `#` `.` `+` `-` `^^^^` edit chars |
| DISPLAY USING  |  ✅  |  ✅  | DISPLAY AT(...) USING — same format engine    |

## Operators

| Operator       | Impl | Test | Notes                                      |
|----------------|:----:|:----:|--------------------------------------------|
| `+ - * / ^`    |  ✅  |  ✅  |                                            |
| `=`, `<>`      |  ✅  |  ✅  |                                            |
| `<`, `<=`      |  ✅  |  ✅  |                                            |
| `>`, `>=`      |  ✅  |  ✅  |                                            |
| `AND`, `OR`    |  ✅  |  ✅  |                                            |
| `NOT`          |  ✅  |  ✅  |                                            |
| `&` concat     |  ✅  |  ✅  |                                            |
| `XOR`          |  ✅  |  ✅  | Extended BASIC only                        |
| Operator precedence |  ✅  |  ✅  | NOT > XOR > AND > OR; Test 22 verified |

## String functions

| Function       | Impl | Test | Notes                                      |
|----------------|:----:|:----:|--------------------------------------------|
| ASC            |  ✅  |  ✅  |                                            |
| CHR$           |  ✅  |  ✅  |                                            |
| LEN            |  ✅  |  ✅  |                                            |
| POS            |  ✅  |  ✅  |                                            |
| SEG$           |  ✅  |  ✅  |                                            |
| STR$           |  ✅  |  ✅  |                                            |
| VAL            |  ✅  |  ✅  |                                            |
| RPT$           |  ✅  |  ✅  |                                            |

## Numeric functions

| Function       | Impl | Test | Notes                                      |
|----------------|:----:|:----:|--------------------------------------------|
| ABS            |  ✅  |  ✅  |                                            |
| ATN            |  ✅  |  ✅  |                                            |
| COS            |  ✅  |  ✅  |                                            |
| SIN            |  ✅  |  ✅  |                                            |
| TAN            |  ✅  |  ✅  |                                            |
| EXP            |  ✅  |  ✅  |                                            |
| LOG            |  ✅  |  ✅  |                                            |
| INT            |  ✅  |  ✅  |                                            |
| SGN            |  ✅  |  ✅  |                                            |
| SQR            |  ✅  |  ✅  |                                            |
| RND            |  ✅  |  ✅  | Zero-arg form works without parens         |
| PI             |  ✅  |  ✅  | Zero-arg constant                          |
| MAX            |  ✅  |  ✅  | Extended BASIC                             |
| MIN            |  ✅  |  ✅  | Extended BASIC                             |

## CALL subprograms — graphics

| Subprogram     | Impl | Test | Notes                                      |
|----------------|:----:|:----:|--------------------------------------------|
| CALL CLEAR     |  ✅  |  ✅  |                                            |
| CALL SCREEN    |  ✅  |  ✅  |                                            |
| CALL COLOR     |  ✅  |  ✅  |                                            |
| CALL CHAR      |  ✅  |  ✅  | 8-byte hex pattern                         |
| CALL HCHAR     |  ✅  |  ✅  |                                            |
| CALL VCHAR     |  ✅  |  ✅  |                                            |
| CALL GCHAR     |  ✅  |  ✅  |                                            |
| CALL CHARSET   |  ✅  |  ✅  | Resets chars 32-127 to ROM defaults        |
| CALL CHARPAT   |  ✅  |  ✅  | `CALL CHARPAT(code, A$)` — 16-char hex     |

## CALL subprograms — sprites (Extended BASIC)

| Subprogram     | Impl | Test | Notes                                      |
|----------------|:----:|:----:|--------------------------------------------|
| CALL SPRITE    |  ✅  |  ✅  | Phase 1: create, static draw                |
| CALL MOTION    |  ✅  |  ✅  | Phase 2: 60 Hz integrator, vel/8 px per frame |
| CALL POSITION  |  ✅  |  ✅  | `CALL POSITION(#n,row,col)` — multi-sprite form  |
| CALL LOCATE    |  ✅  |  ✅  | Phase 1: relocate sprite                    |
| CALL COINC     |  ✅  |  ✅  | sprite-pair, sprite-point, ALL — uses footprint (magnify-aware) |
| CALL DISTANCE  |  ✅  |  ✅  | sprite-sprite or sprite-point; sum of dr²+dc²  |
| CALL DELSPRITE |  ✅  |  ✅  | Phase 1: remove sprite(s) / ALL             |
| CALL MAGNIFY   |  ✅  |  ✅  | Phase 1: 1..4 (8×8, 8×8×2, 16×16, 16×16×2)  |
| CALL PATTERN   |  ✅  |  ✅  | Phase 1: change sprite character            |

## CALL subprograms — I/O & system

| Subprogram     | Impl | Test | Notes                                      |
|----------------|:----:|:----:|--------------------------------------------|
| CALL KEY       |  ✅  |  ✅  | Mode 0; other modes treated same           |
| CALL VERSION   |  ✅  |  ✅  | Returns 110                                |
| CALL JOYST     |  ✅  |      | Single-pad: unit 1 = BLE gamepad / arrow keys; unit 2 always centered |
| CALL SOUND     |  ✅  |      | Stub: parses + honors duration (wait vs. immediate); no audio yet |
| CALL SPEED     |  ✅  |  ✅  | Our addition: `CALL SPEED(usPerLine)` — 0=fast (default), 285≈XB, 666≈TI BASIC |
| CALL DELAY     |  ✅  |  ✅  | Our addition: `CALL DELAY(ms)` — block for ms milliseconds (animation pacing) |
| CALL TIMER     |  ✅  |  ✅  | Our addition: `CALL TIMER(var)` — millis() since boot; subtract two readings to time code |
| CALL SAY       |      |      | Speech                                     |
| CALL SPGET     |      |      | Speech                                     |
| CALL ERR       |  ✅  |  ✅  | Stub: returns 0,0,0,lastErrLine (no classification yet) |
| CALL INIT      |      |      | Memory Expansion init                      |
| CALL LINK      |      |      | Assembly linkage                           |
| CALL LOAD      |      |      | Memory poke                                |
| CALL PEEK      |      |      | Memory read                                |

---

## Editor / environment features

| Feature                            | Impl | Test | Notes                                   |
|------------------------------------|:----:|:----:|-----------------------------------------|
| Line editor — DEL / INS / ERASE    |  ✅  |  ✅  | FCTN+1/2/3                              |
| Line editor — arrows (L/R/U/D)     |  ✅  |  ✅  | FCTN+S/D/E/X                            |
| Line editor — BKSP (PC-style)      |  ✅  |  ✅  | 0x7F → cursor-left + delete, BLE & serial |
| REDO recall (FCTN+8)               |  ✅  |  ✅  | Recalls last submitted line             |
| Line-number recall (`<N>` + UP/DN) |  ✅  |  ✅  |                                         |
| UP/DOWN browse in EDIT mode        |  ✅  |  ✅  | Commits current line before navigating  |
| CLEAR breaks running program       |  ✅  |  ✅  | FCTN+4 or Ctrl+C or ESC                 |
| BLE keyboard input                 |  ✅  |  ✅  | F12 or BOOT button = pairing            |
| Serial paste                       |  ✅  |  ✅  | 16 KB decoupled buffer                  |
| Title / menu screen                |  ✅  |  ✅  | Color stripes, 3×3 logo, © char         |
| Error format (blank + msg + BEL)   |  ✅  |  ✅  | TI-style "* MSG IN nn"                  |
| TI-standard token table (0x00-0xFE)|  ✅  |  ✅  | Matches ninerpedia spec; two-token `<=`,`>=`,`<>` |
| `tokenNames[256]` O(1) detokenize  |  ✅  |  ✅  | ASCII bytes + keyword tokens unified    |
| 800×480 new-board port             |  ✅  |  ✅  | ESP32-8048S043C, 32×24 @ 16×16 chars    |
| ~~ OTG version preserved ~~        |  ✅  |  ✅  | Frozen in `ti-basic-otg/`               |
| V9T9 `.dsk` image support          |  ✅  |  ✅  | MOUNT/UNMOUNT/NEWDISK/COPY/OLD/SAVE; FLASH or SDCARD; DIS/VAR + DIS/FIX + PROGRAM; loads real TI archives |
