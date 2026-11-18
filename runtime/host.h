// Host platform services (SDL3): window, events, keyboard, presentation, audio.
#pragma once
#include <stdint.h>

void host_window_create(const char *title);
void host_message_box(const char *caption, const char *text);
void host_pump_events(void);
int host_key_down(int vk);
const char *host_button_label(int action, int *ps);   // see host.c
// present a 32-bit XRGB frame of w x h pixels (scaled to the window)
void host_present(const uint32_t *pixels, int w, int h);

struct SDL_AudioStream;
// a new audio stream of 16-bit samples, bound to the output device (NULL when audio is off)
struct SDL_AudioStream *host_audio_stream(int channels, int rate);
struct SDL_AudioStream *host_audio_stream_f32(int channels, int rate);   // float samples

// settings, launcher and in-game menu (menu.c, settings.c)
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Renderer *host_renderer(void);
struct SDL_Texture *host_last_frame(void);
void host_apply_settings(int resize);
void host_launcher(void);
void host_audio_volume(void);
void host_audio_pause(int paused);
int menu_run(int launcher);

uint32_t guest_call(uint32_t fn, int n, const uint32_t *args);
void user32_post_key(int vk, int down);
void user32_post_close(void);
void DestroyWindow_host(uint32_t hwnd);
extern uint32_t main_hwnd;
