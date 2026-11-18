// Language = English: switch the game to the data/en overlay at startup (tools/install_english.py
// builds it). The story scripts ADV/E0-E3.BIN are read from data/en (files.c), their sector tables
// in Persona.exe are replaced by the rebuilt files' own tables (entries moved when the English
// boxes grew), and the dialogue engine (vwf.c) is switched on with the English font.
#include "rt.h"
#include "paths.h"
#include "settings.h"
#include <stdlib.h>
#include <string.h>

int vwf_load_font(const char *path);
extern int vwf_active;

int english_on;

// where Persona.exe keeps the sector table of each story script (as on the PS1, one u16 per entry)
static const struct { const char *file; uint32_t table; } SCRIPTS[] = {
    { "E0", 0x57946cu }, { "E1", 0x579630u }, { "E2", 0x5797f4u }, { "E3", 0x5799d8u },
};

static int patch_table(const char *en_dir, const char *name, uint32_t table) {
    char path[600];
    snprintf(path, sizeof path, "%s/ADV/%s.BIN", en_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint16_t t[1024];
    size_t n = fread(t, 2, 1024, f);
    fclose(f);
    // the file's sector 0 is the zero-terminated table; the executable's copy has the same count
    size_t k = 1;
    while (k < n && t[k]) k++;
    for (size_t j = 0; j < k; j++) {
        if (RD16(table + 2u * j) == 0 && j > 0) break;     // never write past the original table
        WR16(table + 2u * j, t[j]);
    }
    return 1;
}

void english_boot(void) {
    if (cfg.language != LANG_ENGLISH || !settings_english_available()) return;
    const char *en = path_en();
    char path[600];
    snprintf(path, sizeof path, "%s/vwf_font.bin", en);
    if (!vwf_load_font(path)) {
        fprintf(stderr, "English: %s is missing or damaged; staying in Japanese\n", path);
        return;
    }
    for (size_t k = 0; k < sizeof SCRIPTS / sizeof SCRIPTS[0]; k++)
        if (!patch_table(en, SCRIPTS[k].file, SCRIPTS[k].table))
            fprintf(stderr, "English: %s/ADV/%s.BIN missing; that part of the script stays Japanese\n", en, SCRIPTS[k].file);
    english_on = 1;
    vwf_active = 1;
    // Only the story interpreter has verified English data so far. The dungeon (VWF_B) and
    // overworld (VWF_C) copies are hooked too, but stay on the original code until theirs is.
    extern int vwf_on[3];
    vwf_on[0] = 1;
    if (rt_trace) fprintf(stderr, "English dialogue on (%s)\n", en);
}
