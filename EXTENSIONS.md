# ESP-XB — Extended Basic for ESP32

*aka EB:ESP32 if you prefer a cartridge label.*

## Extensions Reference

This document covers everything our simulator adds **on top of** stock
TI Extended BASIC. For the core language (statements, functions, CALL
subprograms that exist on a real TI), see the official *TI Extended
BASIC Reference Manual*.

If you came here to write a magazine type-in from 1985, you don't need
this file — that program targets stock XB, which we implement
faithfully. Read this when you want to drive the simulator's hardware
features (disk images, FLASH storage, timing) from BASIC.

## Device prefixes

Anywhere a file specification can appear (OPEN, OLD, SAVE, MERGE,
DELETE, COPY, MOUNT, NEWDISK, CAT), prefix the name with one of:

| Prefix      | Storage                                                |
|-------------|--------------------------------------------------------|
| `FLASH.`    | Internal LittleFS (~1 MB on the ESP32-S3)              |
| `SDCARD.`   | Raw files on the SD card                               |
| `DSK1..DSK9`, `DSKA..DSKZ` | A V9T9 `.dsk` image previously MOUNTed |

Default for SAVE/OLD/MERGE/DELETE if no prefix: **FLASH** (back-compat
with `.bas` files saved before the prefix system existed).

```
SAVE FLASH.MYGAME       saves /MYGAME.bas to LittleFS
SAVE SDCARD.MYGAME      saves /MYGAME.bas to SD card
SAVE DSK1.MYGAME        saves as TI PROGRAM format inside mounted DSK1
SAVE MYGAME             same as SAVE FLASH.MYGAME
```

For OPEN you must quote the spec because dots aren't TI-tokenizer-
friendly inside expressions:

```
OPEN #1:"FLASH.SCORES",OUTPUT
OPEN #2:"DSK1.HISCORES",INPUT
```

## Disk management commands

All are *immediate* commands (typed at the prompt or as program
statements with no `::` chain after).

### MOUNT — attach a `.dsk` image to a virtual drive

```
MOUNT DSKn <spec>
```

Where `n` is `1..9` or `A..Z` (35 drives total) and `<spec>` is one of:

```
MOUNT DSK1 SDCARD.GAMES.DSK     SD-card image
MOUNT DSK1 FLASH.SYSTEM.DSK     LittleFS-resident image (read-only)
MOUNT DSK2 GAMES                bare name → SDCARD./GAMES.DSK
MOUNT DSK1 /GAMES.DSK           absolute SD path
```

Bare `MOUNT` (no args) lists every mounted drive.

The mount table is persisted to `/mounts.cfg` on LittleFS, so mounts
survive reboots automatically.

> ⚠ Writing to a FLASH-backed `.dsk` image works but tears the RGB
> display during the flash erase (interrupt-mask hardware behavior).
> SDCARD-backed images are clean. Use FLASH for read-only collections.

### UNMOUNT

```
UNMOUNT DSKn
```

Releases the drive slot.

### NEWDISK — create a blank V9T9 image

```
NEWDISK <spec> "VOLNAME" [SSSD|DSSD|DSDD]
```

Default size SSSD (90 KB / 360 sectors). DSSD = 180 KB, DSDD = 360 KB.

Prompts to confirm overwrite if the target exists.

```
NEWDISK SDCARD.WORK.DSK "WORK"
NEWDISK FLASH.SYS.DSK "SYS" DSSD
```

### COPY

```
COPY <src-spec> <dst-spec>
```

Line-level copy between any two device prefixes. Reads source as
DIS/VAR records and writes the same to destination.

```
COPY DSK1.HELLO FLASH.HELLO
COPY FLASH.NOTES SDCARD.NOTES
COPY DSK1.HELLO DSK2.HELLO
```

### CAT / CATALOG / DIR — list files

```
CAT [device]
CATALOG [device]   (alias)
DIR [device]       (PC-keyboard habit alias)
```

`CAT` with no arg lists FLASH. With `DSK1..Z`, shows the TI Disk
Manager style catalog (filename, type, size, P-flag for protected).
Pages every 23 lines — press any key to continue, ESC/Ctrl+C/CLEAR
to cancel.

### DELETE

```
DELETE FLASH.NAME
DELETE SDCARD.NAME
DELETE DSK1.NAME
DELETE NAME            (legacy: removes /NAME.bas from FLASH)
```

For DSKn, must be quoted in tokenized program form:

```
100 DELETE "DSK1.SCORES"
```

## CALL extensions (our additions, not on real TI)

### CALL SPEED(usPerLine)

Sets a per-statement throttle in microseconds.

```
CALL SPEED(0)        unthrottled — native ESP32 speed (~30× real TI)
CALL SPEED(285)      ~TI Extended BASIC native speed
CALL SPEED(666)      ~stock TI BASIC native speed
CALL SPEED(2500)     about 1/10 of XB speed (game-friendly slow-mo)
```

The throttle fires on every `::` separator and once at end of line.
Set to 0 inside the prompt or in your program to return to native
speed for editing.

State is session-wide; reset on reboot.

### CALL DELAY(ms)

Block for `ms` milliseconds. Yields to BLE / display / sprite ticker
during the wait.

```
CALL DELAY(500)        half-second pause
```

Use for animation pacing where `CALL SPEED` throttling would be too
coarse or unwanted.

### CALL TIMER(numeric-var)

Writes `millis()` since boot into a numeric variable. Subtract two
readings to time a code block:

```
100 CALL TIMER(T0)
110 FOR I=1 TO 1000 :: NEXT I
120 CALL TIMER(T1)
130 PRINT "1000 ITERATIONS:";T1-T0;"MS"
```

## Editor / shell improvements

### `!` tail comment

Anywhere on a line, `!` makes the rest of the line a comment (TI XB
feature). Statements *before* the `!` still run; everything after it
— text, `::`-joined statements, all of it — is dropped.

```
10 PRINT "HI" :: PRINT "TWO" ! and the rest is a comment :: PRINT "GONE"
```

prints `HI` then `TWO`. The `! and the rest …` plus the trailing
`PRINT "GONE"` are skipped because they're inside the tail comment.

To suppress later statements on the same line, put the `!` immediately
after the keep-running prefix:

```
10 PRINT "HI" ! :: PRINT "DROPPED"
```

prints only `HI`.

### Backspace in the editor

PC-style: deletes the character before the cursor and moves the
cursor left. Works on both the BLE keyboard (HID 0x2A → 0x7F) and
serial paste (`0x7F`).

FCTN+S for "cursor left without delete" still works — same TI
semantics as the original.

### CAT / DIR pagination

Long catalogs page at 23 lines with `-- PRESS ANY KEY (ESC TO STOP) --`.
ESC, Ctrl+C, or the CLEAR key (FCTN+4) abort the listing.

## Hardware notes

### CALL SPEED scope and meaning

Throttling targets the *statement* level. A single line with many
`::`-joined statements throttles per statement. A line that is one
big statement (e.g. a multi-arg CALL) throttles once.

Typical throttle values from measurement (your mileage will vary):

| Goal              | usPerLine     |
|-------------------|---------------|
| Full speed        | 0             |
| TI XB pace        | ~285          |
| TI BASIC pace     | ~666          |
| "Slow enough to watch" | 2500–10000 |

### FLASH-disk vs SDCARD-disk write performance

- **SDCARD.dsk** writes go over the SPI SD driver. Writing a few KB
  is fast (tens of ms) and clean.
- **FLASH.dsk** writes go through the internal flash controller. The
  ESP32-S3 masks interrupts during flash-erase, which starves the RGB
  panel's bounce buffer and tears the display for the duration of
  the write. For NEWDISK on FLASH (~90 KB write) the tear lasts a
  full second.

Use FLASH for shipped read-only collections, SDCARD for working /
writable disks.

### Sprite layer

We render up to 28 sprites in pixel-accurate software, layered over
the 32×24 character grid:

- 8×8 (MAGNIFY 1) or 16×16 (MAGNIFY 3) source patterns
- 2× scaled (MAGNIFY 2 and 4)
- 60 Hz motion integrator runs from both `loop()` and the run-loop's
  per-line tick, so sprites move during program execution AND at the
  prompt
- COINC bounding-box collision uses the actual on-screen footprint
  (8/16/32 px) — works correctly across mixed magnify levels

### File handles

`OPEN #n` supports unit numbers `1..10`. Units `11..12` are reserved
for internal use by `COPY`. Adjustable via `MAX_FILES` in `file_io.h`
(each slot costs ~700 bytes of BSS for its File object, DIS/VAR
reader, and writer).

## Token storage

The interpreter stores BASIC programs in TI-binary-compatible token
format:

- Token values match ninerpedia / Thierry's spec byte-for-byte
- `TOK_LINENUM` (0xC9 + big-endian word) used for line refs after
  GOTO/GOSUB/THEN/ELSE/RESTORE/BREAK/UNBREAK/RESEQUENCE/ON ERROR
- TI PROGRAM-format `.dsk` files round-trip end-to-end, verified
  against 1985-era community shareware (DSKUTILS LOAD, etc.)

This means programs SAVE'd from our simulator into a `.dsk` image
should load (and run) on Classic99 / V9T9 / MAME with no conversion,
provided the program doesn't use any of the simulator's extensions
above.

## What's *not* implemented (yet)

- **CALL SOUND** — parses correctly and honors timing (positive
  duration waits for the previous sound, negative cancels it), but
  produces no actual audio. Hardware audio path not wired.
- **CALL JOYST** — not present (no joystick hardware to read from)
- **CALL SAY / CALL SPGET** — speech synthesizer, no module
- **CALL LINK / CALL LOAD / CALL PEEK / CALL INIT** — assembly-language
  bridge to memory-expansion, not implemented (would require an
  emulated TMS9900 to be useful)
- **`CON` after `END`** — TI-faithful: CONTINUE works after STOP /
  breakpoint / user BREAK but is refused after END or NEW.

## Known limitations

These features are partially implemented — they parse and run, but
behave differently from a real TI-99/4A in ways that matter for file
interchange or absolute fidelity.

- **INTERNAL files are stored as DISPLAY.** `OPEN ..., INTERNAL` is
  accepted by the parser, but the I/O path writes plain ASCII text
  the same way DISPLAY files do — there is no TI radix-100
  floating-point encoding. Consequences:
  - Round-trip *within ESP-XB* works fine: write INTERNAL on this
    box, read it back on this box, you get the same data.
  - **Files do not interop with a real TI-99 as INTERNAL.** A
    DISPLAY-style ESP-XB file moved to a real TI must be opened as
    DISPLAY there, and an INTERNAL file written by a real TI cannot
    be read here.
  - Numeric precision is whatever DISPLAY's print formatter produces
    (~6-7 significant digits), not the full 13-digit radix-100
    precision real INTERNAL would preserve.
  - Adding real radix-100 encode/decode is on the future-work list
    but isn't trivial — it's a few hundred lines of bit-twiddling
    and BCD-style math.

For the latest authoritative list of statuses, see
[KEYWORDS.md](KEYWORDS.md).
