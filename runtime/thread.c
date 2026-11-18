// Guest threads: one host thread per guest thread, but only one runs guest code at a time.
// The running thread owns the CPU state (globals in cpu.h); a hand-off saves it into the
// thread record and loads the next thread's. Hand-offs happen at loop back-edges (POLL),
// at blocking API calls, and when a thread ends.
#include "win32.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

struct cpu_state {
    uint32_t r[8], df, fs, fk, fsz, fa, fb, fr, fc, fraw;
    double fpr[8];
    uint32_t top, c0, c1, c2, c3, cw;
};

struct gthread {
    uint32_t id, teb, handle;
    struct cpu_state st;
    SDL_Condition *turn;
    int finished, terminate;
    uint32_t entry, param, stack_top;
    struct gthread *next_wait;      // run queue link
};

struct gevent { int manual, signaled; };

static SDL_Mutex *gil;
static struct gthread *running;
static struct gthread *queue_head, *queue_tail;
static struct gthread *threads[64];
static int nthreads;
static uint32_t next_id = 0x100;
static uint32_t next_stack = STACK_TOP - 0x200000u;   // main thread: top 2 MB

static void save(struct cpu_state *s) {
    memcpy(s->r, R, sizeof R); s->df = DF; s->fs = FS_BASE;
    s->fk = fk; s->fsz = fsz; s->fa = fa; s->fb = fb; s->fr = fr; s->fc = fc; s->fraw = fraw;
    memcpy(s->fpr, FPR, sizeof FPR);
    s->top = FPU_TOP; s->c0 = FPU_C0; s->c1 = FPU_C1; s->c2 = FPU_C2; s->c3 = FPU_C3; s->cw = FPU_CW;
}
static void load(const struct cpu_state *s) {
    memcpy(R, s->r, sizeof R); DF = s->df; FS_BASE = s->fs;
    fk = s->fk; fsz = s->fsz; fa = s->fa; fb = s->fb; fr = s->fr; fc = s->fc; fraw = s->fraw;
    memcpy(FPR, s->fpr, sizeof FPR);
    FPU_TOP = s->top; FPU_C0 = s->c0; FPU_C1 = s->c1; FPU_C2 = s->c2; FPU_C3 = s->c3; FPU_CW = s->cw;
}

static uint32_t make_teb(uint32_t stack_top, uint32_t stack_size, uint32_t id) {
    uint32_t teb = TEB_ADDR + 0x1000u * (uint32_t)nthreads;
    memset(MEM + teb, 0, 0x1000);
    WR32(teb + 0x00, 0xffffffffu);        // SEH chain: end
    WR32(teb + 0x04, stack_top);
    WR32(teb + 0x08, stack_top - stack_size);
    WR32(teb + 0x18, teb);                // self
    WR32(teb + 0x24, id);
    WR32(teb + 0x2c, teb + 0xe10u);       // TLS slots live inside the TEB
    return teb;
}

static void enqueue(struct gthread *t) {
    t->next_wait = 0;
    if (queue_tail) queue_tail->next_wait = t; else queue_head = t;
    queue_tail = t;
    rt_switch_pending = 1;
}

// Hand the CPU to the next queued thread; the caller must already be queued (or finished).
static void switch_away(struct gthread *self) {
    struct gthread *nx = queue_head;
    if (!nx) rt_fatal("thread switch with an empty run queue");
    queue_head = nx->next_wait;
    if (!queue_head) queue_tail = 0;
    rt_switch_pending = queue_head != 0;
    if (self) save(&self->st);
    running = nx;
    SDL_SignalCondition(nx->turn);
    if (!self) return;
    while (running != self) SDL_WaitCondition(self->turn, gil);
    load(&self->st);
}

void rt_yield(void) {
    if (!queue_head) return;
    struct gthread *self = running;
    enqueue(self);
    switch_away(self);
    if (self->terminate) { self->finished = 1; SDL_UnlockMutex(gil); for (;;) SDL_Delay(1000); }
}

// Block until cond(arg) holds, letting other threads run meanwhile.
static void wait_until(int (*cond)(void *), void *arg, uint32_t timeout_ms, int *timed_out) {
    uint64_t t0 = SDL_GetTicks();
    *timed_out = 0;
    while (!cond(arg)) {
        if (timeout_ms != 0xffffffffu && SDL_GetTicks() - t0 >= timeout_ms) { *timed_out = 1; return; }
        if (queue_head) rt_yield();
        else { SDL_UnlockMutex(gil); SDL_Delay(1); SDL_LockMutex(gil); }
    }
}

uint32_t thread_current_id(void) { return running->id; }
uint32_t thread_teb(void) { return running->teb; }

void thread_init_main(void) {
    gil = SDL_CreateMutex();
    SDL_LockMutex(gil);
    struct gthread *t = calloc(1, sizeof *t);
    t->id = next_id++;
    t->turn = SDL_CreateCondition();
    t->teb = make_teb(STACK_TOP, 0x200000u, t->id);
    threads[nthreads++] = t;
    running = t;
    ESP = STACK_TOP - 16;
    FS_BASE = t->teb;
}

static int thread_main(void *arg) {
    struct gthread *t = arg;
    SDL_LockMutex(gil);
    while (running != t) SDL_WaitCondition(t->turn, gil);
    load(&t->st);
    PUSH32(t->param);
    rt_call(t->entry, MAGIC_THREAD_END);      // the thread procedure returns to MAGIC_THREAD_END
    t->finished = 1;
    if (rt_trace) fprintf(stderr, "thread %x finished\n", t->id);
    if (queue_head) switch_away(0);
    else running = 0;
    SDL_UnlockMutex(gil);
    return 0;
}

// ---------------------------------------------------------------- API
DEF(CreateThread) {
    if (nthreads == 64) rt_fatal("too many guest threads");
    struct gthread *t = calloc(1, sizeof *t);
    uint32_t size = a[1] ? (a[1] + 0xffffu) & ~0xffffu : 0x100000u;
    t->id = next_id++;
    t->turn = SDL_CreateCondition();
    t->stack_top = next_stack;
    next_stack -= size + 0x10000u;
    t->teb = make_teb(t->stack_top, size, t->id);
    t->entry = a[2];
    t->param = a[3];
    memset(&t->st, 0, sizeof t->st);
    t->st.r[4] = t->stack_top - 16;
    t->st.fs = t->teb;
    t->st.cw = 0x027f;
    t->st.fk = K_LOGIC; t->st.fsz = 32;
    threads[nthreads++] = t;
    t->handle = handle_new(H_THREAD, t);
    if (a[5]) WR32(a[5], t->id);
    if (rt_trace) fprintf(stderr, "CreateThread entry=%08x param=%08x -> id %x\n", a[2], a[3], t->id);
    if (a[4] & 4u) rt_fatal("CreateThread: CREATE_SUSPENDED is not supported");
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)thread_main);
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, t);
    SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "guest");
    SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 64 << 20);
    SDL_Thread *h = SDL_CreateThreadWithProperties(props);
    SDL_DestroyProperties(props);
    if (!h) rt_fatal("cannot create host thread: %s", SDL_GetError());
    SDL_DetachThread(h);
    enqueue(t);
    return t->handle;
}
DEF(TerminateThread) {
    struct gthread *t = handle_get(a[0], H_THREAD);
    if (!t) return 0;
    if (t == running) rt_fatal("TerminateThread on the current thread");
    t->terminate = 1;     // it stops at its next hand-off point
    return 1;
}

DEF(CreateEventA) {
    struct gevent *e = calloc(1, sizeof *e);
    e->manual = a[1] != 0;
    e->signaled = a[2] != 0;
    return handle_new(H_EVENT, e);
}
DEF(SetEvent) {
    struct gevent *e = handle_get(a[0], H_EVENT);
    if (!e) return 0;
    e->signaled = 1;
    return 1;
}
static int event_set(void *p) { return ((struct gevent *)p)->signaled; }
static int thread_done(void *p) { return ((struct gthread *)p)->finished; }
DEF(WaitForSingleObject) {
    int to;
    struct gevent *e = handle_get(a[0], H_EVENT);
    if (e) {
        wait_until(event_set, e, a[1], &to);
        if (to) return 0x102u;            // WAIT_TIMEOUT
        if (!e->manual) e->signaled = 0;
        return 0;
    }
    struct gthread *t = handle_get(a[0], H_THREAD);
    if (t) {
        wait_until(thread_done, t, a[1], &to);
        return to ? 0x102u : 0;
    }
    rt_fatal("WaitForSingleObject on unsupported handle %x", a[0]);
}

// critical sections: offset 8 = recursion count, 12 = owner thread id
void thread_cs_enter(uint32_t cs) {
    while (RD32(cs + 12) && RD32(cs + 12) != running->id) {
        if (queue_head) rt_yield();
        else rt_fatal("deadlock: critical section %08x held by thread %x", cs, RD32(cs + 12));
    }
    WR32(cs + 12, running->id);
    WR32(cs + 8, RD32(cs + 8) + 1);
}
void thread_cs_leave(uint32_t cs) {
    uint32_t n = RD32(cs + 8);
    if (n) n--;
    WR32(cs + 8, n);
    if (!n) WR32(cs + 12, 0);
}

int thread_close(uint32_t h) {
    if (handle_get(h, H_EVENT)) { free(handle_get(h, H_EVENT)); handle_close(h); return 1; }
    if (handle_get(h, H_THREAD)) { handle_close(h); return 1; }
    return 0;
}

const struct impl THREAD_IMPL[] = {
#define K(name, n) API("KERNEL32.DLL", name, n)
    K(CreateThread, 6), K(TerminateThread, 2), K(CreateEventA, 4), K(SetEvent, 1), K(WaitForSingleObject, 2),
#undef K
    { 0 }
};
