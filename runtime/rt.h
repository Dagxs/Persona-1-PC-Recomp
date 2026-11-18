// Runtime-internal declarations (not needed by the generated code).
#pragma once
#include "cpu.h"
#include "../gen/image_info.h"
#include <stdio.h>

// Magic guest addresses (outside guest memory) that stand for host functions:
// imported Win32 functions and the methods of host-implemented COM objects.
#define MAGIC_BASE 0xf0000000u
#define MAGIC_EXIT 0xffffffffu         // return address of the process entry point
#define MAGIC_THREAD_END 0xfffffffeu   // return address of a thread procedure
#define MAGIC_RET 0xfffffffdu          // return address of host -> guest callbacks
void host_invoke(uint32_t magic);

// Guest address-space layout (MEM_SIZE = 512 MB)
#define TEB_ADDR      0x00030000u      // thread environment blocks, 4 KB each
#define HOSTOBJ_LO    0x00100000u      // host-made guest objects: COM vtables, strings, structures
#define HOSTOBJ_HI    0x00400000u
#define HEAP_LO       0x01000000u      // HeapAlloc / VirtualAlloc
#define HEAP_HI       0x1e000000u
#define STACK_TOP     0x20000000u      // main thread stack grows down from here; thread stacks below

extern int rt_trace;
uint8_t *host_alloc_guest(uint32_t size);
void load_image(const char *path);
void english_boot(void);
guest_fn_t rt_lookup(uint32_t a);

// host-made guest memory
uint32_t hostobj_alloc(uint32_t size);           // permanent, zeroed
uint32_t hostobj_str(const char *s);             // permanent copy of a C string
uint32_t guest_heap_alloc(uint32_t size, int zero);
void guest_heap_free(uint32_t p);
uint32_t guest_heap_size(uint32_t p);
uint32_t guest_heap_realloc(uint32_t p, uint32_t size);
uint32_t guest_vm_reserve(uint32_t size);        // 64 KB aligned, zeroed

// Win32 layer
void win32_init(int argc, char **argv);
// register a host function; returns its magic address
typedef uint32_t (*api_fn)(const uint32_t *a);
uint32_t api_magic(const char *name, api_fn fn, int nargs);
static inline char *gstr(uint32_t p) { return p ? (char *)MEM + p : NULL; }
void gstrcpy(uint32_t dst, const char *src, uint32_t max);
extern uint32_t last_error;
