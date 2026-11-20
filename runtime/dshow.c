// DirectShow, as far as Persona uses it.
//  * Sound: every BGM and sound effect is an MP3 (SND\*.SEQ) played through its own filter graph
//    (IGraphBuilder::RenderFile + IMediaControl / IMediaEventEx / IMediaPosition / IBasicAudio).
//    The whole file is decoded with minimp3 when it is rendered and played on an SDL audio stream.
//  * Movies (STR*\*.STR, MPEG-1 system streams) go through IAMMultiMediaStream: see movie.c.
#include "com.h"
#include "host.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define MINIMP3_IMPLEMENTATION
#include "../third_party/minimp3/minimp3.h"

#define CLSID_AMMultiMediaStream "49c47ce5-9ba4-11d0-8212-00c04fc32c45"
#define IID_IGraphBuilder  "56a868a9-0ad4-11ce-b03a-0020af0ba770"
#define IID_IFilterGraph   "56a8689f-0ad4-11ce-b03a-0020af0ba770"
#define IID_IMediaControl  "56a868b1-0ad4-11ce-b03a-0020af0ba770"
#define IID_IMediaEvent    "56a868b6-0ad4-11ce-b03a-0020af0ba770"
#define IID_IMediaEventEx  "56a868c0-0ad4-11ce-b03a-0020af0ba770"
#define IID_IMediaPosition "56a868b2-0ad4-11ce-b03a-0020af0ba770"
#define IID_IBasicAudio    "56a868b3-0ad4-11ce-b03a-0020af0ba770"
#define EC_COMPLETE 1
#define E_ABORT 0x80004004u

static const char *const GB_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "AddFilter", "RemoveFilter", "EnumFilters", "FindFilterByName",
    "ConnectDirect", "Reconnect", "Disconnect", "SetDefaultSyncSource", "Connect", "Render", "RenderFile",
    "AddSourceFilter", "SetLogFile", "Abort", "ShouldOperationContinue" };
#define DISPATCH "QueryInterface", "AddRef", "Release", "GetTypeInfoCount", "GetTypeInfo", "GetIDsOfNames", "Invoke"
static const char *const MC_METHODS[] = { DISPATCH, "Run", "Pause", "Stop", "GetState", "RenderFile",
    "AddSourceFilter", "get_FilterCollection", "get_RegFilterCollection", "StopWhenReady" };
static const char *const ME_METHODS[] = { DISPATCH, "GetEventHandle", "GetEvent", "WaitForCompletion",
    "CancelDefaultHandling", "RestoreDefaultHandling", "FreeEventParams", "SetNotifyWindow", "SetNotifyFlags",
    "GetNotifyFlags" };
static const char *const MP_METHODS[] = { DISPATCH, "get_Duration", "put_CurrentPosition", "get_CurrentPosition",
    "get_StopTime", "put_StopTime", "get_PrerollTime", "put_PrerollTime", "put_Rate", "get_Rate",
    "CanSeekForward", "CanSeekBackward" };
static const char *const BA_METHODS[] = { DISPATCH, "put_Volume", "get_Volume", "put_Balance", "get_Balance" };
#define N(x) ((int)(sizeof x / sizeof x[0]))

enum { ST_STOPPED, ST_PAUSED, ST_RUNNING };

struct graph {
    uint32_t obj_gb, obj_mc, obj_me, obj_mp, obj_ba;
    int16_t *pcm;               // interleaved
    int channels, rate;
    uint64_t frames;
    int state;
    uint64_t start_frame;       // frame the stream was (re)filled from
    uint64_t pos_frame;         // position while not running
    uint64_t run_ms;            // without an audio stream (no audio device, tests): when it started
    SDL_AudioStream *stream;
    uint32_t event_handle;      // manual-reset event: signalled while an event is pending
    int complete_pending, complete_sent;
    int32_t volume;
    char name[64];
};

static uint32_t vt_gb, vt_mc, vt_me, vt_mp, vt_ba;
static struct graph *graphs[256];
static int ngraphs;

static struct graph *G(uint32_t obj) { return com_host(obj); }

static void wide_to_guest_path(uint32_t w, char *out, size_t cap) {
    size_t k = 0;
    for (uint32_t c; (c = RD16(w + 2u * k)) && k + 1 < cap; k++) out[k] = (char)c;
    out[k] = 0;
}

static uint64_t position(struct graph *g) {
    if (g->state != ST_RUNNING) return g->pos_frame;
    if (!g->stream) {                       // silent: the music still takes its time
        uint64_t p = g->start_frame + (SDL_GetTicks() - g->run_ms) * (uint64_t)g->rate / 1000u;
        return p > g->frames ? g->frames : p;
    }
    int queued = SDL_GetAudioStreamQueued(g->stream);
    uint64_t q = queued > 0 ? (uint64_t)queued / (2u * (uint32_t)g->channels) : 0;
    uint64_t p = g->frames - (q > g->frames - g->start_frame ? g->frames - g->start_frame : q);
    return p > g->frames ? g->frames : p;
}

// called from the poll points: raises EC_COMPLETE when a running graph has played everything
static void update(struct graph *g) {
    if (g->state == ST_RUNNING && !g->complete_sent && position(g) >= g->frames) {
        g->complete_pending = 1;
        g->complete_sent = 1;
        thread_event_set(g->event_handle, 1);
    }
}

static void refill(struct graph *g) {
    g->start_frame = g->pos_frame;
    g->run_ms = SDL_GetTicks();
    if (!g->stream) return;
    SDL_ClearAudioStream(g->stream);
    if (g->pos_frame < g->frames)
        SDL_PutAudioStreamData(g->stream, g->pcm + g->pos_frame * (uint64_t)g->channels,
                               (int)((g->frames - g->pos_frame) * 2u * (uint64_t)g->channels));
    SDL_FlushAudioStream(g->stream);
    g->start_frame = g->pos_frame;
}

// ---------------------------------------------------------------- IGraphBuilder
static uint32_t gb_QueryInterface(const uint32_t *a) {
    struct graph *g = G(a[0]);
    uint32_t r = 0;
    if (guid_eq(a[1], IID_IGraphBuilder) || guid_eq(a[1], IID_IFilterGraph)) r = g->obj_gb;
    else if (guid_eq(a[1], IID_IMediaControl)) r = g->obj_mc;
    else if (guid_eq(a[1], IID_IMediaEventEx) || guid_eq(a[1], IID_IMediaEvent)) r = g->obj_me;
    else if (guid_eq(a[1], IID_IMediaPosition)) r = g->obj_mp;
    else if (guid_eq(a[1], IID_IBasicAudio)) r = g->obj_ba;
    if (!r) { char s[40]; guid_str(a[1], s); fprintf(stderr, "filter graph: no interface %s\n", s); }
    WR32(a[2], r);
    return r ? DD_OK : E_NOINTERFACE;
}
static uint32_t gb_RenderFile(const uint32_t *a) {
    struct graph *g = G(a[0]);
    char path[260], host[600];
    wide_to_guest_path(a[1], path, sizeof path);
    if (!files_map(path, host, sizeof host)) return 0x80070002u;
    snprintf(g->name, sizeof g->name, "%s", strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path);
    FILE *f = fopen(host, "rb");
    if (!f) { if (rt_trace) fprintf(stderr, "RenderFile %s: not found\n", host); return 0x80070002u; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    fread(buf, 1, (size_t)n, f);
    fclose(f);
    mp3dec_t dec;
    mp3dec_init(&dec);
    size_t cap = 1u << 20, used = 0;
    int16_t *pcm = malloc(cap * sizeof *pcm);
    int16_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
    long off = 0;
    g->channels = 0;
    // a few sound files (SND/CSR04_*.SEQ) are 16-bit PCM WAV, not MP3
    if (n >= 12 && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WAVE", 4)) {
        int ch = 0, rate = 0, bits = 0, fmt = 0;
        for (long c = 12; c + 8 <= n;) {
            uint32_t len = (uint32_t)(buf[c + 4] | buf[c + 5] << 8 | buf[c + 6] << 16 | (uint32_t)buf[c + 7] << 24);
            long body = c + 8;
            if (len > (uint32_t)(n - body)) len = (uint32_t)(n - body);
            if (!memcmp(buf + c, "fmt ", 4) && len >= 16) {
                fmt = buf[body] | buf[body + 1] << 8;
                ch = buf[body + 2] | buf[body + 3] << 8;
                rate = buf[body + 4] | buf[body + 5] << 8 | buf[body + 6] << 16 | buf[body + 7] << 24;
                bits = buf[body + 14] | buf[body + 15] << 8;
            } else if (!memcmp(buf + c, "data", 4) && fmt == 1 && bits == 16 && ch >= 1 && ch <= 2 && rate > 0) {
                size_t samples = len / 2;
                if (samples > cap) { cap = samples; pcm = realloc(pcm, cap * sizeof *pcm); }
                memcpy(pcm, buf + body, samples * 2);
                used = samples - samples % (size_t)ch;
                g->channels = ch; g->rate = rate;
                break;
            }
            c = body + (long)len + (len & 1);
        }
        off = n;                                    // not MP3
    }
    while (off < n) {
        mp3dec_frame_info_t info;
        int s = mp3dec_decode_frame(&dec, buf + off, (int)(n - off), frame, &info);
        if (!info.frame_bytes) break;
        off += info.frame_bytes;
        if (!s) continue;
        if (!g->channels) { g->channels = info.channels; g->rate = info.hz; }
        size_t add = (size_t)s * (size_t)info.channels;
        if (info.channels != g->channels) continue;
        if (used + add > cap) { cap *= 2; pcm = realloc(pcm, cap * sizeof *pcm); }
        memcpy(pcm + used, frame, add * sizeof *pcm);
        used += add;
    }
    free(buf);
    if (!g->channels) { free(pcm); fprintf(stderr, "RenderFile %s: no MP3 or WAV audio\n", path); return 0x80040241u; }
    free(g->pcm);
    g->pcm = pcm;
    g->frames = used / (size_t)g->channels;
    g->pos_frame = 0;
    if (g->stream) SDL_DestroyAudioStream(g->stream);
    g->stream = host_audio_stream(g->channels, g->rate);
    if (g->stream) SDL_SetAudioStreamGain(g->stream, powf(10.0f, (float)g->volume / 2000.0f));
    if (rt_trace) fprintf(stderr, "RenderFile %s: %d Hz, %d ch, %.2f s\n", path, g->rate, g->channels, (double)g->frames / g->rate);
    return DD_OK;
}

// ---------------------------------------------------------------- IMediaControl
static int dslog = -1;
#define DSLOG(...) do { if (dslog < 0) dslog = getenv("P1_DSLOG") != NULL; if (dslog) fprintf(stderr, __VA_ARGS__); } while (0)
static uint32_t mc_Run(const uint32_t *a) {
    struct graph *g = G(a[0]);
    DSLOG("ds %s Run state=%d pos=%llu/%llu\n", g->name, g->state, (unsigned long long)position(g), (unsigned long long)g->frames);
    if (g->state != ST_RUNNING && g->rate) {
        refill(g);
        g->state = ST_RUNNING;
        g->complete_sent = 0;
    }
    return DD_OK;
}
static uint32_t mc_Pause(const uint32_t *a) {
    struct graph *g = G(a[0]);
    g->pos_frame = position(g);
    if (g->stream) SDL_ClearAudioStream(g->stream);
    g->state = ST_PAUSED;
    return DD_OK;
}
static uint32_t mc_Stop(const uint32_t *a) {
    struct graph *g = G(a[0]);
    DSLOG("ds %s Stop\n", g->name);
    g->pos_frame = position(g);
    if (g->stream) SDL_ClearAudioStream(g->stream);
    g->state = ST_STOPPED;
    return DD_OK;
}
static uint32_t mc_GetState(const uint32_t *a) { WR32(a[2], (uint32_t)G(a[0])->state); return DD_OK; }

// ---------------------------------------------------------------- IMediaEventEx
static uint32_t me_GetEventHandle(const uint32_t *a) { WR32(a[1], G(a[0])->event_handle); return DD_OK; }
static uint32_t me_GetEvent(const uint32_t *a) {
    struct graph *g = G(a[0]);
    update(g);
    if (!g->complete_pending) { rt_yield(); return E_ABORT; }
    g->complete_pending = 0;
    thread_event_set(g->event_handle, 0);
    WR32(a[1], EC_COMPLETE); WR32(a[2], 0); WR32(a[3], 0);
    return DD_OK;
}
static int finished(void *p) {
    struct graph *g = p;
    if (g->state != ST_RUNNING) return 1;          // stopped from another thread: stop waiting
    update(g);
    return g->complete_sent;
}
// Blocks (letting other guest threads run) until the graph has played to the end.
static uint32_t me_WaitForCompletion(const uint32_t *a) {
    struct graph *g = G(a[0]);
    DSLOG("ds %s Wait enter state=%d\n", g->name, g->state);
    int done = g->state == ST_RUNNING ? thread_wait(finished, g, a[1]) : 1;
    DSLOG("ds %s Wait done=%d sent=%d\n", g->name, done, g->complete_sent);
    if (done && g->complete_sent) {
        g->complete_pending = 0;
        thread_event_set(g->event_handle, 0);
        WR32(a[2], EC_COMPLETE);
        return DD_OK;
    }
    WR32(a[2], 0);
    return E_ABORT;
}
static uint32_t me_ok(const uint32_t *a) { (void)a; return DD_OK; }

// ---------------------------------------------------------------- IMediaPosition (REFTIME = double seconds)
static double arg_double(const uint32_t *a, int k) { uint64_t v = (uint64_t)a[k] | ((uint64_t)a[k + 1] << 32); double d; memcpy(&d, &v, 8); return d; }
static void put_double(uint32_t p, double d) { uint64_t v; memcpy(&v, &d, 8); WR64(p, v); }
static uint32_t mp_get_Duration(const uint32_t *a) { struct graph *g = G(a[0]); put_double(a[1], g->rate ? (double)g->frames / g->rate : 0); return DD_OK; }
static uint32_t mp_get_CurrentPosition(const uint32_t *a) {
    struct graph *g = G(a[0]);
    update(g);
    put_double(a[1], g->rate ? (double)position(g) / g->rate : 0);
    return DD_OK;
}
static uint32_t mp_put_CurrentPosition(const uint32_t *a) {
    struct graph *g = G(a[0]);
    double t = arg_double(a, 1);
    DSLOG("ds %s put_CurrentPosition %.3f (of %.3f)\n", g->name, t, g->rate ? (double)g->frames / g->rate : 0.0);
    uint64_t f = t <= 0 ? 0 : (uint64_t)(t * g->rate + 0.5);   // rounded: a seek to the end must reach it
    g->pos_frame = f > g->frames ? g->frames : f;
    g->complete_sent = 0;
    g->complete_pending = 0;
    thread_event_set(g->event_handle, 0);
    if (g->state == ST_RUNNING) refill(g);
    return DD_OK;
}
static uint32_t mp_get_StopTime(const uint32_t *a) { return mp_get_Duration(a); }
static uint32_t mp_put_Rate(const uint32_t *a) { (void)a; return DD_OK; }
static uint32_t mp_get_Rate(const uint32_t *a) { put_double(a[1], 1.0); return DD_OK; }

// ---------------------------------------------------------------- IBasicAudio (volume in 1/100 dB, -10000..0)
static uint32_t ba_put_Volume(const uint32_t *a) {
    struct graph *g = G(a[0]);
    g->volume = (int32_t)a[1];
    if (g->stream) SDL_SetAudioStreamGain(g->stream, g->volume <= -10000 ? 0.0f : powf(10.0f, (float)g->volume / 2000.0f));
    return DD_OK;
}
static uint32_t ba_get_Volume(const uint32_t *a) { WR32(a[1], (uint32_t)G(a[0])->volume); return DD_OK; }
static uint32_t ba_put_Balance(const uint32_t *a) { (void)a; return DD_OK; }

// The graph goes when all five of its interfaces are released (the game makes one per song played,
// each holding the whole decoded song).
static uint32_t graph_Release(const uint32_t *a) {
    if (!RD32(a[0] + 4)) return 0;                  // already freed
    struct graph *g = G(a[0]);
    uint32_t n = com_release(a);
    if (n || !g) return n;
    uint32_t objs[5] = { g->obj_gb, g->obj_mc, g->obj_me, g->obj_mp, g->obj_ba };
    for (int k = 0; k < 5; k++) if (RD32(objs[k] + 8)) return 0;
    for (int k = 0; k < ngraphs; k++)
        if (graphs[k] == g) { graphs[k] = graphs[--ngraphs]; break; }
    for (int k = 0; k < 5; k++) com_free(objs[k]);
    int thread_close(uint32_t h);
    thread_close(g->event_handle);
    if (g->stream) SDL_DestroyAudioStream(g->stream);
    free(g->pcm);
    free(g);
    return 0;
}

static const struct com_method GB_IMPL[] = {
    { "QueryInterface", gb_QueryInterface, 3 }, { "AddRef", com_addref, 1 }, { "Release", graph_Release, 1 },
    { "RenderFile", gb_RenderFile, 3 }, { 0 } };
static const struct com_method MC_IMPL[] = {
    { "QueryInterface", gb_QueryInterface, 3 }, { "AddRef", com_addref, 1 }, { "Release", graph_Release, 1 },
    { "Run", mc_Run, 1 }, { "Pause", mc_Pause, 1 }, { "Stop", mc_Stop, 1 }, { "GetState", mc_GetState, 3 },
    { "StopWhenReady", mc_Stop, 1 }, { 0 } };
static const struct com_method ME_IMPL[] = {
    { "QueryInterface", gb_QueryInterface, 3 }, { "AddRef", com_addref, 1 }, { "Release", graph_Release, 1 },
    { "GetEventHandle", me_GetEventHandle, 2 }, { "GetEvent", me_GetEvent, 5 },
    { "WaitForCompletion", me_WaitForCompletion, 3 }, { "FreeEventParams", me_ok, 4 },
    { "SetNotifyWindow", me_ok, 4 }, { "SetNotifyFlags", me_ok, 2 }, { "CancelDefaultHandling", me_ok, 2 }, { 0 } };
static const struct com_method MP_IMPL[] = {
    { "QueryInterface", gb_QueryInterface, 3 }, { "AddRef", com_addref, 1 }, { "Release", graph_Release, 1 },
    { "get_Duration", mp_get_Duration, 2 }, { "put_CurrentPosition", mp_put_CurrentPosition, 3 },
    { "get_CurrentPosition", mp_get_CurrentPosition, 2 }, { "get_StopTime", mp_get_StopTime, 2 },
    { "put_Rate", mp_put_Rate, 3 }, { "get_Rate", mp_get_Rate, 2 }, { 0 } };
static const struct com_method BA_IMPL[] = {
    { "QueryInterface", gb_QueryInterface, 3 }, { "AddRef", com_addref, 1 }, { "Release", graph_Release, 1 },
    { "put_Volume", ba_put_Volume, 2 }, { "get_Volume", ba_get_Volume, 2 }, { "put_Balance", ba_put_Balance, 2 },
    { 0 } };

uint32_t dshow_create(uint32_t iid, uint32_t out) {
    if (!vt_gb) {
        vt_gb = com_vtable("IGraphBuilder", GB_METHODS, N(GB_METHODS), GB_IMPL);
        vt_mc = com_vtable("IMediaControl", MC_METHODS, N(MC_METHODS), MC_IMPL);
        vt_me = com_vtable("IMediaEventEx", ME_METHODS, N(ME_METHODS), ME_IMPL);
        vt_mp = com_vtable("IMediaPosition", MP_METHODS, N(MP_METHODS), MP_IMPL);
        vt_ba = com_vtable("IBasicAudio", BA_METHODS, N(BA_METHODS), BA_IMPL);
    }
    struct graph *g = calloc(1, sizeof *g);
    g->obj_gb = com_new(vt_gb, g);
    g->obj_mc = com_new(vt_mc, g);
    g->obj_me = com_new(vt_me, g);
    g->obj_mp = com_new(vt_mp, g);
    g->obj_ba = com_new(vt_ba, g);
    g->event_handle = thread_event_new(1);
    if (ngraphs < 256) graphs[ngraphs++] = g;
    uint32_t args[3] = { g->obj_gb, iid, out };
    return gb_QueryInterface(args);
}

int dshow_is_multimedia_stream(uint32_t clsid) { return guid_eq(clsid, CLSID_AMMultiMediaStream); }
