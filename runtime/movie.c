// Movies: DirectShow multimedia streaming (IAMMultiMediaStream + IDirectDrawMediaStream) over the
// MPEG-1 system streams in STR*\*.STR, decoded with pl_mpeg. The game follows the DirectX SDK's
// ShowStream pattern: it creates one sample surface, calls IDirectDrawStreamSample::Update for
// every frame and blits that surface to the screen until Update reports the end of the stream.
// Update blocks until the frame is due, so the video keeps time with the soundtrack.
#include "com.h"
#include "ddraw.h"
#include "host.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
void subtitles_draw(const char *movie, double t, uint32_t pixels, uint32_t pitch, int w, int h);
void quote_draw(const char *movie, double t, uint32_t pixels, uint32_t pitch, int w, int h);
void poems_draw(const char *movie, const char *host, double t, uint8_t *rgba, int w, int h);
#define PL_MPEG_IMPLEMENTATION
#include "../third_party/pl_mpeg/pl_mpeg.h"

#define MSPID_PrimaryVideo "a35ff56a-9fda-11d0-8fdf-00c04fd9189d"
#define MSPID_PrimaryAudio "a35ff56b-9fda-11d0-8fdf-00c04fd9189d"
#define IID_IAMMultiMediaStream "bebe595c-9a6f-11d0-8fde-00c04fd9189d"
#define IID_IMultiMediaStream "b502d1bc-9a57-11d0-8fde-00c04fd9189d"
#define IID_IMediaStream "b502d1bd-9a57-11d0-8fde-00c04fd9189d"
#define IID_IDirectDrawMediaStream "f4104fce-9a70-11d0-8fde-00c04fd9189d"
#define IID_IStreamSample "b502d1be-9a57-11d0-8fde-00c04fd9189d"
#define IID_IDirectDrawStreamSample "f4104fcf-9a70-11d0-8fde-00c04fd9189d"
#define MS_S_ENDOFSTREAM 0x00040003u
#define MS_E_NOSTREAM 0x80040402u

static const char *const AMMS_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "GetInformation", "GetMediaStream", "EnumMediaStreams", "GetState",
    "SetState", "GetTime", "GetDuration", "Seek", "GetEndOfStreamEventHandle",
    "Initialize", "GetFilterGraph", "GetFilter", "AddMediaStream", "OpenFile", "OpenMoniker", "Render" };
static const char *const DDMS_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "GetMultiMediaStream", "GetInformation", "SetSameFormat",
    "AllocateSample", "CreateSharedSample", "SendEndOfStream",
    "GetFormat", "SetFormat", "GetDirectDraw", "SetDirectDraw", "CreateSample", "GetTimePerFrame" };
static const char *const SAMPLE_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "GetMediaStream", "GetSampleTimes", "SetSampleTimes", "Update",
    "CompletionStatus", "GetSurface", "SetRect" };
#define N(x) ((int)(sizeof x / sizeof x[0]))

static uint32_t vt_amms, vt_ddms, vt_sample;

struct movie {
    char name[16];                 // e.g. MV11, for the subtitles
    uint32_t obj_amms, obj_video, obj_sample;
    int want_video, want_audio;
    plm_t *video;                  // video-only decoder
    SDL_AudioStream *audio;
    float *pcm; size_t pcm_frames; int rate;
    struct dd_surface *surface;
    uint64_t start_ns;             // when SetState(RUN) was called
    int running, ended;
    uint8_t *rgb;
    char host[600];                // the movie file (the ending poems decode a plate from it)
};

static struct movie *M(uint32_t obj) { return com_host(obj); }

static uint32_t amms_QueryInterface(const uint32_t *a) {
    struct movie *m = M(a[0]);
    uint32_t r = 0;
    if (guid_eq(a[1], IID_IAMMultiMediaStream) || guid_eq(a[1], IID_IMultiMediaStream)) r = m->obj_amms;
    WR32(a[2], r);
    return r ? DD_OK : E_NOINTERFACE;
}
static uint32_t amms_Initialize(const uint32_t *a) { (void)a; return DD_OK; }
static uint32_t amms_AddMediaStream(const uint32_t *a) {
    struct movie *m = M(a[0]);
    if (guid_eq(a[2], MSPID_PrimaryVideo)) m->want_video = 1;
    else if (guid_eq(a[2], MSPID_PrimaryAudio)) m->want_audio = 1;
    if (a[4]) WR32(a[4], 0);
    return DD_OK;
}

static void stop_audio(struct movie *m) {
    if (m->audio) { SDL_DestroyAudioStream(m->audio); m->audio = 0; }
}

static uint32_t amms_OpenFile(const uint32_t *a) {
    struct movie *m = M(a[0]);
    char path[260], host[600];
    size_t k = 0;
    for (uint32_t c; (c = RD16(a[1] + 2u * k)) && k + 1 < sizeof path; k++) path[k] = (char)c;
    path[k] = 0;
    files_map(path, host, sizeof host);
    snprintf(m->host, sizeof m->host, "%s", host);
    {   // the movie's name: file name without extension
        const char *b = strrchr(path, 0x5c) ? strrchr(path, 0x5c) + 1 : path;   // after the last backslash
        size_t j = 0;
        for (; b[j] && b[j] != '.' && j + 1 < sizeof m->name; j++) m->name[j] = b[j];
        m->name[j] = 0;
    }
    m->video = plm_create_with_filename(host);
    if (!m->video) { fprintf(stderr, "movie %s: cannot open %s\n", path, host); return 0x80070002u; }
    plm_set_audio_enabled(m->video, 0);
    plm_set_loop(m->video, 0);
    int w = plm_get_width(m->video), h = plm_get_height(m->video);
    // the soundtrack: decode it all now with a second decoder (STR movies are short)
    plm_t *au = plm_create_with_filename(host);
    if (au && plm_get_num_audio_streams(au) > 0) {
        plm_set_video_enabled(au, 0);
        plm_set_audio_enabled(au, 1);
        m->rate = plm_get_samplerate(au);
        size_t cap = 1u << 20;
        m->pcm = malloc(cap * 2 * sizeof(float));
        plm_samples_t *s;
        while ((s = plm_decode_audio(au))) {
            if (m->pcm_frames + s->count > cap) { cap *= 2; m->pcm = realloc(m->pcm, cap * 2 * sizeof(float)); }
            memcpy(m->pcm + m->pcm_frames * 2, s->interleaved, s->count * 2 * sizeof(float));
            m->pcm_frames += s->count;
        }
    }
    if (au) plm_destroy(au);
    if (rt_trace || getenv("P1_MOVIELOG")) fprintf(stderr, "movie %s: %dx%d %.2f fps, %.2f s, audio %zu frames at %d Hz\n", path, w, h,
                          plm_get_framerate(m->video), plm_get_duration(m->video), m->pcm_frames, m->rate);
    m->rgb = malloc((size_t)w * (size_t)h * 4);
    return DD_OK;
}
static uint32_t amms_GetMediaStream(const uint32_t *a) {
    struct movie *m = M(a[0]);
    if (guid_eq(a[1], MSPID_PrimaryVideo) && m->video) {
        WR32(m->obj_video + 8, RD32(m->obj_video + 8) + 1);
        WR32(a[2], m->obj_video);
        return DD_OK;
    }
    WR32(a[2], 0);
    return MS_E_NOSTREAM;
}
static uint32_t amms_SetState(const uint32_t *a) {
    struct movie *m = M(a[0]);
    if (a[1] == 1 && !m->running) {            // STREAMSTATE_RUN
        m->running = 1;
        m->start_ns = SDL_GetTicksNS();
        if (m->want_audio && m->pcm_frames && !m->audio) {
            SDL_AudioSpec src = { SDL_AUDIO_F32, 2, m->rate };
            (void)src;
            m->audio = host_audio_stream_f32(2, m->rate);
            if (m->audio) SDL_PutAudioStreamData(m->audio, m->pcm, (int)(m->pcm_frames * 2 * sizeof(float)));
        }
    } else if (a[1] == 0) {                    // STREAMSTATE_STOP
        m->running = 0;
        stop_audio(m);
    }
    return DD_OK;
}
static uint32_t amms_GetState(const uint32_t *a) { WR32(a[1], M(a[0])->running ? 1u : 0u); return DD_OK; }
static uint32_t amms_Release(const uint32_t *a) {
    uint32_t n = com_release(a);
    if (!n) {
        struct movie *m = M(a[0]);
        stop_audio(m);
        if (m->video) { plm_destroy(m->video); m->video = 0; }
        free(m->pcm); m->pcm = 0; m->pcm_frames = 0;
        free(m->rgb); m->rgb = 0;
    }
    return n;
}
static void put_i64(uint32_t p, int64_t v) { WR64(p, (uint64_t)v); }
static uint32_t amms_GetDuration(const uint32_t *a) {
    struct movie *m = M(a[0]);
    put_i64(a[1], m->video ? (int64_t)(plm_get_duration(m->video) * 1e7) : 0);
    return DD_OK;
}
static uint32_t amms_GetTime(const uint32_t *a) {
    struct movie *m = M(a[0]);
    put_i64(a[1], m->running ? (int64_t)((SDL_GetTicksNS() - m->start_ns) / 100) : 0);
    return DD_OK;
}

// ---------------------------------------------------------------- IDirectDrawMediaStream
static uint32_t ddms_QueryInterface(const uint32_t *a) {
    struct movie *m = M(a[0]);
    uint32_t r = 0;
    if (guid_eq(a[1], IID_IDirectDrawMediaStream) || guid_eq(a[1], IID_IMediaStream)) r = m->obj_video;
    WR32(a[2], r);
    if (r) WR32(r + 8, RD32(r + 8) + 1);
    return r ? DD_OK : E_NOINTERFACE;
}
static uint32_t ddms_CreateSample(const uint32_t *a) {
    struct movie *m = M(a[0]);
    if (a[1]) m->surface = dd_surface_of(a[1]);
    else if (!m->surface) m->surface = dd_new_surface((uint32_t)plm_get_width(m->video), (uint32_t)plm_get_height(m->video));
    WR32(a[4], m->obj_sample);
    return DD_OK;
}
static uint32_t ddms_GetFormat(const uint32_t *a) {
    struct movie *m = M(a[0]);
    for (int k = 1; k <= 2; k++) if (a[k]) {
        uint32_t p = a[k];
        memset(MEM + p, 0, 108);
        WR32(p, 108); WR32(p + 4, 0x1007u);
        WR32(p + 8, (uint32_t)plm_get_height(m->video)); WR32(p + 12, (uint32_t)plm_get_width(m->video));
        WR32(p + 72, 32); WR32(p + 76, 0x40u); WR32(p + 84, 16); WR32(p + 88, 0xf800); WR32(p + 92, 0x07e0); WR32(p + 96, 0x001f);
    }
    return DD_OK;
}
static uint32_t ddms_ok(const uint32_t *a) { (void)a; return DD_OK; }
static uint32_t ddms_GetTimePerFrame(const uint32_t *a) {
    struct movie *m = M(a[0]);
    put_i64(a[1], (int64_t)(1e7 / plm_get_framerate(m->video)));
    return DD_OK;
}

// ---------------------------------------------------------------- IDirectDrawStreamSample
void thread_sleep(uint32_t ms);
static uint32_t sample_Update(const uint32_t *a) {
    struct movie *m = M(a[0]);
    if (!m->video || m->ended) return MS_S_ENDOFSTREAM;
    // P1_TURBO (tests, misc.c): movies end at once; P1_MOVIES=n: only those before presented frame n
    static int turbo = -1;
    static unsigned from = 0;
    if (turbo < 0) { turbo = getenv("P1_TURBO") != NULL; from = getenv("P1_MOVIES") ? (unsigned)atoi(getenv("P1_MOVIES")) : ~0u; }
    extern uint32_t frame_counter;
    if (turbo && frame_counter < from) { m->ended = 1; return MS_S_ENDOFSTREAM; }
    if (!m->running) { m->running = 1; m->start_ns = SDL_GetTicksNS(); }
    plm_frame_t *f = plm_decode_video(m->video);
    if (!f) { m->ended = 1; return MS_S_ENDOFSTREAM; }
    // wait until the frame is due (letting the other guest threads run)
    for (;;) {
        double now = (double)(SDL_GetTicksNS() - m->start_ns) / 1e9;
        if (now >= f->time) break;
        uint32_t ms = (uint32_t)((f->time - now) * 1000.0);
        thread_sleep(ms ? ms : 1);
    }
    struct dd_surface *s = m->surface;
    if (s) {
        int w = (int)f->width, h = (int)f->height;
        plm_frame_to_rgba(f, m->rgb, w * 4);
        extern int english_on;
        if (english_on) poems_draw(m->name, m->host, f->time, m->rgb, w, h);
        for (int y = 0; y < h && y < (int)s->h; y++) {
            const uint8_t *src = m->rgb + (size_t)y * (size_t)w * 4;
            uint8_t *dst = MEM + s->pixels + (uint32_t)y * s->pitch;
            for (int x = 0; x < w && x < (int)s->w; x++) {
                uint16_t v = (uint16_t)((src[4 * x] >> 3) << 11 | (src[4 * x + 1] >> 2) << 5 | (src[4 * x + 2] >> 3));
                dst[2 * x] = (uint8_t)v; dst[2 * x + 1] = (uint8_t)(v >> 8);
            }
        }
        extern int english_on;
        if (english_on) {
            quote_draw(m->name, f->time, s->pixels, s->pitch, (int)s->w, (int)s->h);
            subtitles_draw(m->name, f->time, s->pixels, s->pitch, (int)s->w, (int)s->h);
        }
    }
    return DD_OK;
}
static uint32_t sample_GetSurface(const uint32_t *a) {
    struct movie *m = M(a[0]);
    if (!m->surface) return 0x80004005u;
    if (a[1]) { WR32(a[1], m->surface->obj[0]); WR32(m->surface->obj[0] + 8, RD32(m->surface->obj[0] + 8) + 1); }
    if (a[2]) { WR32(a[2], 0); WR32(a[2] + 4, 0); WR32(a[2] + 8, m->surface->w); WR32(a[2] + 12, m->surface->h); }
    return DD_OK;
}
static uint32_t sample_CompletionStatus(const uint32_t *a) { return M(a[0])->ended ? MS_S_ENDOFSTREAM : DD_OK; }

static const struct com_method AMMS_IMPL[] = {
    { "QueryInterface", amms_QueryInterface, 3 }, { "AddRef", com_addref, 1 }, { "Release", amms_Release, 1 },
    { "Initialize", amms_Initialize, 4 }, { "AddMediaStream", amms_AddMediaStream, 5 },
    { "OpenFile", amms_OpenFile, 3 }, { "GetMediaStream", amms_GetMediaStream, 3 },
    { "SetState", amms_SetState, 2 }, { "GetState", amms_GetState, 2 }, { "GetDuration", amms_GetDuration, 2 },
    { "GetTime", amms_GetTime, 2 }, { 0 } };
static const struct com_method DDMS_IMPL[] = {
    { "QueryInterface", ddms_QueryInterface, 3 }, { "AddRef", com_addref, 1 }, { "Release", com_release, 1 },
    { "CreateSample", ddms_CreateSample, 5 }, { "GetFormat", ddms_GetFormat, 5 }, { "SetFormat", ddms_ok, 3 },
    { "GetTimePerFrame", ddms_GetTimePerFrame, 2 }, { 0 } };
static const struct com_method SAMPLE_IMPL[] = {
    { "AddRef", com_addref, 1 }, { "Release", com_release, 1 }, { "Update", sample_Update, 5 },
    { "GetSurface", sample_GetSurface, 3 }, { "CompletionStatus", sample_CompletionStatus, 3 },
    { "SetRect", ddms_ok, 2 }, { 0 } };

uint32_t movie_create(uint32_t iid, uint32_t out) {
    if (!vt_amms) {
        vt_amms = com_vtable("IAMMultiMediaStream", AMMS_METHODS, N(AMMS_METHODS), AMMS_IMPL);
        vt_ddms = com_vtable("IDirectDrawMediaStream", DDMS_METHODS, N(DDMS_METHODS), DDMS_IMPL);
        vt_sample = com_vtable("IDirectDrawStreamSample", SAMPLE_METHODS, N(SAMPLE_METHODS), SAMPLE_IMPL);
    }
    struct movie *m = calloc(1, sizeof *m);
    m->obj_amms = com_new(vt_amms, m);
    m->obj_video = com_new(vt_ddms, m);
    m->obj_sample = com_new(vt_sample, m);
    uint32_t args[3] = { m->obj_amms, iid, out };
    return amms_QueryInterface(args);
}
