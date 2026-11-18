// Install the game from the player's disc image: copies every file on the Persona (Windows, 1999)
// disc into path_cd() and its Persona.exe to path_exe(), so the disc no longer has to be
// mounted. Reads a .cue sheet, a raw .bin (2352-byte MODE1 sectors) or a plain .iso.
#include "rt.h"
#include "paths.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

struct disc { SDL_IOStream *io; int sector_size, header; };

static int read_sectors(struct disc *d, uint32_t lba, uint32_t count, uint8_t *out) {
    for (uint32_t k = 0; k < count; k++) {
        if (SDL_SeekIO(d->io, (Sint64)(lba + k) * d->sector_size + d->header, SDL_IO_SEEK_SET) < 0) return 0;
        if (SDL_ReadIO(d->io, out + (size_t)k * 2048, 2048) != 2048) return 0;
    }
    return 1;
}

// .cue -> the .bin it names (cue sheets often disagree with the file's real case)
static int resolve_image(const char *path, char *out, size_t cap) {
    size_t n = strlen(path);
    if (n < 4 || SDL_strcasecmp(path + n - 4, ".cue")) { snprintf(out, cap, "%s", path); return 1; }
    size_t len;
    char *txt = SDL_LoadFile(path, &len);
    if (!txt) return 0;
    char *q = strstr(txt, "FILE \"");
    if (!q) { SDL_free(txt); return 0; }
    q += 6;
    char *e = strchr(q, '"');
    if (!e) { SDL_free(txt); return 0; }
    *e = 0;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/'), *bs = strrchr(dir, '\\');
    if (bs > slash) slash = bs;
    if (slash) slash[1] = 0; else dir[0] = 0;
    snprintf(out, cap, "%s%s", dir, q);
    if (!SDL_GetPathInfo(out, NULL)) {                 // try the other files in the folder, ignoring case
        int count = 0;
        char **names = SDL_GlobDirectory(dir[0] ? dir : ".", NULL, 0, &count);
        for (int k = 0; names && k < count; k++)
            if (!SDL_strcasecmp(names[k], q)) { snprintf(out, cap, "%s%s", dir, names[k]); break; }
        SDL_free(names);
    }
    SDL_free(txt);
    return 1;
}

static int open_disc(const char *image, struct disc *d, char *err, size_t errcap) {
    char path[1100];
    if (!resolve_image(image, path, sizeof path)) { snprintf(err, errcap, "cannot read the cue sheet"); return 0; }
    d->io = SDL_IOFromFile(path, "rb");
    if (!d->io) { snprintf(err, errcap, "cannot open %s", path); return 0; }
    uint8_t sync[12];
    Sint64 size = SDL_GetIOSize(d->io);
    static const uint8_t SYNC[12] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    if (SDL_ReadIO(d->io, sync, 12) == 12 && !memcmp(sync, SYNC, 12) && size % 2352 == 0) {
        d->sector_size = 2352; d->header = 16;
    } else {
        d->sector_size = 2048; d->header = 0;
    }
    uint8_t pvd[2048];
    if (!read_sectors(d, 16, 1, pvd) || memcmp(pvd + 1, "CD001", 5)) {
        snprintf(err, errcap, "this is not a CD image (no ISO 9660 file system)");
        SDL_CloseIO(d->io);
        return 0;
    }
    return 1;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

struct job {
    struct disc *d;
    void (*progress)(double, double, void *);
    void *user;
    double done, total;
    char *err; size_t errcap;
    int counting;
};

static int copy_file(struct job *j, uint32_t lba, uint32_t size, const char *dst) {
    SDL_IOStream *out = SDL_IOFromFile(dst, "wb");
    if (!out) { snprintf(j->err, j->errcap, "cannot write %s", dst); return 0; }
    static uint8_t buf[64 * 2048];
    uint32_t left = size;
    while (left) {
        uint32_t n = left > sizeof buf ? (uint32_t)sizeof buf : left;
        uint32_t secs = (n + 2047) / 2048;
        if (!read_sectors(j->d, lba, secs, buf)) { SDL_CloseIO(out); snprintf(j->err, j->errcap, "read error on the disc image"); return 0; }
        if (SDL_WriteIO(out, buf, n) != n) { SDL_CloseIO(out); snprintf(j->err, j->errcap, "cannot write %s (disk full?)", dst); return 0; }
        left -= n; lba += secs;
        j->done += n;
        if (j->progress) j->progress(j->done, j->total, j->user);
    }
    SDL_CloseIO(out);
    return 1;
}

// walk a directory; when counting, only add up file sizes
static int walk(struct job *j, uint32_t lba, uint32_t size, const char *dst, int depth) {
    if (depth > 8) return 1;
    uint32_t secs = (size + 2047) / 2048;
    uint8_t *data = malloc((size_t)secs * 2048);
    if (!data || !read_sectors(j->d, lba, secs, data)) { free(data); snprintf(j->err, j->errcap, "read error on the disc image"); return 0; }
    if (!j->counting) SDL_CreateDirectory(dst);
    int ok = 1;
    for (uint32_t i = 0; ok && i < secs * 2048u;) {
        uint8_t n = data[i];
        if (!n) { i = (i / 2048 + 1) * 2048; continue; }
        const uint8_t *rec = data + i;
        i += n;
        uint8_t nl = rec[32];
        if (nl == 1 && (rec[33] == 0 || rec[33] == 1)) continue;          // . and ..
        char name[256];
        memcpy(name, rec + 33, nl);
        name[nl] = 0;
        char *semi = strchr(name, ';');
        if (semi) *semi = 0;
        size_t L = strlen(name);
        if (L && name[L - 1] == '.') name[L - 1] = 0;
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dst, name);
        if (rec[25] & 2) ok = walk(j, le32(rec + 2), le32(rec + 10), path, depth + 1);
        else if (j->counting) j->total += le32(rec + 10);
        else ok = copy_file(j, le32(rec + 2), le32(rec + 10), path);
    }
    free(data);
    return ok;
}

static uint32_t crc32_file(const char *path, Sint64 *size) {
    size_t n;
    uint8_t *p = SDL_LoadFile(path, &n);
    if (!p) { *size = -1; return 0; }
    uint32_t c = 0xffffffffu;
    for (size_t k = 0; k < n; k++) {
        c ^= p[k];
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    SDL_free(p);
    *size = (Sint64)n;
    return ~c;
}

int install_from_disc(const char *image, void (*progress)(double, double, void *), void *user, char *err, size_t errcap) {
    struct disc d;
    if (!open_disc(image, &d, err, errcap)) return 1;
    uint8_t pvd[2048];
    read_sectors(&d, 16, 1, pvd);
    const uint8_t *root = pvd + 156;
    struct job j = { &d, progress, user, 0, 0, err, errcap, 1 };
    SDL_CreateDirectory(path_data());
    int ok = walk(&j, le32(root + 2), le32(root + 10), path_cd(), 0);    // count
    j.counting = 0;
    if (ok) ok = walk(&j, le32(root + 2), le32(root + 10), path_cd(), 0);
    SDL_CloseIO(d.io);
    if (!ok) return 1;
    // the program: verify it is the version this build was made from, then keep a copy
    char src[1200];
    snprintf(src, sizeof src, "%s/acc/persona/Program/Persona.exe", path_cd());
    Sint64 size;
    uint32_t crc = crc32_file(src, &size);
    if (size < 0) { snprintf(err, errcap, "this disc has no acc/persona/Program/Persona.exe - is it the Windows version?"); return 1; }
    if ((uint32_t)size != EXE_SIZE || crc != EXE_CRC32) {
        snprintf(err, errcap, "this disc's Persona.exe is a different version from the one this build supports");
        return 1;
    }
    if (!SDL_CopyFile(src, path_exe())) { snprintf(err, errcap, "cannot copy Persona.exe: %s", SDL_GetError()); return 1; }
    return 0;
}
