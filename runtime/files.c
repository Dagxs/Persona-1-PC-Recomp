// Files and drives. The game looks for its CD by drive type and volume label, then reads data
// files from it; save files (Perdata.*) live next to the program.
//   Q:\...           -> the disc contents (P1_CD, default data/cd), read-only
//   C:\PERSONA\...   -> the save folder (P1_SAVE, default save)
//   relative paths   -> the save folder (the game's working directory)
#include "win32.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_host(p) _mkdir(p)
#else
#include <unistd.h>
#define mkdir_host(p) mkdir(p, 0755)
#endif

#define CD_LETTER 'Q'
static char cd_root[512], save_root[512];

void files_init(void) {
    const char *cd = getenv("P1_CD"), *sv = getenv("P1_SAVE");
    snprintf(cd_root, sizeof cd_root, "%s", cd ? cd : "data/cd");
    snprintf(save_root, sizeof save_root, "%s", sv ? sv : "save");
    mkdir_host(save_root);
}

// Map a guest path to a host path; returns 0 if it is outside every mapped area.
static int map_path(const char *g, char *out, size_t cap, int *on_cd) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", g);
    char *semi = strchr(tmp, ';');                 // ISO 9660 version suffix "FILE.BIN;1"
    if (semi) *semi = 0;
    for (char *c = tmp; *c; c++) if (*c == '\\') *c = '/';
    const char *rest;
    *on_cd = 0;
    if ((tmp[0] == CD_LETTER || tmp[0] == CD_LETTER + 32) && tmp[1] == ':') {
        rest = tmp + 2; *on_cd = 1;
        snprintf(out, cap, "%s/%s", cd_root, *rest == '/' ? rest + 1 : rest);
    } else if (tmp[1] == ':') {
        rest = tmp + 2;
        if (!strncmp(rest, "/PERSONA/", 9) || !strncmp(rest, "/persona/", 9)) rest += 9;
        else if (*rest == '/') rest++;
        const char *slash = strrchr(rest, '/');
        snprintf(out, cap, "%s/%s", save_root, slash ? slash + 1 : rest);
    } else {
        snprintf(out, cap, "%s/%s", save_root, tmp[0] == '/' ? tmp + 1 : tmp);
    }
    return 1;
}

struct gfile { FILE *f; int console; };

DEF(CreateFileA) {
    const char *g = gstr(a[0]);
    char host[1024]; int on_cd;
    map_path(g, host, sizeof host, &on_cd);
    uint32_t access = a[1], disp = a[4];
    const char *mode;
    int writing = (access & 0x40000000u) != 0;
    if (on_cd && writing) { last_error = 5; return INVALID_HANDLE; }
    struct stat st;
    int exists = stat(host, &st) == 0;
    switch (disp) {
    case 1: if (exists) { last_error = 80; return INVALID_HANDLE; } mode = "w+b"; break;  // CREATE_NEW
    case 2: mode = "w+b"; break;                                                          // CREATE_ALWAYS
    case 3: if (!exists) { last_error = ERROR_FILE_NOT_FOUND; goto fail; } mode = writing ? "r+b" : "rb"; break;
    case 4: mode = exists ? (writing ? "r+b" : "rb") : "w+b"; break;                       // OPEN_ALWAYS
    case 5: if (!exists) { last_error = ERROR_FILE_NOT_FOUND; goto fail; } mode = "w+b"; break;
    default: last_error = ERROR_INVALID_PARAMETER; return INVALID_HANDLE;
    }
    FILE *f = fopen(host, mode);
    if (!f) { last_error = ERROR_FILE_NOT_FOUND; goto fail; }
    struct gfile *gf = calloc(1, sizeof *gf);
    gf->f = f;
    if (rt_trace) fprintf(stderr, "CreateFileA(\"%s\") -> %s\n", g, host);
    last_error = (disp == 4 || disp == 2) && exists ? ERROR_ALREADY_EXISTS : 0;
    return handle_new(H_FILE, gf);
fail:
    if (rt_trace) fprintf(stderr, "CreateFileA(\"%s\") -> %s: not found\n", g, host);
    return INVALID_HANDLE;
}

static struct gfile std_files[3];
DEF(GetStdHandle) {
    int k = a[0] == 0xfffffff6u ? 0 : a[0] == 0xfffffff5u ? 1 : a[0] == 0xfffffff4u ? 2 : -1;
    if (k < 0) return INVALID_HANDLE;
    static uint32_t h[3];
    if (!h[k]) { std_files[k].console = 1; std_files[k].f = k == 0 ? stdin : k == 1 ? stdout : stderr; h[k] = handle_new(H_FILE, &std_files[k]); }
    return h[k];
}
DEF(SetStdHandle) { UNUSED_ARGS; return 1; }
DEF(GetFileType) { struct gfile *f = handle_get(a[0], H_FILE); return !f ? 0 : f->console ? 2 : 1; }
DEF(SetHandleCount) { return a[0]; }

DEF(ReadFile) {
    struct gfile *f = handle_get(a[0], H_FILE);
    if (!f) { last_error = ERROR_INVALID_HANDLE; return 0; }
    if (a[2] && (a[1] + a[2] > MEM_SIZE || a[1] + a[2] < a[1])) rt_fatal("ReadFile into bad buffer %08x+%x", a[1], a[2]);
    size_t n = a[2] ? fread(MEM + a[1], 1, a[2], f->f) : 0;
    if (a[3]) WR32(a[3], (uint32_t)n);
    return 1;
}
DEF(WriteFile) {
    struct gfile *f = handle_get(a[0], H_FILE);
    if (!f) { last_error = ERROR_INVALID_HANDLE; return 0; }
    size_t n = fwrite(MEM + a[1], 1, a[2], f->f);
    if (a[3]) WR32(a[3], (uint32_t)n);
    return 1;
}
DEF(SetFilePointer) {
    struct gfile *f = handle_get(a[0], H_FILE);
    if (!f) { last_error = ERROR_INVALID_HANDLE; return 0xffffffffu; }
    int64_t off = (int32_t)a[1];
    if (a[2]) off = (int64_t)(((uint64_t)RD32(a[2]) << 32) | a[1]);
    int whence = a[3] == 0 ? SEEK_SET : a[3] == 1 ? SEEK_CUR : SEEK_END;
    if (fseek(f->f, (long)off, whence)) { last_error = 131; return 0xffffffffu; }
    long pos = ftell(f->f);
    if (a[2]) WR32(a[2], 0);
    return (uint32_t)pos;
}
DEF(SetEndOfFile) {
    struct gfile *f = handle_get(a[0], H_FILE);
    if (!f) return 0;
    fflush(f->f);
#ifdef _WIN32
    return _chsize(_fileno(f->f), ftell(f->f)) == 0;
#else
    return ftruncate(fileno(f->f), ftell(f->f)) == 0;
#endif
}
DEF(FlushFileBuffers) { struct gfile *f = handle_get(a[0], H_FILE); if (f) fflush(f->f); return 1; }
int thread_close(uint32_t h);
DEF(CloseHandle) {
    struct gfile *f = handle_get(a[0], H_FILE);
    if (f) { if (!f->console) { fclose(f->f); free(f); } handle_close(a[0]); return 1; }
    if (thread_close(a[0])) return 1;
    if (handle_get(a[0], H_MODULE)) { handle_close(a[0]); return 1; }
    last_error = ERROR_INVALID_HANDLE;
    return 0;
}
DEF(GetFileAttributesA) {
    char host[1024]; int on_cd;
    map_path(gstr(a[0]), host, sizeof host, &on_cd);
    struct stat st;
    if (stat(host, &st)) { last_error = ERROR_FILE_NOT_FOUND; return 0xffffffffu; }
    uint32_t attr = (st.st_mode & S_IFDIR) ? 0x10u : 0x20u;
    if (on_cd) attr |= 1u;     // read-only
    return attr;
}
DEF(DeleteFileA) {
    char host[1024]; int on_cd;
    map_path(gstr(a[0]), host, sizeof host, &on_cd);
    if (on_cd) { last_error = 5; return 0; }
    return remove(host) == 0;
}

// ---------------------------------------------------------------- drives
static int drive_of(uint32_t p) {
    if (!p) return 'C';
    int c = RD8(p);
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}
DEF(GetDriveTypeA) {
    int d = drive_of(a[0]);
    if (d == CD_LETTER) return 5;        // DRIVE_CDROM
    if (d == 'C') return 3;              // DRIVE_FIXED
    return 1;                            // DRIVE_NO_ROOT_DIR
}
DEF(GetVolumeInformationA) {
    int d = drive_of(a[0]);
    if (d != CD_LETTER && d != 'C') { last_error = 21; return 0; }   // ERROR_NOT_READY
    if (a[1]) gstrcpy(a[1], d == CD_LETTER ? "Persona" : "SYSTEM", a[2]);
    if (a[3]) WR32(a[3], 0x19990226u);
    if (a[4]) WR32(a[4], 255);
    if (a[5]) WR32(a[5], 0);
    if (a[6]) gstrcpy(a[6], d == CD_LETTER ? "CDFS" : "NTFS", a[7]);
    return 1;
}

const struct impl FILES_IMPL[] = {
#define K(name, n) API("KERNEL32.DLL", name, n)
    K(CreateFileA, 7), K(GetStdHandle, 1), K(SetStdHandle, 2), K(GetFileType, 1), K(SetHandleCount, 1),
    K(ReadFile, 5), K(WriteFile, 5), K(SetFilePointer, 4), K(SetEndOfFile, 1), K(FlushFileBuffers, 1),
    K(CloseHandle, 1), K(GetFileAttributesA, 1), K(DeleteFileA, 1), K(GetDriveTypeA, 1),
    K(GetVolumeInformationA, 8),
#undef K
    { 0 }
};
