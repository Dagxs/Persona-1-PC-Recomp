// A hard crash (an access violation or the like in the runtime or the generated code) writes
// crash.log beside the game, like rt_fatal does for the errors it catches: the exception, where in
// persona1.exe it happened, and the guest function and registers of the running game thread.
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

void rt_guest_state(FILE *f);       // rt.c

static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    FILE *f = fopen("crash.log", "w");
    if (f) {
        EXCEPTION_RECORD *r = ep->ExceptionRecord;
        HMODULE self = GetModuleHandleA(NULL);
        uintptr_t at = (uintptr_t)r->ExceptionAddress;
        fprintf(f, "CRASH: exception %08lx at persona1.exe+%llx", (unsigned long)r->ExceptionCode,
                (unsigned long long)(at - (uintptr_t)self));
        if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
            fprintf(f, " (%s %llx)", r->ExceptionInformation[0] ? "writing" : "reading",
                    (unsigned long long)r->ExceptionInformation[1]);
        fprintf(f, "\n");
        rt_guest_state(f);
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void rt_crash_install(void) { SetUnhandledExceptionFilter(on_crash); }
#else
void rt_crash_install(void) {}
#endif
