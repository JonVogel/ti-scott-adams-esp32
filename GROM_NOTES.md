# TI Extended BASIC 2.5 — GROM / ROM Reference

Notes on the XB cartridge dump stored in this directory, extracted as a
reference for feature parity work on the simulator. Not used by the
firmware at runtime.

## Files

| File | Size | Contents |
|------|-----:|----------|
| `xb25C.BIN` | 8 KB | CPU ROM bank C — loads at `>6000-7FFF`, banked |
| `xb25D.BIN` | 8 KB | CPU ROM bank D — loads at `>6000-7FFF`, banked |
| `xb25G.BIN` | 40 KB | Five GROM chips mapped at `>6000`, `>8000`, `>A000`, `>C000`, `>E000` |

First 0x1000 bytes of the two CPU ROM banks are identical; they diverge
from `>1000` onward. Standard banked XB layout.

The GROM file is a straight concatenation of the five 8 KB chips:

| File offset | GROM address | Chip | What's there |
|-------------|--------------|------|--------------|
| `0x0000`    | `>6000`      | G3 | Master header, program-list entry "EXTENDED BASIC V2.5" |
| `0x2000`    | `>8000`      | G4 | XB 2.5 extension code + font data (LRGCPS / CHARA1) |
| `0x4000`    | `>A000`      | G5 | Core CALL subprogram name table + code |
| `0x6000`    | `>C000`      | G6 | Memory-expansion sub name table (LINK/LOAD/INIT/PEEK/CHARPAT) |
| `0x8000`    | `>E000`      | G7 | System subs, XXB re-entry, CAT, CLSALL |

The G3 header at offset 0 is the only one with marker `>AA`:

```
marker=0xAA ver=0x02 count=1
powerup = 0x0000
proglist= 0x6A01   -> "EXTENDED BASIC  V2.5"
dsrlist = 0x9830
sublist = 0xA026   -> chain of CALL subprograms
```

## Version

Title string at GROM offset `0x0A06`:

```
EXTENDED BASIC  V2.5
```

This is *not* stock TI Extended BASIC (which reports 110 = v1.10 via
`CALL VERSION`). XB 2.5 is a community/enhanced build with additional
subprograms grafted onto the standard XB base. Real hardware running
stock XB will *not* have the XB 2.5 extras listed below.

## CALL subprogram table

Subprogram name list format (backward-chained):

```
  word : link to next entry  (0x0000 terminates)
  byte : name length
  N    : ASCII name
  word : code entry point (GPL address)
```

Head pointer is in the G3 header at bytes 10-11 (`>A026`). Entries
spread across G5, G6, G7, G4 as the chain hops.

### Graphics / I/O (core — mostly implemented)

| Sub | Entry | Our status |
|-----|-------|------------|
| `SOUND`    | `>AA1E` | not implemented |
| `CLEAR`    | `>A9F8` | done |
| `COLOR`    | `>A91D` | done |
| `GCHAR`    | `>A8FA` | done |
| `HCHAR`    | `>AAE3` | done |
| `VCHAR`    | `>AB01` | done |
| `CHAR`     | `>AB1A` | done |
| `KEY`      | `>ABCE` | done |
| `JOYST`    | `>AC13` | not implemented |
| `SCREEN`   | `>AC66` | done |
| `VERSION`  | `>A9FF` | done (returns 110) |
| `ERR`      | `>AC96` | not implemented (needs ON ERROR) |
| `CHARSET`  | `>B029` | done |

### Sprites (none implemented)

| Sub | Entry |
|-----|-------|
| `SPRITE`    | `>AE8F` |
| `DELSPRITE` | `>AEC0` |
| `POSITION`  | `>AEFF` |
| `COINC`     | `>AF40` |
| `MAGNIFY`   | `>AF75` |
| `MOTION`    | `>AF8D` |
| `LOCATE`    | `>AF9D` |
| `PATTERN`   | `>AFB1` |
| `DISTANCE`  | `>AFC1` |

### Speech

| Sub | Entry |
|-----|-------|
| `SAY`   | `>B108` |
| `SPGET` | `>B261` |

### Memory expansion (G6)

| Sub | Entry |
|-----|-------|
| `LINK`    | `>C325` |
| `LOAD`    | `>C040` |
| `INIT`    | `>C2BA` |
| `PEEK`    | `>C2CA` |
| `CHARPAT` | `>C434` (done) |

### System / utility (G7)

| Sub | Entry | Notes |
|-----|-------|-------|
| `GSAVE`     | `>E0FE` | Save VDP pattern table |
| `GLOAD`     | `>E104` | Load VDP pattern table |
| `VPEEK`     | `>D3D9` | Read VDP RAM |
| `GPEEK`     | `>D3DE` | Read GROM |
| `VPOKE`     | `>D432` | Write VDP RAM |
| `ALLSET`    | `>D105` | |
| `WAIT`      | `>D11D` | |
| `MOVE`      | `>D161` | Memory block copy |
| `MLOAD`     | `>D20E` | |
| `MSAVE`     | `>D208` | |
| `BYE`       | `>D36B` | Return to title |
| `NEW`       | `>D36F` | |
| `RESTORE`   | `>D374` | |
| `QUITON`    | `>D3C4` | Enable FCTN+= quit |
| `QUITOF`    | `>D3BD` | Disable quit |
| `SPRON`     | `>D3CB` | Re-enable sprite magic |
| `SPROF`     | `>D3D2` | |
| `SCREENON`  | `>D522` | |
| `SCREENOF`  | `>D51B` | |
| `XXB`       | `>E027` | XB self-entry |
| `CAT`       | `>DC00` | Catalog disk |
| `CLSALL`    | `>E050` | Close all files |

### XB 2.5 extensions (not in real XB — SKIP for parity)

Flagged so we don't waste time chasing these while targeting stock-XB
compatibility.

- Predefined color schemes: `COLR13`, `COLR15`, `COLR17`, `COLR18`,
  `COLR19`, `COLR1A`, `COLR1B`, `COLR1F`,
  `COLRF1`, `COLRF2`, `COLRF4`, `COLRF6`, `COLRFC`
- Predefined sounds: `BEEP`, `HONK`, `CRASH`, `CHIME`, `NYANYA`, `QUIT`
- Font switchers: `LRGCPS` (large caps), `CHARA1` (standard A1 font)
- Meta: `HELP`, `XB`

### Disk format subs

| Sub | Entry |
|-----|-------|
| `SSSD` | `>77D4` |
| `DSSD` | `>77E0` |
| `DSDD` | `>77EC` |

## Font data

GROM 4 at file offset `~0x3B00` onward holds 8-byte bitmap patterns
for the `LRGCPS` and `CHARA1` character sets. Each glyph is 8 bytes
of row-major pixel data, same format our `CALL CHAR` accepts. If we
ever want pixel-accurate TI fonts, the source is right here — extract
64 × 8 bytes starting at the right offset for the desired set.

## How this was decoded

1. Read G3 header at GROM file offset 0 (`marker=0xAA`).
2. Head of subprogram chain = header bytes 10-11 = `>A026`.
3. Walk chain: `word link | byte len | N×name | word code`, terminate
   on `link == 0x0000`. Chain crosses chip boundaries naturally
   because GROM is a flat 40 KB address space from the CPU side.
4. Map GROM addresses back to file offsets with `offset = addr - 0x6000`.

## What we can't do with this

GPL (the GROM bytecode) is a stack-oriented VM specific to the TMS9900
GROM controller. The code at each entry point is GPL, not TMS9900
assembly and not anything our interpreter can execute directly. The
name table is useful; the bodies are reference-only without a GPL
emulator.
