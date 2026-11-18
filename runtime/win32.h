// Shared declarations for the Win32 implementation files.
#pragma once
#include "rt.h"

struct impl { const char *dll, *name; api_fn fn; int nargs; };
#define API(dll, name, n) { dll, #name, w_##name, n }
#define DEF(name) static uint32_t w_##name(const uint32_t *a)
#define UNUSED_ARGS (void)a

int strcasecmp_ascii(const char *a, const char *b);
uint32_t resolve_import(const char *dll, const char *name);
const struct impl *find_impl(const char *dll, const char *name);
extern uint32_t g_cmdline, g_module_path;

// threads (thread.c)
void thread_init_main(void);
uint32_t thread_current_id(void);
uint32_t thread_teb(void);
void thread_cs_enter(uint32_t cs);
void thread_cs_leave(uint32_t cs);

// files (files.c)
void files_init(void);

// handles: small integers tagged by kind
enum { H_FREE, H_FILE, H_EVENT, H_THREAD, H_HEAP, H_FIND, H_MODULE, H_KEY };
uint32_t handle_new(int kind, void *obj);
void *handle_get(uint32_t h, int kind);
void handle_close(uint32_t h);

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_INVALID_HANDLE 6
#define ERROR_NO_MORE_FILES 18
#define ERROR_INVALID_PARAMETER 87
#define ERROR_CALL_NOT_IMPLEMENTED 120
#define ERROR_ALREADY_EXISTS 183
#define INVALID_HANDLE 0xffffffffu
