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
- Patch points in tools/recomp.py (`dialogue_patches`) for all three copies; each copy is
  switched on separately (`vwf_on[]`), currently only ADV.
- Still to do: TALK (demon conversations; files grow up to 40 %, the PC buffer size is not
  checked yet, and they may be drawn by the battle text system rather than these
  interpreters), dungeon packs D00-D04 (54 boxes do not fit in place; the PS1 patch redirects
  them to a spill area by matching a 24-byte fingerprint at box start), menus / items / skills
  (small font, strings inside Persona.exe), battle text (BTLP), movie subtitles.
