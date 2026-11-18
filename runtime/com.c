// COM objects implemented on the host. A guest object is { vtable pointer, host id, refcount };
// each vtable slot is a magic address that names "Interface::Method", so an unimplemented call
// stops with the method name.
#include "win32.h"
#include "com.h"
#include <stdlib.h>
#include <string.h>

uint32_t com_vtable(const char *iface, const char *const *names, int n, const struct com_method *impl) {
    uint32_t vt = hostobj_alloc(4u * (uint32_t)n);
    for (int k = 0; k < n; k++) {
        char *full = malloc(strlen(iface) + strlen(names[k]) + 3);
        sprintf(full, "%s::%s", iface, names[k]);
        api_fn fn = 0; int nargs = 1;
        for (const struct com_method *m = impl; m && m->name; m++)
            if (!strcmp(m->name, names[k])) { fn = m->fn; nargs = m->nargs; }
        WR32(vt + 4u * (uint32_t)k, fn ? api_magic(full, fn, nargs) : api_magic(full, com_missing, 1));
    }
    return vt;
}
uint32_t com_missing(const uint32_t *a) {
    (void)a;
    extern const char *host_current_api(void);
    rt_fatal("unimplemented COM method %s", host_current_api());
}

uint32_t com_new(uint32_t vtable, void *host) {
    uint32_t obj = hostobj_alloc(16);
    WR32(obj, vtable);
    WR32(obj + 8, 1);
    com_set_host(obj, host);
    return obj;
}

// host pointers are kept in a side table indexed by the id stored in the object
static void *host_ptrs[4096];
static uint32_t nhost = 1;
void com_set_host(uint32_t obj, void *host) {
    if (nhost == 4096) rt_fatal("too many COM objects");
    host_ptrs[nhost] = host;
    WR32(obj + 4, nhost++);
}
void *com_host(uint32_t obj) {
    uint32_t id = RD32(obj + 4);
    return id < nhost ? host_ptrs[id] : 0;
}

uint32_t com_addref(const uint32_t *a) { uint32_t n = RD32(a[0] + 8) + 1; WR32(a[0] + 8, n); return n; }
uint32_t com_release(const uint32_t *a) { uint32_t n = RD32(a[0] + 8); if (n) n--; WR32(a[0] + 8, n); return n; }

int guid_eq(uint32_t p, const char *s) {
    // s: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
    uint8_t g[16];
    unsigned d1, d2, d3, b[8];
    sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]);
    memcpy(g, &d1, 4); g[4] = d2 & 0xff; g[5] = d2 >> 8; g[6] = d3 & 0xff; g[7] = d3 >> 8;
    for (int k = 0; k < 8; k++) g[8 + k] = (uint8_t)b[k];
    return memcmp(MEM + p, g, 16) == 0;
}
void guid_str(uint32_t p, char *out) {
    sprintf(out, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", RD32(p), RD16(p + 4), RD16(p + 6),
            RD8(p + 8), RD8(p + 9), RD8(p + 10), RD8(p + 11), RD8(p + 12), RD8(p + 13), RD8(p + 14), RD8(p + 15));
}

uint32_t com_create_instance(uint32_t clsid, uint32_t iid, uint32_t out) {
    char c[40], i[40];
    guid_str(clsid, c); guid_str(iid, i);
    if (guid_eq(clsid, CLSID_FilterGraph)) return dshow_create(iid, out);
    fprintf(stderr, "CoCreateInstance: class %s interface %s not available\n", c, i);
    WR32(out, 0);
    return 0x80040154u;    // REGDB_E_CLASSNOTREG
}
