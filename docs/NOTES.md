# Reverse-engineering notes (Persona.exe, 1999 Japanese Windows port)

Addresses are virtual addresses in `Persona.exe` (image base 0x400000). The Windows port is the
PS1 game's C code compiled for x86, so the PS1 decompilation (daanhenke/persona-psx) and the PS1
translation's docs (third_party/persona-translation/docs) usually describe the same logic.

## Memory
- **Emulated PS1 main RAM**: a 2 MB array at `0x664ff0`. PS1 address `0x80xxxxxx` maps to
  `0x664ff0 + (addr & 0x1fffff)`. Examples: FONT.BIN (PS1 `0x801E0000`) at `0x844ff0`; a file the
  PS1 loads to `0x80180000` lands at `0x7e4ff0`.
- Guest memory in the runtime is a flat 512 MB array; see runtime/rt.h for the layout.

## Startup
- WinMain `0x540210`: FindWindow("ClassPersona"), CoInitialize, settings (`0x532df0`, registry
  `HKLM\SOFTWARE\ASCII Corp.\Persona`, values Key_1..14, GUID, AutoSelect, SEVol, BGMVol,
  SEVolRange, Version), DevConfig (`0x532fe0`, a 216-byte blob; missing => exit code 1).
- Startup errors go through `0x53fe30(code)`: "起動に失敗しました (エラーコード %d)".
  Code 12 = SetDisplayMode(640x480x16) failed.
- The CD is found by scanning drive letters with GetDriveTypeA/GetVolumeInformationA (label
  "Persona"); the runtime presents it as drive Q:.

## Graphics
- DirectDraw: primary + 1 back buffer (caps 0x2218), clipper, z-buffer (caps 0x20800), D3D via
  IDirect3D2 + HAL device, viewport, background material. Main loop renders with DrawPrimitive
  (D3DVT_TLVERTEX, pre-transformed), textures are 256x256 pages the game rewrites through a
  pointer it got from a single Lock (PS1 VRAM emulation).
- The game never Locks the back buffer, so a host-side high-resolution copy of it is safe
  (runtime/ddraw.c `hi`).
- Frame pacing `0x5433a0`: frame budget 1000/[0x5dab88] ms; if late, the frame is not drawn.

## Input
- `0x536650` polls GetAsyncKeyState for two tables of {PS1 pad bit, VK}: `0x5da4d0`
  (configurable Key_1..14) and `0x5da540` (fixed: arrows = D-pad, Enter = circle 0x10,
  Esc = cross 0x20, F2 = triangle 0x100, F1 = square 0x40, Home/End/PgUp/PgDn = L1 0x400 /
  L2 0x80 / R1 0x800 / R2 0x200, Insert 0x1000, Delete 0x2000). Joystick via joyGetPosEx.

## Sound and movies
- All BGM and effects are MP3 files (`SND\*.SEQ`) played through DirectShow filter graphs
  (`0x534300`..). A thread per looping BGM (`0x5348b0`) waits in WaitForCompletion.
- A background loader thread (`0x533d00`) reads files on request: jobs at `0x640c74` (count),
  event `0x640c80`, busy flag `0x644870` (-1 = idle).
- Movies (`STR*\*.STR`, MPEG-1 system streams, 320x240) play through IAMMultiMediaStream /
  IDirectDrawMediaStream (`0x53f8xx`), one sample surface blitted to the back buffer.

## Text
- Three copies of the PS1 `MsgStep` text interpreter, each with its own glyph routine, cell
  activation routine and VRAM upload queue (12-byte entries {src, x, y, w=4, h=16}):

  | copy | interpreter | glyph | activate | FONT ptr | queue | count |
  |---|---|---|---|---|---|---|
  | ADV (story, E0-E3.BIN) | `0x479350` | `0x479670` | `0x479610` | `0x618f80` | `0x61ecb8` | `0x61ec28` |
  | B | `0x420a30` | `0x421310` | `0x4212b0` | `0x5e0e8c` | `0x5e4f18` | `0x5e5478` |
  | C | `0x4e1870` | `0x4e2150` | `0x4e20f0` | `0x62985c` | `0x62a278` | `0x62a248` |

  B and C are the dungeon and overworld copies (which is which is not confirmed yet).
- Interpreter state struct (esi): +0x0 flags (0x200000 = name read backwards), +0x6 delay,
  +0x8 speed, +0xA cell position (line*15 + column), +0xC / +0x10 read pointers,
  +0x35 remaining name characters. Read pointer in edi while running.
- Text box: 15x4 cells of 16x16 4bpp at VRAM x = 0x3C0 + ((P+1)&15)*4, y = 0x100 + ((P+1)&0xF0).
  Glyph colours: ink 2, shadow 1.
- Sector tables of the story scripts: E0 `0x57946c`, E1 `0x579630`, E2 `0x5797f4`,
  E3 `0x5799d8` (u16 per entry, zero-terminated, same as the file's own sector 0).

## English (runtime/vwf.c, runtime/english.c, runtime/enpatch.c)
- Glyph codes 88-E6 = English (ASCII + 0x68); 80-87 = two-byte original glyphs.
- Patch points in tools/recomp.py (`dialogue_patches`) for all three copies (ADV, B = dungeon,
  C = 2D town: the PS1 overlays ADV, DNG, S2D); each is switched on by `vwf_on[]` (all three
  with English: they read the same story files).
- PS1 data tables: Persona.exe holds the PS1 executable's data in the same order and spacing,
  PC address = PS1 address - 0x8004442C + 0x5ce258 (items 0x5ce230 = record 0, nicknames,
  skills 0x5d1b84, Personas, demons, descriptions...). tools/install_english.py rebuilds a
  stand-in SLPS_005.00 from it for the translation's tools.
- Small font (names, menus, battle panels): one byte per glyph, 8x12 cells; English through the
  translation's Latin cells (a-z over rare hiragana) in every copy of the page (2D/, D0?/,
  ADV/ADVCMD*.BIN, OPEN.BIN, B/BF.BIN compressed). Names from names.bin (english.c).
- Button prompts: glyphs 0xEA/0xE3/0xEE/0xEC (circle/cross/triangle/square, two-byte codes) are
  drawn as the button of the device in use (host_button_label).
- TALK (demon conversations): `0x5257e0`/`0x525810` load `TALK\<name>.BIN` (name table at
  `0x5ca7d8`, 24 bytes per entry) to PS1 RAM `0x801C0000` (host `0x824ff0`) through the loader
  thread; FONT.BIN follows at `0x801E0000`, so a TALK file may be up to 128 KB (the largest
  English one is 62 KB). The conversation text is drawn in battle, by the battle text system
  (the PS1 translation's `vwf_btlp*`), not by the three MsgStep copies: that renderer still has
  to be found on the PC (trace FONT.BIN reads during a battle with the P1_WATCHREAD build).
- Dungeon field script (PS1 `FieldRunScript`, decomp src/dng/field): the text inside the dungeon
  packs `D0?/D??.BIN` (locked doors, party remarks...). Not a MsgStep copy. On the PC: loop read
  `mov al,[esi]` at `0x42cdd3` (esi = read pointer), glyph path `test al,0x80` at `0x42d8a2`
  (one byte -> `0x42d8a6`, two bytes -> `0x42d8c5`), FieldMsgPutGlyph `0x42da00` (ecx = code;
  col `0x5e46e8`, line `0x5e46ec`, colour `0x5e474c`, upload rect `0x5e47b0`), FieldMsgClear
  `0x42daa0`, FieldMsgNewLine `0x42db00`, FieldMsgTint `0x42dc80` (ecx cells, dl colour, stack
  col/line/count, ret 0xc), FieldMsgPrintBytes `0x42dcd0`, FieldMsgPrint `0x42dd00`,
  FieldMsgPrintCodes `0x42dda0`, FieldDecodeGlyph `0x428a70` (font pointer `0x5e110c`, glyph
  image at `[0x5e107c]+0x6da5c`, ink 1, shadow 2 one pixel right and down), LoadImage
  `0x54c380` (ecx rect, edx source), FieldFrame `0x422100`. English: runtime/vwf_field.c.
- Battle text (PS1 BTLP: battle messages, Triangle help, the TALK conversations, story-battle
  lines): interpreter step `0x52fd60(ecx = text struct)` (reads the pointer at struct+8, `0x52fd6d`),
  character step `0x52f850`, glyph function `0x52fbb0(ecx struct, edx ptr) -> ptr` (PS1
  0x8007C754), code read `0x52f370`, FONT.BIN glyph decode `0x521430(code, dst, font)` (ink 1,
  shadow 2), upload queue `0x5212d0(ecx src, dx x, stack y, w, h)` (72 entries at `0x637f18`,
  count `0x638454`, flushed by `0x5213e7` after the queued clears). Text struct as on the PS1:
  +0x10 sprite attribute, +0x1C cell, +0x20 sprite count, +0x28 sprites {x, y, 0, 0, attr},
  +0x26C/+0x26E texture origin; the sprite draw takes the texture slot from the sprite index.
  Structs: message box `0x63f0b0` (opened by `0x5302e0(ecx text)`, through `0x531bf0`),
  dialogue `0x638f40` (`0x52fd00`). Name / number inserts are copied to `0x6391e0..0x639288`
  (`0x52e5d0`; names through `0x52e6c0`: bytes >= 0x80 get an 0x80 lead). 16 px string
  renderer (command windows) `0x52f2c0(ecx descriptor, edx string)` (PS1 0x8007BF44).
  English: runtime/vwf_btl.c. The BTLP strings are in Persona.exe's data, but not in one block
  (own alignment, identical strings stored once); tools/btl_map.py matches the translation's rows
  to them by slot sizes. Descriptions: table of PC pointers at `0x5d48a8` (PS1 0x8004AA7C), read by
  `0x41b885`, `0x473985`, `0x4dc705` (the three MsgStep copies) and `0x4fcd81` (battle help).
- Menu names (runtime/vwf_menu.c, data/en/sf_names.bin): the menus write names into background
  maps (40-cell grids of u16 cell indices, 8x12 cells) with TileMapWriteRow `0x421490(ecx src,
  edx dst, base, count)` - one copy shared by every menu - and libgs' GsSortFastBg `0x5496d0(ecx
  GsBG, edx GsOT, otz)` turns each cell into a 0x2c-byte textured quad (code 0xF0; vertices at
  +8/+12/+1C/+24, u/v at +C/+16/+20/+28, clut +10, tpage +1A; work pointer `0xa65288`, draw offset
  `0xa65330`/`0xa65334`; GsMAP: cellw, cellh, ncellw, ncellh, cells, index). A row whose source is
  a name (by the head's address in the tables, else by its bytes) is remembered; when its map is
  drawn and the cells are unchanged, they are hidden for that draw and the whole name is drawn as
  quads cut to each glyph's ink, squeezed to the field when wider (down to 0.70 for the longest).
  Seen on the Equip screen. The dialogue inserts (MsgStep item / skill / demon / Persona cases, all
  ending at `0x47941a` / `0x4210c3` / `0x4e1f03` with edi = the record's head and left = 10) read
  the whole name from a scratch buffer (written, not yet seen in game). Record layouts the inserts
  use: item names 0x5ce238 + 32*id, skills 0x5d1b70 + 20*id, demons 0x5cb5cc + 56*id, Personas
  0x5d4f00 + 44*id (the name is 8 bytes into the Persona record: personas.NAME).
- Still cut at 10 glyphs: names in battle (BTLP panels and the insert buffers `0x6391e0..`, 24 bytes
  each), the Persona status bar and Persona Data bar (sprite rows, not maps), and any menu that
  copies map cells instead of writing them. The debug menus' strings (BTLP 0x800DD000..) and the
  battle rows btl_map.py cannot find stay Japanese.
- Overlay messages (runtime/msg_text.c, data/en/msg_text.bin): ADV/DNG/S2D keep some messages in
  their own data, compiled into Persona.exe; matched from the first release's overlays and read in
  English by the interpreters' per-glyph read (`0x478f42` / `0x420c05` / `0x4e1a45`). MsgOpen of
  the ADV copy is `0x478b50(ecx text)`. Menu text the translation leaves Japanese is in
  tools/menu_text.tsv (sort prompt, spell-card and config prompts, the SORT list's swap label);
  the Equip screen's stat labels are art in the font page (menu_gfx.label_rows, now also applied
  to ADVCMD). Dialogue choices (<FF0E>): the table of 32 {u32 pointer, rows, cols, width, 0} sits 4
  bytes after 0x8004D760 + DELTA (`0x5d7590`); the option strings are laid out a little differently
  from the PS1 (same order), so the installer pairs them by order and sends them through
  msg_text.bin with the translation's layout (choices.py); the layout bytes go in names.bin. Seen
  in the opening (Bet on Mark / Bet on Brown). The config screen's option words (はやい/おそい/なし, する/しない) and the
  heal-mode labels are still Japanese (their layout is by fixed cells).
- Tests: P1_TURBO=n runs the game clock n ms per call and ends movies at once, so P1_KEYS scripts
  land on the same frames each run (new game: Enter taps to the name screen, then Right, Enter,
  F2, Enter for each field, Insert, Enter, Up, Enter; F1 opens the field menu). P1_SFLOG logs the
  menu name rows; P1_SFSWAP=k draws name k in their place.
  Done and seen: story dialogue, movie subtitles, the opening quote, button prompts, the ending
  poems (runtime/poems.c; checked on the decoded movies with P1_POEMTEST).
  Written but not yet seen in game: dungeon field-script boxes and long dungeon map titles
  (vwf_field.c; MapDrawName 0x4079e0, map_titles.bin), the dungeon / town dialogue copies, item /
  skill / Persona / demon names and the small-font Latin cells, race names and tags, AUTO modes,
  descriptions, the ending pages and the staff roll's logo sheet (EXE/END.BIN; crop table at
  0x5d81f8), the whole battle text system (vwf_btl.c, TALK files, btl_text.bin). P1_BTLVWF=0 turns
  the battle engine off, but the English TALK files and descriptions then show as wrong glyphs.
- Developer data: the battle strings, race tables and AUTO modes are matched with the first PS1
  release's overlays (BTLP.BIN, ADV.BIN, DNG.BIN, S2D.BIN in the translation repo's data/psx, from
  the developer's own disc; git-ignored, not shipped). Without them install_english.py falls back
  to slot-size matching for the battle strings and skips the tables.
