# Persona 1 PC Recomp

Static recompilation of the 1999 Japanese Windows port of *Megami Ibunroku Persona*
(ASCII, `Persona.exe`) into portable C, running on SDL3 instead of DirectX 6 and DirectShow.

https://github.com/Dagxs/Persona-1-PC-Recomp

Inspired by [persona-digital-collection-recomp](https://github.com/ACR-GD/persona-digital-collection-recomp),
which does the same for the Persona Digital Collection disc; the recompiler and runtime here are
written from scratch.

**No game code or data is included.** The C code is generated from your own copy of the
Japanese Windows release (the disc with `acc\persona\Program\Persona.exe`), and the game data is
read from your disc.

## Build
Needs Python 3 with `pefile` and `capstone`, clang, CMake, Ninja and SDL3
(on Windows: MSYS2 `mingw-w64-clang-x86_64-sdl3`).

    python -I tools/extract_disc.py "path/to/Persona.cue" data/cd     # disc files + data/Persona.exe
    python -I tools/recomp.py data/Persona.exe gen                    # x86 -> C
    cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang
    cmake --build build
    play.bat                                                          # or ./build/persona1.exe

`extract_disc.py` accepts a `.cue`, a raw `.bin` or an `.iso`.

## What works
- The whole game boots and plays from the intro movies through the title screen into the story.
- Graphics: DirectDraw and Direct3D 5 (`IDirect3D2`, HAL device) on a software rasterizer with
  exact edge rules, z-buffer, perspective-correct textures, colour keys and blend modes.
- Sound: every BGM and sound effect (MP3 files played through DirectShow) via minimp3.
- Movies: the MPEG-1 `.STR` movies (DirectShow multimedia streams) via pl_mpeg, with sound.
- Keyboard and SDL gamepads, a launcher and an in-game options menu.

## Launcher and options
A settings screen opens before the game (Play / Quit), and the same menu opens in game with **F10**
or the controller's Home/Guide button (the game pauses). Settings: fullscreen or window, window size
1x-4x, internal resolution 1x-4x, fill-screen or pixel-perfect integer scaling, sharp or smooth
filtering, VSync, controller layout (Western / Japanese), master volume, language (English once
installed) and whether to show the launcher. They are saved in `save/settings.ini`.

## Controls
Keyboard (the game's own defaults): numpad 8/2/4/6 move, S confirm, Z cancel, W / A the other face
buttons, numpad 7/1/9/3 shoulder buttons, numpad 0 / numpad . select / start. Arrows, Enter
(confirm) and Esc (cancel) also work. Alt+Enter toggles fullscreen.

Gamepad (any SDL-supported controller): D-pad or left stick moves; bottom face button confirms,
right face button cancels, top = triangle, left = square, bumpers/triggers = L1/R1/L2/R2,
Back/Start = select/start. The Japanese layout (right button confirms) is in the options menu.

## English translation (in progress)
The English text comes from the PS1 fan translation
[ayla6/megami-ibunroku-persona-translation](https://github.com/ayla6/megami-ibunroku-persona-translation)
(MIT), a port of the PSP script. The Windows port keeps the PS1 data formats, so its text tools
apply to the Windows files directly:

    git clone https://github.com/ayla6/megami-ibunroku-persona-translation _tl
    python tools/install_english.py _tl data/cd data/en

This builds `data/en/` (the story scripts `ADV/E0-E3.BIN` rebuilt with English from your disc's
own files, plus the translation's variable-width font). Then pick **Language: English** in the
launcher. The story dialogue (all four script files, about 7,300 text boxes) is drawn by a
variable-width text engine (`runtime/vwf.c`), the same design as the PS1 translation's.
Menus, items, battles, dungeon text, demon talk and movie subtitles are still Japanese.

## Status
- [x] Function discovery without relocations (~26k functions, 83% of .text)
- [x] x86 -> C emitter (integer, x87, string ops, switch tables) with patch points
- [x] Win32 runtime: kernel32, guest threads, files with a virtual CD drive, user32, registry
- [x] DirectDraw + Direct3D software renderer on SDL3
- [x] DirectShow: MP3 music/effects, MPEG-1 movies
- [x] Gamepads, launcher, in-game options
- [ ] Internal resolution above 1x: implemented, quality and speed not yet checked
- [x] English story dialogue (variable-width font)
- [ ] English: faster printing (two letters per frame), menus, battles, names, dungeons, subtitles
- [ ] Play-testing beyond the opening

## Debugging
Environment variables: `P1_TRACE=1` logs every Win32 and COM call; `P1_WATCHDOG=secs` reports
where each guest thread is; `P1_HEADLESS=1` runs without a window; `P1_KEYS="frame:vk:down,..."`
scripts key presses; `P1_SHOT=dir` dumps frames; `P1_FRAMES=n` exits after n frames;
`P1_CD` / `P1_SAVE` / `P1_EXE` override paths. Configuring with `-DCMAKE_C_FLAGS=-DP1_WATCHREAD`
adds `P1_WATCH_LO` / `P1_WATCH_LEN`, which report the guest functions that read a memory range.

## Credits and licences
This project's code is under the MIT licence (`LICENSE`). It covers the recompiler and runtime
only: *Persona* and its data belong to their rights holders, and you need your own copy of the game.
- minimp3 (CC0) and pl_mpeg (MIT) in `third_party/`.
- English text and font: the PS1 translation above (MIT); its licence and contributors are copied
  into `data/en/` by the installer.
- The PS1 decompilation [daanhenke/persona-psx](https://github.com/daanhenke/persona-psx) (GPLv3)
  is used only as a reference for understanding the original code; none of it is copied here.
