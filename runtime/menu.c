// The launcher (before the game starts) and the in-game options menu (F10 or the controller's
// Guide/Home button). Both show the same settings list; changes apply immediately and are saved.
// Drawn with SDL's built-in 8x8 debug font on a 960x720 canvas; works with keyboard, mouse and
// gamepad. While the in-game menu is open the game is paused (its threads wait in Flip).
#include "rt.h"
#include "host.h"
#include "settings.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CW 960
#define CH 720

enum { IT_FULLSCREEN, IT_SCALE, IT_RENDER, IT_INTEGER, IT_FILTER, IT_VSYNC, IT_PAD, IT_VOLUME, IT_LANGUAGE, IT_LAUNCHER,
       IT_GO, IT_QUIT, IT_COUNT };

static const char *label(int it, int launcher) {
    switch (it) {
    case IT_FULLSCREEN: return "Display mode";
    case IT_SCALE: return "Window size";
    case IT_RENDER: return "Internal resolution";
    case IT_INTEGER: return "Scaling";
    case IT_FILTER: return "Filter";
    case IT_VSYNC: return "VSync";
    case IT_PAD: return "Controller layout";
    case IT_VOLUME: return "Volume";
    case IT_LANGUAGE: return "Language";
    case IT_LAUNCHER: return "Launcher at startup";
    case IT_GO: return launcher ? "Play" : "Resume";
    default: return launcher ? "Quit" : "Quit game";
    }
}

static void value(int it, char *out, size_t cap) {
    switch (it) {
    case IT_FULLSCREEN: snprintf(out, cap, "%s", cfg.fullscreen ? "Fullscreen" : "Window"); break;
    case IT_SCALE: snprintf(out, cap, "%dx  (%d x %d)", cfg.window_scale, 640 * cfg.window_scale, 480 * cfg.window_scale); break;
    case IT_RENDER: snprintf(out, cap, "%dx  (%d x %d)%s", cfg.render_scale, 640 * cfg.render_scale, 480 * cfg.render_scale,
                             cfg.render_scale == 1 ? "  original" : ""); break;
    case IT_INTEGER: snprintf(out, cap, "%s", cfg.integer_scale ? "Pixel-perfect" : "Fill screen (4:3)"); break;
    case IT_FILTER: snprintf(out, cap, "%s", cfg.smooth ? "Smooth" : "Sharp pixels"); break;
    case IT_VSYNC: snprintf(out, cap, "%s", cfg.vsync ? "On" : "Off"); break;
    case IT_PAD: snprintf(out, cap, "%s", cfg.pad_japanese ? "Japanese (right confirms)" : "Western (bottom confirms)"); break;
    case IT_VOLUME: snprintf(out, cap, "%d%%", cfg.volume); break;
    case IT_LANGUAGE:
        snprintf(out, cap, "%s", cfg.language == LANG_ENGLISH ? "English" :
                 settings_english_available() ? "Japanese" : "Japanese (no English yet)");
        break;
    case IT_LAUNCHER: snprintf(out, cap, "%s", cfg.show_launcher ? "Show" : "Skip"); break;
    default: out[0] = 0;
    }
}

// change a setting by dir (+1 / -1); returns 1 if something changed
static int change(int it, int dir) {
    switch (it) {
    case IT_FULLSCREEN: cfg.fullscreen = !cfg.fullscreen; return 1;
    case IT_SCALE: cfg.window_scale = (cfg.window_scale - 1 + dir + 4) % 4 + 1; return 1;
    case IT_RENDER: cfg.render_scale = (cfg.render_scale - 1 + dir + 4) % 4 + 1; return 1;
    case IT_INTEGER: cfg.integer_scale = !cfg.integer_scale; return 1;
    case IT_FILTER: cfg.smooth = !cfg.smooth; return 1;
    case IT_VSYNC: cfg.vsync = !cfg.vsync; return 1;
    case IT_PAD: cfg.pad_japanese = !cfg.pad_japanese; return 1;
    case IT_VOLUME: {
        int v = cfg.volume + 10 * dir;
        cfg.volume = v < 0 ? 0 : v > 100 ? 100 : v;
        return 1;
    }
    case IT_LANGUAGE:
        if (!settings_english_available()) return 0;
        cfg.language = !cfg.language;
        return 1;
    case IT_LAUNCHER: cfg.show_launcher = !cfg.show_launcher; return 1;
    default: return 0;
    }
}

// ---------------------------------------------------------------- drawing
static SDL_Renderer *R_;
static void text(float x, float y, float scale, const char *s, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    SDL_SetRenderScale(R_, scale, scale);
    SDL_SetRenderDrawColor(R_, r, g, b, a);
    SDL_RenderDebugText(R_, x / scale, y / scale, s);
    SDL_SetRenderScale(R_, 1, 1);
}
static void rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    SDL_FRect f = { x, y, w, h };
    SDL_SetRenderDrawColor(R_, r, g, b, a);
    SDL_RenderFillRect(R_, &f);
}

#define ROW_Y(k) (176.0f + 38.0f * (float)(k) + ((k) >= IT_GO ? 24.0f : 0.0f))
#define ROW_X 96.0f
#define ROW_W 768.0f
#define VAL_X (ROW_X + 352.0f)

static void draw(int sel, int launcher, SDL_Texture *bg) {
    SDL_SetRenderDrawBlendMode(R_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(R_, 0, 0, 0, 255);
    SDL_RenderClear(R_);
    if (bg) {
        SDL_RenderTexture(R_, bg, NULL, NULL);
        rect(0, 0, CW, CH, 0, 0, 0, 170);
    } else {
        SDL_Vertex g[4] = {
            { { 0, 0 }, { 0.10f, 0.04f, 0.16f, 1 }, { 0, 0 } }, { { CW, 0 }, { 0.10f, 0.04f, 0.16f, 1 }, { 0, 0 } },
            { { 0, CH }, { 0.20f, 0.10f, 0.32f, 1 }, { 0, 0 } }, { { CW, CH }, { 0.20f, 0.10f, 0.32f, 1 }, { 0, 0 } } };
        int idx[6] = { 0, 1, 2, 1, 3, 2 };
        SDL_RenderGeometry(R_, NULL, g, 4, idx, 6);
    }
    if (launcher) {
        text(ROW_X, 64, 4, "MEGAMI IBUNROKU PERSONA", 240, 200, 120, 255);
        text(ROW_X, 112, 2, "PC recompilation - settings", 200, 200, 220, 255);
    } else {
        text(ROW_X, 96, 4, "OPTIONS", 240, 200, 120, 255);
        text(ROW_X, 144, 2, "Game paused", 200, 200, 220, 255);
    }
    char v[128];
    for (int k = 0; k < IT_COUNT; k++) {
        float y = ROW_Y(k);
        if (k == sel) rect(ROW_X - 16, y - 8, ROW_W + 32, 32, 90, 70, 160, 200);
        if (k >= IT_GO) {
            text(ROW_X, y, 2, label(k, launcher), k == sel ? 255 : 220, k == sel ? 240 : 220, 200, 255);
            continue;
        }
        text(ROW_X, y, 2, label(k, launcher), 220, 220, 230, 255);
        value(k, v, sizeof v);
        int greyed = k == IT_LANGUAGE && !settings_english_available();
        if (k == sel && !greyed) {
            char w[160];
            snprintf(w, sizeof w, "< %s >", v);
            text(VAL_X - 32, y, 2, w, 255, 240, 160, 255);
        } else text(VAL_X, y, 2, v, greyed ? 140 : 255, greyed ? 140 : 230, greyed ? 140 : 160, 255);
    }
    text(ROW_X, CH - 72, 1.5f, "Up/Down: choose   Left/Right: change   Enter or A: select", 170, 170, 190, 255);
    text(ROW_X, CH - 48, 1.5f, launcher ? "In game: F10 or the controller's Home button opens this menu"
                                        : "Esc, B or F10: back to the game", 170, 170, 190, 255);
    const char *shot = getenv("P1_MENUSHOT");      // test hook: save the menu as a BMP and exit
    if (shot) {
        SDL_Surface *s = SDL_RenderReadPixels(R_, NULL);
        if (s) { SDL_SaveBMP(s, shot); SDL_DestroySurface(s); }
        exit(0);
    }
    SDL_RenderPresent(R_);
}

static int row_at(float my) {
    for (int k = 0; k < IT_COUNT; k++) {
        float y = ROW_Y(k);
        if (my >= y - 8 && my < y + 24) return k;
    }
    return -1;
}

// Runs the menu until the player leaves it. Returns 1 for Play/Resume, 0 for Quit.
int menu_run(int launcher) {
    SDL_Renderer *r = host_renderer();
    if (!r) return 1;
    R_ = r;
    SDL_Texture *bg = launcher ? NULL : host_last_frame();
    SDL_SetRenderLogicalPresentation(r, CW, CH, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    if (bg) SDL_SetTextureScaleMode(bg, SDL_SCALEMODE_LINEAR);
    host_audio_pause(1);
    int sel = IT_GO, result = -1;
    while (result < 0) {
        draw(sel, launcher, bg);
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        do {
            int act = 0, dir = 0, move = 0, back = 0;
            switch (e.type) {
            case SDL_EVENT_QUIT: result = 0; break;
            case SDL_EVENT_KEY_DOWN:
                switch (e.key.scancode) {
                case SDL_SCANCODE_UP: move = -1; break;
                case SDL_SCANCODE_DOWN: move = 1; break;
                case SDL_SCANCODE_LEFT: dir = -1; break;
                case SDL_SCANCODE_RIGHT: dir = 1; break;
                case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: act = 1; break;
                case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_F10: back = 1; break;
                default: break;
                }
                break;
            case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                switch (e.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_UP: move = -1; break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN: move = 1; break;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT: dir = -1; break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: dir = 1; break;
                case SDL_GAMEPAD_BUTTON_SOUTH: act = 1; break;
                case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_GUIDE: case SDL_GAMEPAD_BUTTON_START: back = 1; break;
                default: break;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION: {
                SDL_ConvertEventToRenderCoordinates(r, &e);
                int k = row_at(e.motion.y);
                if (k >= 0) sel = k;
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                SDL_ConvertEventToRenderCoordinates(r, &e);
                int k = row_at(e.button.y);
                if (k >= 0) { sel = k; if (e.button.button == SDL_BUTTON_RIGHT) dir = -1; else act = 1; }
                break;
            }
            default: break;
            }
            if (move) sel = (sel + move + IT_COUNT) % IT_COUNT;
            if (back && !launcher) result = 1;
            if (act && sel == IT_GO) result = 1;
            else if (act && sel == IT_QUIT) result = 0;
            else if (act || dir) {
                if (change(sel, dir ? dir : 1)) {
                    settings_save();
                    host_apply_settings(sel == IT_SCALE || sel == IT_FULLSCREEN);
                    SDL_SetRenderLogicalPresentation(r, CW, CH, SDL_LOGICAL_PRESENTATION_LETTERBOX);
                }
            }
        } while (result < 0 && SDL_PollEvent(&e));
    }
    settings_save();
    host_audio_pause(0);
    if (bg) SDL_SetTextureScaleMode(bg, cfg.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    host_apply_settings(0);
    return result;
}
