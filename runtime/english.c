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

// names.bin (tools/install_english.py): English item / skill / Persona / demon / nickname names,
// written over the Japanese ones in the game's tables: "P1NM", u32 count, {u32 address, u16 length,
// bytes}. Every address must lie in Persona.exe's image.
static int load_names(const char *en_dir) {
    char path[600];
    snprintf(path, sizeof path, "%s/names.bin", en_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hdr[8];
    int n = 0;
    if (fread(hdr, 1, 8, f) == 8 && !memcmp(hdr, "P1NM", 4)) {
        uint32_t count;
        memcpy(&count, hdr + 4, 4);
        for (uint32_t k = 0; k < count; k++) {
            uint8_t h[6], b[64];
            if (fread(h, 1, 6, f) != 6) break;
            uint32_t addr; uint16_t len;
            memcpy(&addr, h, 4); memcpy(&len, h + 4, 2);
            if (len > sizeof b || fread(b, 1, len, f) != len) break;
            if (addr < 0x401000u || addr + len > 0x700000u) continue;
            for (uint16_t i = 0; i < len; i++) WR8(addr + i, b[i]);
            n++;
        }
    }
    fclose(f);
    return n;
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
    int names = load_names(en);
    if (rt_trace) fprintf(stderr, "English names: %d\n", names);
    english_on = 1;
    vwf_active = 1;
    extern void subtitles_load(void);
    subtitles_load();
    // All three copies of the dialogue interpreter read the same story files (E0-E3), so all three
    // need the English engine: story (ADV), dungeon (B) and town (C). P1_VWF=mask overrides (tests).
    extern int vwf_on[3];
    int mask = getenv("P1_VWF") ? atoi(getenv("P1_VWF")) : 7;
    for (int k = 0; k < 3; k++) vwf_on[k] = (mask >> k) & 1;
    // the dungeon field script's boxes (vwf_field.c): packs from data/en, long boxes spilled
    extern void dng_spill_load(void);
    dng_spill_load();
    vwf_field_on = 1;
    if (rt_trace) fprintf(stderr, "English dialogue on (%s)\n", en);
}
