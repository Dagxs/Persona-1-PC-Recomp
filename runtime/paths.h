// Where the game's files and saves live (paths.c).
#pragma once
#include <stddef.h>

void paths_init(void);
const char *path_data(void);       // root of the installed data
const char *path_cd(void);         // the disc's files
const char *path_exe(void);        // Persona.exe
const char *path_en(void);         // English overlay
const char *path_save(void);       // saves
const char *path_settings(void);   // settings.ini
int game_installed(void);

// Install the game from a disc image (.cue / .bin / .iso) into path_data() (installer.c).
// progress(done, total, user) is called as files are copied; returns 0 on success, otherwise
// writes a message to err.
int install_from_disc(const char *image, void (*progress)(double done, double total, void *user), void *user,
                      char *err, size_t errcap);
