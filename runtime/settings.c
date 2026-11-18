// Player settings, stored as key=value lines in save/settings.ini.
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct settings cfg = {
    .fullscreen = 0, .window_scale = 2, .integer_scale = 0, .smooth = 0, .vsync = 1,
    .pad_japanese = 0, .volume = 100, .language = LANG_JAPANESE, .show_launcher = 1,
};

static const struct { const char *key; int *val; int lo, hi; } FIELDS[] = {
    { "fullscreen", &cfg.fullscreen, 0, 1 }, { "window_scale", &cfg.window_scale, 1, 4 },
    { "integer_scale", &cfg.integer_scale, 0, 1 }, { "smooth", &cfg.smooth, 0, 1 },
    { "vsync", &cfg.vsync, 0, 1 }, { "pad_japanese", &cfg.pad_japanese, 0, 1 },
    { "volume", &cfg.volume, 0, 100 }, { "language", &cfg.language, 0, 1 },
    { "show_launcher", &cfg.show_launcher, 0, 1 },
};
#define NFIELDS (int)(sizeof FIELDS / sizeof FIELDS[0])

static void path_of(char *out, size_t cap) {
    const char *sv = getenv("P1_SAVE");
    snprintf(out, cap, "%s/settings.ini", sv ? sv : "save");
}

void settings_load(void) {
    char path[512], line[256];
    path_of(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == ';') continue;
        *eq = 0;
        for (int k = 0; k < NFIELDS; k++)
            if (!strcmp(line, FIELDS[k].key)) {
                int v = atoi(eq + 1);
                *FIELDS[k].val = v < FIELDS[k].lo ? FIELDS[k].lo : v > FIELDS[k].hi ? FIELDS[k].hi : v;
            }
    }
    fclose(f);
    if (cfg.language == LANG_ENGLISH && !settings_english_available()) cfg.language = LANG_JAPANESE;
}

void settings_save(void) {
    char path[512];
    path_of(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Megami Ibunroku Persona (PC recomp) settings\n");
    for (int k = 0; k < NFIELDS; k++) fprintf(f, "%s=%d\n", FIELDS[k].key, *FIELDS[k].val);
    fclose(f);
}

// The English translation will ship as a folder of replacement data; until then it is absent.
int settings_english_available(void) {
    struct stat st;
    return stat("data/en", &st) == 0;
}
