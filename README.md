# Persona 1 PC Recomp

Static recompilation of the 1999 Japanese Windows port of *Megami Ibunroku Persona*
(ASCII, `Persona.exe`) into portable C, running on SDL3 instead of DirectX 6 / DirectShow.
Inspired by [persona-digital-collection-recomp](https://github.com/ACR-GD/persona-digital-collection-recomp);
the generator and runtime here are written from scratch.

No game code or data is included: the C is generated from your own copy of the disc.

## Build
Needs Python 3 with `pefile` and `capstone`, clang, CMake, Ninja and SDL3
(on Windows: MSYS2 `mingw-w64-clang-x86_64-sdl3`).

    python -I tools/extract_disc.py "path/to/Persona.cue" data/cd     # disc files + data/Persona.exe
    python -I tools/recomp.py data/Persona.exe gen                    # x86 -> C
    cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang
    cmake --build build
    ./build/persona1.exe

## Launcher and options
A settings screen opens before the game (Play / Quit), and the same menu opens in game with **F10**
or the controller's Home/Guide button (the game pauses). Settings: fullscreen or window, window size
1x-4x, fill-screen or pixel-perfect integer scaling, sharp or smooth filtering, VSync, controller
layout (Western / Japanese), master volume, language (English once a translation is installed),
and whether to show the launcher. They are saved in `save/settings.ini`.

## Controls
Keyboard (the game's own defaults): numpad 8/2/4/6 move, S confirm, Z cancel, W / A the other face
buttons, numpad 7/1/9/3 shoulder buttons, numpad 0 / numpad . select / start. Arrows, Enter
(confirm) and Esc (cancel) also work. Alt+Enter toggles fullscreen.

Gamepad (any SDL-supported controller): D-pad or left stick moves; bottom face button confirms,
right face button cancels, top = triangle, left = square, bumpers/triggers = L1/R1/L2/R2,
Back/Start = select/start. Set `P1_PAD_JAPANESE=1` for the original Japanese layout
(right button confirms).

Environment: `P1_TRACE=1` logs every Win32 call; `P1_CD` / `P1_SAVE` / `P1_EXE` override paths.

## Status
- [x] Function discovery without relocations (~26k functions, 83% of .text)
- [x] x86 -> C emitter (integer, x87, string ops, switch tables)
- [x] C runtime startup reaches WinMain (kernel32, heap, TLS, threads, files, CD drive Q:)
- [x] USER32 window/message loop, registry config
- [x] DirectDraw + Direct3D (IDirect3D2 / HAL device) software renderer on SDL3
- [x] DirectShow: MP3 music/effects, MPEG-1 movies
- [x] Gamepads (SDL)
