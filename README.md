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

Environment: `P1_TRACE=1` logs every Win32 call; `P1_CD` / `P1_SAVE` / `P1_EXE` override paths.

## Status
- [x] Function discovery without relocations (~26k functions, 83% of .text)
- [x] x86 -> C emitter (integer, x87, string ops, switch tables)
- [x] C runtime startup reaches WinMain (kernel32, heap, TLS, threads, files, CD drive Q:)
- [ ] USER32 window/message loop, registry config
- [ ] DirectDraw + Direct3D (IDirect3D2 / HAL device) renderer on SDL3
- [ ] DirectSound mixer output, joystick/keyboard input
- [ ] DirectShow movie playback (STR files)
