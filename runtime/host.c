// SDL3 host: the game window, keyboard state (Win32 virtual-key codes), frame presentation.
#include "rt.h"
#include "host.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *frame;
static int frame_w, frame_h;
static uint8_t keys[256];
static int headless;

void host_window_create(const char *title) {
    if (window || headless) return;
    if (getenv("P1_HEADLESS")) { headless = 1; return; }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) rt_fatal("SDL video: %s", SDL_GetError());
    (void)title;   // the Japanese title is Shift-JIS; SDL wants UTF-8
    window = SDL_CreateWindow("Megami Ibunroku Persona", 1280, 960, SDL_WINDOW_RESIZABLE);
    if (!window) rt_fatal("SDL window: %s", SDL_GetError());
    renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) rt_fatal("SDL renderer: %s", SDL_GetError());
    SDL_SetRenderLogicalPresentation(renderer, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(renderer, 1);
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

void host_pump_events(void) {
    if (headless || !window) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            user32_post_close();
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            int vk = vk_of(e.key.scancode), down = e.type == SDL_EVENT_KEY_DOWN;
            if (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT) && down) {
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                break;
            }
            if (vk) { keys[vk] = (uint8_t)down; if (!e.key.repeat) user32_post_key(vk, down); }
            if (e.key.scancode == SDL_SCANCODE_KP_ENTER) keys[0x0d] = (uint8_t)down;
            break;
        }
        default: break;
        }
    }
}

int host_key_down(int vk) { return vk >= 0 && vk < 256 && keys[vk]; }

void host_present(const uint32_t *pixels, int w, int h) {
    if (headless || !renderer) return;
    if (!frame || w != frame_w || h != frame_h) {
        if (frame) SDL_DestroyTexture(frame);
        frame = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureScaleMode(frame, SDL_SCALEMODE_NEAREST);
        SDL_SetRenderLogicalPresentation(renderer, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        frame_w = w; frame_h = h;
    }
    SDL_UpdateTexture(frame, NULL, pixels, w * 4);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, frame, NULL, NULL);
    SDL_RenderPresent(renderer);
}
