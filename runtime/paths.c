// Where the game's files live.
//   data/        the disc's files (cd/), Persona.exe, the English overlay (en/)
//   save/        saves and settings.ini
// Looked for, in order: P1_DATA / P1_SAVE, a data/ folder in the working directory (a source
// checkout), a data/ folder next to the program, then a per-user folder that the launcher's
// "Install from disc image" fills (e.g. %APPDATA%\Persona1Recomp\Persona1\ on Windows).
#include "rt.h"
#include "paths.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

static char data_dir[1024], save_dir[1024];
static char cd_dir[1100], exe_path[1100], en_dir[1100], ini_path[1100];

static int exists(const char *p) { return SDL_GetPathInfo(p, NULL); }

static void finish(void) {
    snprintf(cd_dir, sizeof cd_dir, "%s/cd", data_dir);
    snprintf(exe_path, sizeof exe_path, "%s/Persona.exe", data_dir);
    snprintf(en_dir, sizeof en_dir, "%s/en", data_dir);
    snprintf(ini_path, sizeof ini_path, "%s/settings.ini", save_dir);
}

void paths_init(void) {
    const char *d = getenv("P1_DATA"), *s = getenv("P1_SAVE");
    char cand[1100];
    if (d) {
        snprintf(data_dir, sizeof data_dir, "%s", d);
        snprintf(save_dir, sizeof save_dir, "%s", s ? s : "save");
    } else if (exists("data/Persona.exe")) {                  // a source checkout
        snprintf(data_dir, sizeof data_dir, "data");
        snprintf(save_dir, sizeof save_dir, "%s", s ? s : "save");
    } else {
        const char *base = SDL_GetBasePath();                  // next to the program
        snprintf(cand, sizeof cand, "%sdata/Persona.exe", base ? base : "");
        if (base && exists(cand)) {
            snprintf(data_dir, sizeof data_dir, "%sdata", base);
            snprintf(save_dir, sizeof save_dir, "%s", s ? s : "save");
            if (!s) snprintf(save_dir, sizeof save_dir, "%ssave", base);
        } else {                                               // the per-user install
            char *pref = SDL_GetPrefPath("Persona1Recomp", "Persona1");
            snprintf(data_dir, sizeof data_dir, "%sdata", pref ? pref : "");
            if (s) snprintf(save_dir, sizeof save_dir, "%s", s);
            else snprintf(save_dir, sizeof save_dir, "%ssave", pref ? pref : "");
            SDL_free(pref);
        }
    }
    finish();
    SDL_CreateDirectory(save_dir);
}

const char *path_data(void) { return data_dir; }
const char *path_save(void) { return save_dir; }
const char *path_cd(void) { return getenv("P1_CD") ? getenv("P1_CD") : cd_dir; }
const char *path_exe(void) { return getenv("P1_EXE") ? getenv("P1_EXE") : exe_path; }
const char *path_en(void) { return getenv("P1_EN") ? getenv("P1_EN") : en_dir; }
const char *path_settings(void) { return ini_path; }
int game_installed(void) { return exists(path_exe()) && exists(path_cd()); }
