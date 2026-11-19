// English for the messages the PS1 overlays (ADV, DNG, S2D) keep in their own data, which the
// Windows port compiled into Persona.exe (data/en/msg_text.bin, tools/install_english.py
// msg_text()): when one of the three dialogue interpreters reads the first byte of such a string
// (PC 0x478f42 / 0x420c05 / 0x4e1a45, edi = the read pointer), it reads the English instead.
#include "rt.h"
#include "paths.h"
#include <stdlib.h>
#include <string.h>

static uint32_t *from, *to;     // sorted Persona.exe addresses -> English in guest memory
static int nmsg;

void msg_text_load(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/msg_text.bin", path_en());
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t hdr[8];
    uint32_t n = 0;
    if (fread(hdr, 1, 8, f) == 8 && !memcmp(hdr, "P1MS", 4)) memcpy(&n, hdr + 4, 4);
    if (n > 8192) n = 0;
    from = calloc(n ? n : 1, 4);
    to = calloc(n ? n : 1, 4);
    static uint8_t b[4096];
    for (uint32_t k = 0; from && to && k < n; k++) {
        uint32_t h[2];
        if (fread(h, 4, 2, f) != 2 || h[1] > sizeof b || fread(b, 1, h[1], f) != h[1]) break;
        uint32_t g = hostobj_alloc(h[1] + 1);
        for (uint32_t i = 0; i < h[1]; i++) WR8(g + i, b[i]);
        from[nmsg] = h[0]; to[nmsg] = g; nmsg++;
    }
    fclose(f);
    if (rt_trace) fprintf(stderr, "overlay messages: %d\n", nmsg);
}

uint32_t msg_text(uint32_t p) {
    int lo = 0, hi = nmsg - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (from[mid] == p) return to[mid];
        if (from[mid] < p) lo = mid + 1; else hi = mid - 1;
    }
    return p;
}
