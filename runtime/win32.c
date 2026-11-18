// Win32 layer core: import binding, host-function invocation, process/thread environment.
#include "rt.h"
#include "win32.h"
#include <stdlib.h>
#include <string.h>

uint32_t last_error;
static const char *current_api;
const char *host_current_api(void) { return current_api; }

// ---------------------------------------------------------------- host functions
struct api { const char *name; api_fn fn; int nargs; };   // nargs < 0: cdecl (caller pops)
static struct api apis[4096];
static int napis;

uint32_t api_magic(const char *name, api_fn fn, int nargs) {
    if (napis == (int)(sizeof apis / sizeof apis[0])) rt_fatal("too many host functions");
    apis[napis].name = name; apis[napis].fn = fn; apis[napis].nargs = nargs;
    return MAGIC_BASE + (uint32_t)napis++;
}

void host_invoke(uint32_t magic) {
    if (magic == MAGIC_EXIT) rt_fatal("process entry point returned");
    uint32_t k = magic - MAGIC_BASE;
    if (k >= (uint32_t)napis) rt_fatal("call to bad magic address %08x", magic);
    struct api *a = &apis[k];
    uint32_t args[16];
    int n = a->nargs < 0 ? 16 : a->nargs;
    uint32_t ret_addr = RD32(ESP);
    for (int j = 0; j < n; j++) args[j] = RD32(ESP + 4 + 4 * j);
    uint32_t sp = ESP;
    current_api = a->name;
    uint32_t r = a->fn(args);
    if (rt_trace) {
        fprintf(stderr, "[%08x] %s(", ret_addr, a->name);
        for (int j = 0; j < (a->nargs < 0 ? 4 : a->nargs); j++) fprintf(stderr, j ? ", %x" : "%x", args[j]);
        fprintf(stderr, ") = %x\n", r);
    }
    EAX = r;
    // stdcall callee pops its arguments; cdecl leaves them to the caller
    ESP = sp + 4 + (a->nargs > 0 ? 4u * (uint32_t)a->nargs : 0u);
}

// ---------------------------------------------------------------- import table
extern const struct impl KERNEL32_IMPL[], THREAD_IMPL[], FILES_IMPL[], USER32_IMPL[], GDI32_IMPL[],
    ADVAPI32_IMPL[], WINMM_IMPL[], OLE32_IMPL[], DDRAW_IMPL[], QUARTZ_IMPL[];
static const struct impl *const ALL_IMPL[] = { KERNEL32_IMPL, THREAD_IMPL, FILES_IMPL, USER32_IMPL,
    GDI32_IMPL, ADVAPI32_IMPL, WINMM_IMPL, OLE32_IMPL, DDRAW_IMPL, QUARTZ_IMPL, 0 };

static uint32_t missing(const uint32_t *a) { (void)a; rt_fatal("unimplemented import %s", current_api); }

const struct impl *find_impl(const char *dll, const char *name) {
    for (int t = 0; ALL_IMPL[t]; t++)
        for (const struct impl *p = ALL_IMPL[t]; p->name; p++)
            if ((!dll || !strcasecmp_ascii(p->dll, dll)) && !strcmp(p->name, name)) return p;
    return 0;
}

// Unimplemented imports get their own magic address so the fatal message names them.
static uint32_t missing_stub(const char *dll, const char *name) {
    char *buf = malloc(strlen(dll) + strlen(name) + 2);
    sprintf(buf, "%s!%s", dll, name);
    return api_magic(buf, missing, 0);
}

uint32_t resolve_import(const char *dll, const char *name) {
    const struct impl *p = find_impl(dll, name);
    if (p) return api_magic(p->name, p->fn, p->nargs);
    return missing_stub(dll, name);
}

static void bind_imports(void) {
    int nmiss = 0;
    for (int k = 0; k < GUEST_NIMPORTS; k++) {
        const struct guest_import *g = &GUEST_IMPORTS[k];
        uint32_t m = resolve_import(g->dll, g->name);
        WR32(g->slot, m);
        if (!find_impl(g->dll, g->name)) nmiss++;
    }
    if (rt_trace) fprintf(stderr, "imports: %d bound, %d not implemented yet\n", GUEST_NIMPORTS - nmiss, nmiss);
}

int strcasecmp_ascii(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y || !x) return x - y;
    }
}

// ---------------------------------------------------------------- process environment
uint32_t g_cmdline, g_module_path;

void win32_init(int argc, char **argv) {
    (void)argc; (void)argv;
    bind_imports();
    g_module_path = hostobj_str("C:\\PERSONA\\PERSONA.EXE");
    g_cmdline = hostobj_str("\"C:\\PERSONA\\PERSONA.EXE\"");
    thread_init_main();
    files_init();
}
