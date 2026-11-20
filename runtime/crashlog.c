// A hard crash (an access violation or the like in the runtime or the generated code) writes
// crash.log beside the game, like rt_fatal does for the errors it catches: the exception, where in
// persona1.exe it happened, and the guest function and registers of the running game thread.
// Every thread keeps some stack in reserve so that even a stack overflow gets its log, and the
// console's messages are also written to game.log (the console closes with the game).
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <io.h>
#include <fcntl.h>

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

// every thread: stack kept back for the handler should the thread overflow its own
void rt_crash_thread(void) {
    ULONG keep = 256 * 1024;
    SetThreadStackGuarantee(&keep);
}

// stderr -> the console as before and game.log, through a pipe and a copying thread
static HANDLE console, logfile;
static DWORD WINAPI tee(LPVOID pipe) {
    char buf[4096];
    DWORD n, w;
    while (ReadFile((HANDLE)pipe, buf, sizeof buf, &n, NULL) && n) {
        WriteFile(console, buf, n, &w, NULL);
        WriteFile(logfile, buf, n, &w, NULL);
    }
    return 0;
}

static void tee_stderr(void) {
    console = GetStdHandle(STD_ERROR_HANDLE);
    if (GetFileType(console) != FILE_TYPE_CHAR) return;          // already going to a file (tests)
    logfile = CreateFileA("game.log", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (logfile == INVALID_HANDLE_VALUE) return;
    HANDLE r, w;
    if (!CreatePipe(&r, &w, NULL, 0)) return;
    int fd = _open_osfhandle((intptr_t)w, _O_WRONLY | _O_TEXT);
    if (fd < 0 || _dup2(fd, _fileno(stderr)) < 0) return;
    setvbuf(stderr, NULL, _IONBF, 0);
    CloseHandle(CreateThread(NULL, 0, tee, r, 0, NULL));
}

void rt_crash_install(void) {
    SetUnhandledExceptionFilter(on_crash);
    rt_crash_thread();
    tee_stderr();
}
#else
void rt_crash_install(void) {}
void rt_crash_thread(void) {}
#endif
