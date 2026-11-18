// Guest memory: the flat address space, PE loading, and the guest heap.
#include "rt.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

uint8_t *host_alloc_guest(uint32_t size) {
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

// ---------------------------------------------------------------- PE image
static uint32_t crc32_of(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t k = 0; k < n; k++) {
        c ^= p[k];
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s (copy Persona.exe from the disc's acc/persona/Program folder)", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) rt_fatal("short read on %s", path);
    fclose(f);
    if ((uint32_t)n != EXE_SIZE || crc32_of(buf, n) != EXE_CRC32)
        rt_fatal("%s is not the 1999 Japanese Persona.exe this build was generated from", path);
    uint32_t pe = *(uint32_t *)(buf + 0x3c);
    uint16_t nsec = *(uint16_t *)(buf + pe + 6);
    uint16_t optsz = *(uint16_t *)(buf + pe + 20);
    uint32_t hdrsz = *(uint32_t *)(buf + pe + 24 + 60);
    memcpy(MEM + IMAGE_BASE, buf, hdrsz);
    uint8_t *sec = buf + pe + 24 + optsz;
    for (int k = 0; k < nsec; k++, sec += 40) {
        uint32_t va = *(uint32_t *)(sec + 12), rawsz = *(uint32_t *)(sec + 16), rawoff = *(uint32_t *)(sec + 20);
        uint32_t vsz = *(uint32_t *)(sec + 8);
        uint32_t cp = rawsz < vsz || !vsz ? rawsz : vsz;
        memcpy(MEM + IMAGE_BASE + va, buf + rawoff, cp);
    }
    free(buf);
}

// ---------------------------------------------------------------- host objects
static uint32_t hostobj_top = HOSTOBJ_LO;
uint32_t hostobj_alloc(uint32_t size) {
    uint32_t p = (hostobj_top + 15u) & ~15u;
    if (p + size > HOSTOBJ_HI) rt_fatal("host object area exhausted");
    hostobj_top = p + size;
    memset(MEM + p, 0, size);
    return p;
}
uint32_t hostobj_str(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, p = hostobj_alloc(n);
    memcpy(MEM + p, s, n);
    return p;
}
void gstrcpy(uint32_t dst, const char *src, uint32_t max) {
    if (!max) return;
    uint32_t n = (uint32_t)strlen(src);
    if (n >= max) n = max - 1;
    memcpy(MEM + dst, src, n);
    MEM[dst + n] = 0;
}

// ---------------------------------------------------------------- guest heap
// Size-class free lists over a bump region. Each block has an 8-byte header holding its
// capacity; the payload is 8-byte aligned. VirtualAlloc reservations share the same region.
#define NCLASS 40
static uint32_t heap_top = HEAP_LO;
static uint32_t free_list[NCLASS];

static int size_class(uint32_t n) {           // capacities 16, 24, 32, 48, 64, 96, ...
    uint32_t cap = 16; int c = 0;
    while (cap < n) { cap = (c & 1) ? cap / 3 * 4 : cap * 3 / 2; c++; }
    return c;
}
static uint32_t class_cap(int c) {
    uint32_t cap = 16;
    for (int k = 0; k < c; k++) cap = (k & 1) ? cap / 3 * 4 : cap * 3 / 2;
    return cap;
}

static uint32_t bump(uint32_t size, uint32_t align) {
    uint32_t p = (heap_top + align - 1) & ~(align - 1);
    if (p + size > HEAP_HI || p + size < p) rt_fatal("guest heap exhausted (%u bytes requested)", size);
    heap_top = p + size;
    return p;
}

uint32_t guest_heap_alloc(uint32_t size, int zero) {
    if (!size) size = 1;
    int c = size_class(size);
    uint32_t cap = class_cap(c), p;
    if (c < NCLASS && free_list[c]) {
        p = free_list[c];
        free_list[c] = RD32(p);
    } else {
        p = bump(cap + 8, 8) + 8;
        WR32(p - 8, cap);
        WR32(p - 4, 0x48454150u);                  // 'HEAP'
    }
    if (zero) memset(MEM + p, 0, cap);
    return p;
}
void guest_heap_free(uint32_t p) {
    if (!p) return;
    if (RD32(p - 4) != 0x48454150u) { if (rt_trace) fprintf(stderr, "heap: free of non-heap pointer %08x\n", p); return; }
    int c = size_class(RD32(p - 8));
    if (c >= NCLASS) return;                        // huge blocks are not recycled
    WR32(p, free_list[c]);
    free_list[c] = p;
}
uint32_t guest_heap_size(uint32_t p) { return p ? RD32(p - 8) : 0; }
uint32_t guest_heap_realloc(uint32_t p, uint32_t size) {
    if (!p) return guest_heap_alloc(size, 0);
    uint32_t cap = RD32(p - 8);
    if (size <= cap) return p;
    uint32_t q = guest_heap_alloc(size, 0);
    memcpy(MEM + q, MEM + p, cap);
    guest_heap_free(p);
    return q;
}
uint32_t guest_vm_reserve(uint32_t size) {
    size = (size + 0xffffu) & ~0xffffu;
    uint32_t p = bump(size, 0x10000u);
    memset(MEM + p, 0, size);
    return p;
}
