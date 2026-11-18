// SDL3 host: the game window, keyboard state (Win32 virtual-key codes), frame presentation.
#include "rt.h"
#include "host.h"
#include "settings.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *frame;
static int frame_w, frame_h;
static uint8_t keys[256];
static uint8_t pad[256];          // keys held by gamepads (see pad_vk)

// ---------------------------------------------------------------- gamepads
// Besides its configurable keys, Persona always reads a fixed set of keys, each tied to a
// PlayStation button: arrows = D-pad, Enter = circle (confirm), Esc = cross (cancel), F2 = triangle,
// F1 = square, Home/End/PgUp/PgDn = L1/L2/R1/R2, Insert = select, Delete = start.
// Gamepads press those keys. The bottom face button confirms and the right one cancels; the
// "Japanese" controller layout setting swaps them to the original PlayStation layout.
enum { VK_UP = 0x26, VK_DOWN = 0x28, VK_LEFT = 0x25, VK_RIGHT = 0x27, VK_CIRCLE = 0x0d, VK_CROSS = 0x1b,
       VK_TRIANGLE = 0x71, VK_SQUARE = 0x70, VK_L1 = 0x24, VK_L2 = 0x23, VK_R1 = 0x21, VK_R2 = 0x22,
       VK_SELECT = 0x2d, VK_START = 0x2e };
static int pad_vk(SDL_GamepadButton b) {
    int western = !cfg.pad_japanese;
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return western ? VK_CIRCLE : VK_CROSS;
    case SDL_GAMEPAD_BUTTON_EAST: return western ? VK_CROSS : VK_CIRCLE;
    case SDL_GAMEPAD_BUTTON_NORTH: return VK_TRIANGLE;
    case SDL_GAMEPAD_BUTTON_WEST: return VK_SQUARE;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return VK_L1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return VK_R1;
    case SDL_GAMEPAD_BUTTON_BACK: return VK_SELECT;
    case SDL_GAMEPAD_BUTTON_START: return VK_START;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return VK_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return VK_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return VK_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return VK_RIGHT;
    default: return 0;
    }
}
// D-pad buttons, the left stick and the triggers are recomputed from the pad state each time,
// so several pads and the stick can be used together without one releasing the other's keys.
static void pad_refresh(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    uint8_t now[256] = { 0 };
    for (int k = 0; k < n; k++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[k]);
        if (!g) continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
            int vk = pad_vk((SDL_GamepadButton)b);
            if (vk && SDL_GetGamepadButton(g, (SDL_GamepadButton)b)) now[vk] = 1;
        }
        const int dead = 16000;
        int lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
        if (lx < -dead) now[VK_LEFT] = 1;
        if (lx > dead) now[VK_RIGHT] = 1;
        if (ly < -dead) now[VK_UP] = 1;
        if (ly > dead) now[VK_DOWN] = 1;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) now[VK_L2] = 1;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) now[VK_R2] = 1;
    }
    SDL_free(ids);
    memcpy(pad, now, sizeof pad);
}
static int headless;
static volatile int menu_requested;   // F10 / Guide pressed: open the options menu after this frame

void host_window_create(const char *title) {
    if (window || headless) return;
    if (getenv("P1_HEADLESS")) { headless = 1; return; }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) rt_fatal("SDL video: %s", SDL_GetError());
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) fprintf(stderr, "gamepads unavailable: %s\n", SDL_GetError());
    (void)title;   // the Japanese title is Shift-JIS; SDL wants UTF-8
    window = SDL_CreateWindow("Megami Ibunroku Persona", 640 * cfg.window_scale, 480 * cfg.window_scale, SDL_WINDOW_RESIZABLE);
    if (!window) rt_fatal("SDL window: %s", SDL_GetError());
    renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) rt_fatal("SDL renderer: %s", SDL_GetError());
    host_apply_settings(0);
}

SDL_Renderer *host_renderer(void) { return renderer; }
SDL_Texture *host_last_frame(void) { return frame; }

// Apply display and audio settings; resize: also set the window size from the scale setting.
void host_apply_settings(int resize) {
    if (!window) return;
    SDL_SetWindowFullscreen(window, cfg.fullscreen);
    if (resize && !cfg.fullscreen) {
        SDL_SetWindowSize(window, 640 * cfg.window_scale, 480 * cfg.window_scale);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    SDL_SetRenderVSync(renderer, cfg.vsync ? 1 : 0);
    SDL_SetRenderLogicalPresentation(renderer, frame_w ? frame_w : 640, frame_h ? frame_h : 480,
                                     cfg.integer_scale ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE : SDL_LOGICAL_PRESENTATION_LETTERBOX);
    if (frame) SDL_SetTextureScaleMode(frame, cfg.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    host_audio_volume();
}

// The launcher: shown before the game starts unless turned off (or P1_HEADLESS / P1_NOLAUNCHER).
void host_launcher(void) {
    settings_load();
    if (getenv("P1_HEADLESS") || getenv("P1_NOLAUNCHER") || !cfg.show_launcher) return;
    host_window_create("");
    if (!menu_run(1)) exit(0);
}

void host_message_box(const char *caption, const char *text) {
    (void)caption; (void)text;     // Shift-JIS text: logged to stderr by the caller
}

// SDL scancode -> Win32 virtual-key code
static int vk_of(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return 0x70 + (s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return 0x61 + (s - SDL_SCANCODE_KP_1);
    switch (s) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_KP_PERIOD: return 0x6e;
    case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_RETURN: return 0x0d;
    case SDL_SCANCODE_ESCAPE: return 0x1b;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    default: return 0;
    }
}

// P1_KEYS="frame:vk:down,..." scripts key presses for tests, e.g. "300:83:1,303:83:0" taps S.
static void scripted_keys(void) {
    static int init, n, k;
    static uint32_t ev[256][3];
    if (!init) {
        init = 1;
        const char *s = getenv("P1_KEYS");
        while (s && *s && n < 256) {
            unsigned f, vk, d;
            if (sscanf(s, "%u:%u:%u", &f, &vk, &d) != 3) break;
            ev[n][0] = f; ev[n][1] = vk; ev[n][2] = d; n++;
            s = strchr(s, ',');
            if (s) s++;
        }
    }
    extern uint32_t frame_counter;
    while (k < n && ev[k][0] <= frame_counter) {
        keys[ev[k][1] & 255] = (uint8_t)ev[k][2];
        user32_post_key((int)ev[k][1], (int)ev[k][2]);
        k++;
    }
}

void host_pump_events(void) {
    scripted_keys();
    if (headless || !window) return;
    SDL_UpdateGamepads();
    pad_refresh();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_GAMEPAD_ADDED: {
            SDL_Gamepad *g = SDL_OpenGamepad(e.gdevice.which);
            if (g) fprintf(stderr, "gamepad connected: %s\n", SDL_GetGamepadName(g));
            break;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) menu_requested = 1;
            break;
        case SDL_EVENT_GAMEPAD_REMOVED: {
            SDL_Gamepad *g = SDL_GetGamepadFromID(e.gdevice.which);
            if (g) SDL_CloseGamepad(g);
            break;
        }
        case SDL_EVENT_QUIT:
            user32_post_close();
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            int vk = vk_of(e.key.scancode), down = e.type == SDL_EVENT_KEY_DOWN;
            if (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT) && down) {
                cfg.fullscreen = !cfg.fullscreen;
                settings_save();
                host_apply_settings(1);
                break;
            }
            if (e.key.scancode == SDL_SCANCODE_F10) { if (down) menu_requested = 1; break; }
            if (vk) { keys[vk] = (uint8_t)down; if (!e.key.repeat) user32_post_key(vk, down); }
            if (e.key.scancode == SDL_SCANCODE_KP_ENTER) keys[0x0d] = (uint8_t)down;
            break;
        }
        default: break;
        }
    }
}

int host_key_down(int vk) { return vk >= 0 && vk < 256 && (keys[vk] || pad[vk]); }

void host_present(const uint32_t *pixels, int w, int h) {
    if (headless || !renderer) return;
    if (!frame || w != frame_w || h != frame_h) {
        if (frame) SDL_DestroyTexture(frame);
        frame = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        frame_w = w; frame_h = h;
        host_apply_settings(0);
    }
    SDL_UpdateTexture(frame, NULL, pixels, w * 4);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, frame, NULL, NULL);
    SDL_RenderPresent(renderer);
    if (menu_requested) {
        menu_requested = 0;
        memset(keys, 0, sizeof keys);
        memset(pad, 0, sizeof pad);
        if (!menu_run(0)) exit(0);
        SDL_SetRenderLogicalPresentation(renderer, frame_w, frame_h,
                                         cfg.integer_scale ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE : SDL_LOGICAL_PRESENTATION_LETTERBOX);
    }
}

// ---------------------------------------------------------------- test hooks
// P1_SHOT=dir writes every P1_SHOT_EVERY-th presented frame (default 30) as dir/fNNNNN.bmp;
// P1_FRAMES=n exits after n presented frames.
void dump_frame_if_requested(const uint32_t *px, int w, int h) {
    static int init, every = 30, limit;
    static const char *dir;
    static uint32_t n;
    if (!init) {
        init = 1;
        dir = getenv("P1_SHOT");
        if (getenv("P1_SHOT_EVERY")) every = atoi(getenv("P1_SHOT_EVERY"));
        if (getenv("P1_FRAMES")) limit = atoi(getenv("P1_FRAMES"));
        if (every < 1) every = 1;
    }
    n++;
    if (dir && n % (uint32_t)every == 0) {
        char path[512];
        snprintf(path, sizeof path, "%s/f%05u.bmp", dir, n);
        FILE *f = fopen(path, "wb");
        if (f) {
            uint32_t sz = 54u + (uint32_t)(w * h * 4);
            uint8_t hdr[54] = { 'B', 'M' };
            memcpy(hdr + 2, &sz, 4); hdr[10] = 54; hdr[14] = 40;
            memcpy(hdr + 18, &w, 4); int32_t nh = -h; memcpy(hdr + 22, &nh, 4);
            hdr[26] = 1; hdr[28] = 32;
            fwrite(hdr, 1, 54, f);
            fwrite(px, 4, (size_t)(w * h), f);
            fclose(f);
        }
    }
    if (limit && n >= (uint32_t)limit) { fprintf(stderr, "P1_FRAMES reached (%u frames)\n", n); exit(0); }
}

// ---------------------------------------------------------------- audio
static SDL_AudioDeviceID audio_dev;
static int audio_state;   // 0 untried, 1 ok, -1 off
static SDL_AudioStream *audio_stream(SDL_AudioFormat fmt, int channels, int rate);
SDL_AudioStream *host_audio_stream(int channels, int rate) { return audio_stream(SDL_AUDIO_S16, channels, rate); }
SDL_AudioStream *host_audio_stream_f32(int channels, int rate) { return audio_stream(SDL_AUDIO_F32, channels, rate); }
static SDL_AudioStream *audio_stream(SDL_AudioFormat fmt, int channels, int rate) {
    if (!audio_state) {
        audio_state = -1;
        if (!getenv("P1_NOAUDIO") && (!headless || getenv("P1_AUDIO")) && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            audio_dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
            if (audio_dev) { audio_state = 1; host_audio_volume(); }
            else fprintf(stderr, "audio: %s\n", SDL_GetError());
        }
    }
    SDL_AudioSpec src = { fmt, channels, rate };
    SDL_AudioSpec dst;
    if (audio_state == 1) SDL_GetAudioDeviceFormat(audio_dev, &dst, NULL);
    else dst = src;
    SDL_AudioStream *s = SDL_CreateAudioStream(&src, &dst);
    if (s && audio_state == 1) SDL_BindAudioStream(audio_dev, s);
    return s;
}

void host_audio_volume(void) {
    if (audio_state == 1) SDL_SetAudioDeviceGain(audio_dev, (float)cfg.volume / 100.0f);
}
void host_audio_pause(int paused) {
    if (audio_state != 1) return;
    if (paused) SDL_PauseAudioDevice(audio_dev); else SDL_ResumeAudioDevice(audio_dev);
}
