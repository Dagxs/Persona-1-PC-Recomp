// Player settings (save/settings.ini), edited in the launcher and the in-game menu (F10).
#pragma once

enum { LANG_JAPANESE, LANG_ENGLISH };

struct settings {
    int fullscreen;      // 0 window, 1 fullscreen (desktop resolution)
    int window_scale;    // window size as a multiple of 640x480: 1..4
    int integer_scale;   // 1: only whole-number scale factors (pixel-perfect), 0: fill the window
    int smooth;          // 1: bilinear filtering, 0: sharp pixels
    int vsync;
    int pad_japanese;    // 1: right face button confirms (original Japanese layout)
    int volume;          // master volume 0..100
    int language;        // LANG_*
    int show_launcher;   // show the launcher before the game starts
};

extern struct settings cfg;
void settings_load(void);
void settings_save(void);
int settings_english_available(void);   // is an English translation installed?
