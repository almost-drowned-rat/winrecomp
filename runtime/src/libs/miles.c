/*
 * winrecomp runtime - Miles Sound System 5 (mss32.dll) on the SDL mixer.
 *
 * Behaviour as GTA 2 drives MSS 5.0r (gta2_recomp docs/re/audio.md):
 * - volume 0..127 -> linear gain vol/127; pan 0..127, one-sided: p < 64 only
 *   lowers the right side (p/63), p > 64 only the left ((127-p)/63)
 * - sample types: 0 mono 8, 1 mono 16, 2 stereo 8, 3 stereo 16; 8-bit is
 *   unsigned unless DIG_PCM_SIGN; loop count 0 = forever; loop block in bytes
 * - status: SMP_DONE 2, SMP_PLAYING 4, SMP_STOPPED 8
 * - no hardware 3D provider (as on any modern Windows): games use 2D
 * - streams: WAV files (PCM / IMA ADPCM), seek by ms; a stereo IMA ADPCM
 *   stream reports half its rate (GTA 2 doubles it for the stereo radio)
 * - double-buffered samples (Bink's audio): load_sample_buffer /
 *   sample_buffer_ready / EOB callbacks - callbacks run on the guest thread
 *   (from AIL_serve and the buffer calls), never on the audio thread.
 * The decorated names carry the stdcall argument bytes: _AIL_x@8 pops 8.
 */
#include "../win32/w32_internal.h"
#include "../audio/wr_audio.h"

#include <stdlib.h>
#include <time.h>

#define SMP_FREE 1u
#define SMP_DONE 2u
#define SMP_PLAYING 4u
#define SMP_STOPPED 8u

#define MAX_SAMPLES 48

typedef struct msample {
    uint32_t va;                /* guest HSAMPLE */
    int voice;
    int type, flags;            /* DIG_F_*, DIG_PCM_* */
    uint32_t rate;
    uint32_t addr, len;         /* single buffer */
    int loops;
    uint32_t loop_start;
    int32_t loop_end;
    float vol, pan;
    uint32_t user[8];
    /* double buffering (Bink) */
    int buffered;
    uint32_t buf_addr[2], buf_len[2];
    int buf_state[2];           /* 0 free, 1 queued, 2 playing */
    int cur;                    /* buffer in the voice */
    uint32_t eob_cb;
    int eob_pending;
} msample;

static msample g_s[MAX_SAMPLES];
static uint32_t g_dig;          /* the one HDIGDRIVER */
static uint32_t g_pref[64];

static msample *sfind(uint32_t va)
{
    for (int i = 0; i < MAX_SAMPLES; i++)
        if (g_s[i].va && g_s[i].va == va)
            return &g_s[i];
    return NULL;
}

static float mss_pan(uint32_t p)
{
    if (p > 127) p = 127;
    if (p < 64) return -(1.0f - (float)p / 63.0f);
    if (p > 64) return 1.0f - (float)(127 - p) / 63.0f;
    return 0.0f;
}

static void apply_format(msample *s, const void *data, uint32_t len)
{
    int stereo = s->type & 2, bits16 = s->type & 1;
    wr_voice_pcm(s->voice, data, len, bits16 ? 16 : 8, stereo ? 2 : 1, s->rate);
    wr_voice_volume(s->voice, s->vol);
    wr_voice_pan(s->voice, s->pan);
}

/* ---- driver --------------------------------------------------------------------------- */

static int g_mtrace = -1;
#define MTRACE(...) do { if (g_mtrace < 0) { const char *e_ = getenv("WR_MILES_TRACE"); g_mtrace = e_ && *e_ == '1'; } \
                         if (g_mtrace) wr_log(__VA_ARGS__); } while (0)
#define STUB(fn, nargs, expr) WR_HANDLER(fn) { (void)base; MTRACE("AIL " #fn " %08X %08X", WR_ARG(0), WR_ARG(1)); WR_RET((expr), (nargs)); }

WR_HANDLER(ail_startup)
{
    (void)base;
    g_pref[1] = 64;   /* DIG_MIXER_CHANNELS */
    WR_RET(1, 0);
}
STUB(ail_shutdown, 0, (wr_audio_shutdown(), 0))

WR_HANDLER(ail_set_preference)
{
    (void)base;
    uint32_t n = WR_ARG(0), old = n < 64 ? g_pref[n] : 0;
    if (n < 64)
        g_pref[n] = WR_ARG(1);
    WR_RET(old, 2);
}

WR_HANDLER(ail_get_preference)
{
    (void)base;
    uint32_t n = WR_ARG(0);
    WR_RET(n < 64 ? g_pref[n] : 0, 1);
}

WR_HANDLER(ail_waveOutOpen)
{
    (void)base;
    uint32_t pdrv = WR_ARG(0), pwo = WR_ARG(1);
    wr_audio_init();
    if (!g_dig)
        g_dig = wr_galloc(0x200);
    if (pdrv)
        wr_wr32(pdrv, g_dig);
    if (pwo)
        wr_wr32(pwo, 0);
    wr_log("Miles: digital driver opened");
    WR_RET(0, 4);
}

STUB(ail_waveOutClose, 1, 0)
STUB(ail_digital_handle_release, 1, (wr_audio_pause(1), 1))
STUB(ail_digital_handle_reacquire, 1, (wr_audio_pause(0), 1))
STUB(ail_mem_free_lock, 1, (wr_heap_free(WR_ARG(0)), 0))

WR_HANDLER(ail_set_digital_master_volume)
{
    (void)base;
    uint32_t v = WR_ARG(1) > 127 ? 127 : WR_ARG(1);
    wr_audio_master((float)v / 127.0f);
    WR_RET(0, 2);
}

WR_HANDLER(ail_mem_alloc_lock)
{
    (void)base;
    WR_RET(wr_heap_alloc(WR_ARG(0), 1), 1);
}

WR_HANDLER(ail_delay)
{
    (void)base;
    uint32_t ticks = WR_ARG(0);   /* 1/60 s */
    if (ticks < 600) {
        uint64_t ns = (uint64_t)ticks * 16666667ull;
        struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
        nanosleep(&ts, NULL);
    }
    WR_RET(0, 1);
}

/* ---- samples ----------------------------------------------------------------------------- */

WR_HANDLER(ail_allocate_sample_handle)
{
    MTRACE("AIL ail_allocate_sample_handle %08X %08X %08X %08X", WR_ARG(0), WR_ARG(1), WR_ARG(2), WR_ARG(3));
    (void)base;
    for (int i = 0; i < MAX_SAMPLES; i++)
        if (!g_s[i].va) {
            msample *s = &g_s[i];
            memset(s, 0, sizeof *s);
            s->voice = wr_voice_new();
            if (s->voice < 0)
                WR_RET(0, 1);
            s->va = wr_galloc(0x100);
            s->type = 1;
            s->rate = 22050;
            s->vol = 1.0f;
            s->loops = 1;
            s->loop_end = -1;
            WR_RET(s->va, 1);
        }
    WR_RET(0, 1);
}

WR_HANDLER(ail_release_sample_handle)
{
    (void)base;
    msample *s = sfind(WR_ARG(0));
    if (s) {
        wr_voice_free(s->voice);
        wr_heap_free(s->va);
        memset(s, 0, sizeof *s);
    }
    WR_RET(0, 1);
}

WR_HANDLER(ail_init_sample)
{
    (void)base;
    msample *s = sfind(WR_ARG(0));
    if (s) {
        wr_voice_end(s->voice);
        s->type = 1;
        s->flags = 1;
        s->rate = 11025;
        s->vol = 1.0f;
        s->pan = 0;
        s->loops = 1;
        s->loop_start = 0;
        s->loop_end = -1;
        s->buffered = 0;
        s->buf_state[0] = s->buf_state[1] = 0;
    }
    WR_RET(0, 1);
}

WR_HANDLER(ail_set_sample_type)
{
    MTRACE("AIL ail_set_sample_type %08X %08X %08X %08X", WR_ARG(0), WR_ARG(1), WR_ARG(2), WR_ARG(3));
    (void)base;
    msample *s = sfind(WR_ARG(0));
    if (s) {
        s->type = (int)WR_ARG(1) & 3;
        s->flags = (int)WR_ARG(2);
    }
    WR_RET(0, 3);
}

WR_HANDLER(ail_set_sample_address)
{
    (void)base;
    msample *s = sfind(WR_ARG(0));
    if (s) {
        s->addr = WR_ARG(1);
        s->len = WR_ARG(2);
        s->buffered = 0;
        if (wr_mem_valid(s->addr, s->len) && s->len)
            apply_format(s, wr_ptr(s->addr), s->len);
    }
    WR_RET(0, 3);
}

WR_HANDLER(ail_set_sample_playback_rate)
{
    (void)base;
    msample *s = sfind(WR_ARG(0));
    if (s) {
        s->rate = WR_ARG(1) ? WR_ARG(1) : 1;
        wr_voice_rate(s->voice, s->rate);
    }
    WR_RET(0, 2);
}

WR_HANDLER(ail_set_sample_volume)
{
    (void)base;
    msample *s = sfind(WR_ARG(0));
    if (s) {
        uint32_t v = WR_ARG(1) > 127 ? 127 : WR_ARG(1);
        s->vol = (float)v / 127.0f;
        wr_voice_volume(s->voice, s->vol);
    }
    WR_RET(0, 2);
}

WR_HANDLER(ail_set_sample_pan)
{
    (void)base;
    msample *s = sfind(WR_ARG(0));
    if (s) {
        s->pan = mss_pan(WR_ARG(1));
        wr_voice_pan(s->voice, s->pan);
    }
    WR_RET(0, 2);
}

WR_HANDLER(ail_set_sample_loop_count)
{
    (void)base;
    msample *s = sfind(WR_ARG(0));
    if (s)
        s->loops = (int)WR_ARG(1);
    WR_RET(0, 2);
}

WR_HANDLER(ail_set_sample_loop_block)
{
    (void)base;
    msample *s = sfind(WR_ARG(0));
    if (s) {
        s->loop_start = WR_ARG(1);
        s->loop_end = (int32_t)WR_ARG(2);
    }
    WR_RET(0, 3);
}

WR_HANDLER(ail_start_sample)
{
    (void)base;
    msample *s = sfind(WR_ARG(0));
    if (s && !s->buffered && s->len) {
        /* a start always restarts from the beginning */
        apply_format(s, wr_ptr(s->addr), s->len);
        wr_voice_rate(s->voice, s->rate);
        wr_voice_loop(s->voice, s->loops, s->loop_start, s->loop_end < 0 ? 0 : (uint32_t)s->loop_end);
        wr_voice_play(s->voice);
    }
    WR_RET(0, 1);
}

STUB(ail_end_sample, 1, (sfind(WR_ARG(0)) ? (wr_voice_end(sfind(WR_ARG(0))->voice), 0) : 0))
STUB(ail_stop_sample, 1, (sfind(WR_ARG(0)) ? (wr_voice_stop(sfind(WR_ARG(0))->voice), 0) : 0))
STUB(ail_resume_sample, 1, (sfind(WR_ARG(0)) ? (wr_voice_play(sfind(WR_ARG(0))->voice), 0) : 0))

static void buffered_step(msample *s);

WR_HANDLER(ail_sample_status)
{
    MTRACE("AIL ail_sample_status %08X %08X %08X %08X", WR_ARG(0), WR_ARG(1), WR_ARG(2), WR_ARG(3));
    (void)base;
    msample *s = sfind(WR_ARG(0));
    if (!s)
        WR_RET(SMP_FREE, 1);
    if (s->buffered) {
        buffered_step(s);
        int any = s->buf_state[0] || s->buf_state[1];
        WR_RET(any ? SMP_PLAYING : SMP_DONE, 1);
    }
    int st = wr_voice_status(s->voice);
    WR_RET(st == WR_VOICE_PLAYING ? SMP_PLAYING : st == WR_VOICE_STOPPED ? SMP_STOPPED : SMP_DONE, 1);
}

WR_HANDLER(ail_set_sample_user_data)
{
    (void)base;
    msample *s = sfind(WR_ARG(0));
    if (s && WR_ARG(1) < 8)
        s->user[WR_ARG(1)] = WR_ARG(2);
    WR_RET(0, 3);
}

WR_HANDLER(ail_sample_user_data)
{
    (void)base;
    msample *s = sfind(WR_ARG(0));
    WR_RET(s && WR_ARG(1) < 8 ? s->user[WR_ARG(1)] : 0, 2);
}

/* ---- double buffering (Bink) ------------------------------------------------------------- */

/* Move the voice on: when its buffer finished, mark it free (EOB pending)
 * and continue with the other queued one. */
static void buffered_step(msample *s)
{
    if (s->buf_state[s->cur] == 2 && wr_voice_status(s->voice) != WR_VOICE_PLAYING) {
        s->buf_state[s->cur] = 0;
        s->eob_pending = 1;
        int n = s->cur ^ 1;
        if (s->buf_state[n] == 1) {
            s->cur = n;
            s->buf_state[n] = 2;
            apply_format(s, wr_ptr(s->buf_addr[n]), s->buf_len[n]);
            wr_voice_rate(s->voice, s->rate);
            wr_voice_loop(s->voice, 1, 0, 0);
            wr_voice_play(s->voice);
        }
    }
}

static void run_eob(X86Ctx *ctx, msample *s)
{
    if (s->eob_pending && s->eob_cb) {
        s->eob_pending = 0;
        uint32_t args[1] = { s->va };
        wr_call_guest(ctx, s->eob_cb, 1, args);
    }
    s->eob_pending = 0;
}

WR_HANDLER(ail_minimum_sample_buffer_size)
{
    (void)base;
    /* (dig, rate, format): ~1/8 s, 4-byte aligned */
    uint32_t rate = WR_ARG(1) ? WR_ARG(1) : 22050, fmt = WR_ARG(2);
    uint32_t bps = ((fmt & 1) ? 2 : 1) * ((fmt & 2) ? 2 : 1);
    WR_RET(((rate / 8) * bps + 3) & ~3u, 3);
}

WR_HANDLER(ail_sample_buffer_ready)
{
    MTRACE("AIL ail_sample_buffer_ready %08X %08X %08X %08X", WR_ARG(0), WR_ARG(1), WR_ARG(2), WR_ARG(3));
    msample *s = sfind(WR_ARG(0));
    (void)base;
    if (!s)
        WR_RET(0xFFFFFFFFu, 1);
    s->buffered = 1;
    buffered_step(s);
    run_eob(ctx, s);
    int r = !s->buf_state[0] ? 0 : !s->buf_state[1] ? 1 : -1;
    MTRACE("  ready -> %d (st %d %d cur %d vs %d)", r, s->buf_state[0], s->buf_state[1], s->cur, wr_voice_status(s->voice));
    WR_RET((uint32_t)r, 1);
}

WR_HANDLER(ail_load_sample_buffer)
{
    MTRACE("AIL ail_load_sample_buffer %08X %08X %08X %08X", WR_ARG(0), WR_ARG(1), WR_ARG(2), WR_ARG(3));
    (void)base;
    msample *s = sfind(WR_ARG(0));
    uint32_t n = WR_ARG(1) & 1, addr = WR_ARG(2), len = WR_ARG(3);
    if (s) {
        s->buffered = 1;
        if (len && wr_mem_valid(addr, len)) {
            s->buf_addr[n] = addr;
            s->buf_len[n] = len;
            s->buf_state[n] = 1;
            int other = n ^ 1;
            if (s->buf_state[other] != 2) {   /* nothing playing: start this one */
                s->cur = (int)n;
                s->buf_state[n] = 2;
                apply_format(s, wr_ptr(addr), len);
                wr_voice_rate(s->voice, s->rate);
                wr_voice_loop(s->voice, 1, 0, 0);
                wr_voice_play(s->voice);
            }
        }
    }
    WR_RET(0, 4);
}

/* (S, &pos0, &len0, &pos1, &len1): bytes played per buffer (mss32 0x21010930) */
WR_HANDLER(ail_sample_buffer_info)
{
    MTRACE("AIL ail_sample_buffer_info %08X %08X %08X %08X", WR_ARG(0), WR_ARG(1), WR_ARG(2), WR_ARG(3));
    (void)base;
    msample *s = sfind(WR_ARG(0));
    for (int i = 1; i <= 4; i++)
        if (WR_ARG(i))
            wr_wr32(WR_ARG(i), 0);
    if (s) {
        buffered_step(s);
        for (int b = 0; b < 2; b++) {
            uint32_t pos = 0;
            if (s->buf_state[b] == 2) {
                uint32_t total = 0, ms = wr_voice_ms(s->voice, &total);
                pos = total ? (uint32_t)((uint64_t)s->buf_len[b] * ms / total) : 0;
            }
            if (WR_ARG(1 + 2 * b)) wr_wr32(WR_ARG(1 + 2 * b), pos);
            if (WR_ARG(2 + 2 * b)) wr_wr32(WR_ARG(2 + 2 * b), s->buf_state[b] ? s->buf_len[b] : 0);
        }
    }
    /* MSS 5 returns the sample's "starved" flag (+0x40): 1 once every loaded
     * buffer has played, 0 again after load_sample_buffer. Bink polls it
     * (all pointers NULL) to wait for the movie's sound to drain. */
    int starved = !s || (!s->buf_state[0] && !s->buf_state[1]);
    WR_RET((uint32_t)starved, 5);
}

WR_HANDLER(ail_register_EOB_callback)
{
    MTRACE("AIL ail_register_EOB_callback %08X %08X %08X %08X", WR_ARG(0), WR_ARG(1), WR_ARG(2), WR_ARG(3));
    (void)base;
    msample *s = sfind(WR_ARG(0));
    uint32_t old = s ? s->eob_cb : 0;
    if (s)
        s->eob_cb = WR_ARG(1);
    WR_RET(old, 2);
}

WR_HANDLER(ail_serve)
{
    MTRACE("AIL ail_serve %08X %08X %08X %08X", WR_ARG(0), WR_ARG(1), WR_ARG(2), WR_ARG(3));
    (void)base;
    for (int i = 0; i < MAX_SAMPLES; i++)
        if (g_s[i].va && g_s[i].buffered) {
            buffered_step(&g_s[i]);
            run_eob(ctx, &g_s[i]);
        }
    WR_RET(0, 0);
}

/* ---- streams ------------------------------------------------------------------------------- */

typedef struct mstream { uint32_t va; int voice; int stereo_adpcm; } mstream;
static mstream g_st[8];

static mstream *stfind(uint32_t va)
{
    for (int i = 0; i < 8; i++)
        if (g_st[i].va && g_st[i].va == va)
            return &g_st[i];
    return NULL;
}

WR_HANDLER(ail_open_stream)
{
    (void)base;
    const char *name = wr_str(WR_ARG(1));
    char host[1200];
    if (!name || !wr_path_resolve_read(name, host, sizeof host)) {
        WR_TRACE("Miles: stream %s not found", name ? name : "");
        WR_RET(0, 3);
    }
    for (int i = 0; i < 8; i++)
        if (!g_st[i].va) {
            int v = wr_voice_new();
            if (v < 0 || !wr_voice_stream(v, host)) {
                if (v >= 0)
                    wr_voice_free(v);
                wr_log("Miles: stream %s cannot be played", name);
                WR_RET(0, 3);
            }
            g_st[i].voice = v;
            g_st[i].va = wr_galloc(0x100);
            /* MSS 5 reports half the rate of a stereo IMA ADPCM stream */
            FILE *f = fopen(host, "rb");
            uint8_t h[64] = { 0 };
            if (f) { fread(h, 1, sizeof h, f); fclose(f); }
            g_st[i].stereo_adpcm = (h[20] | h[21] << 8) == 0x11 && (h[22] | h[23] << 8) == 2;
            wr_voice_loop(v, 1, 0, 0);
            WR_TRACE("Miles: stream %s opened", name);
            WR_RET(g_st[i].va, 3);
        }
    WR_RET(0, 3);
}

WR_HANDLER(ail_close_stream)
{
    (void)base;
    mstream *s = stfind(WR_ARG(0));
    if (s) {
        wr_voice_free(s->voice);
        wr_heap_free(s->va);
        memset(s, 0, sizeof *s);
    }
    WR_RET(0, 1);
}

STUB(ail_start_stream, 1, (stfind(WR_ARG(0)) ? (wr_voice_play(stfind(WR_ARG(0))->voice), 0) : 0))

WR_HANDLER(ail_stream_status)
{
    (void)base;
    mstream *s = stfind(WR_ARG(0));
    if (!s)
        WR_RET(SMP_DONE, 1);
    int st = wr_voice_status(s->voice);
    WR_RET(st == WR_VOICE_PLAYING ? SMP_PLAYING : st == WR_VOICE_STOPPED ? SMP_STOPPED : SMP_DONE, 1);
}

WR_HANDLER(ail_set_stream_volume)
{
    (void)base;
    mstream *s = stfind(WR_ARG(0));
    uint32_t v = WR_ARG(1) > 127 ? 127 : WR_ARG(1);
    if (s)
        wr_voice_volume(s->voice, (float)v / 127.0f);
    WR_RET(0, 2);
}

STUB(ail_stream_volume, 1, 127)
STUB(ail_set_stream_loop_count, 2, (stfind(WR_ARG(0)) ? (wr_voice_loop(stfind(WR_ARG(0))->voice, (int)WR_ARG(1), 0, 0), 0) : 0))
STUB(ail_set_stream_ms_position, 2, (stfind(WR_ARG(0)) ? (wr_voice_seek_ms(stfind(WR_ARG(0))->voice, WR_ARG(1)), 0) : 0))

WR_HANDLER(ail_stream_playback_rate)
{
    (void)base;
    mstream *s = stfind(WR_ARG(0));
    if (!s)
        WR_RET(22050, 1);
    uint32_t r = wr_voice_get_rate(s->voice);
    WR_RET(s->stereo_adpcm ? r / 2 : r, 1);
}

STUB(ail_set_stream_playback_rate, 2, (stfind(WR_ARG(0)) ? (wr_voice_rate(stfind(WR_ARG(0))->voice, WR_ARG(1)), 0) : 0))

WR_HANDLER(ail_stream_ms_position)
{
    (void)base;
    mstream *s = stfind(WR_ARG(0));
    uint32_t total = 0, cur = s ? wr_voice_ms(s->voice, &total) : 0;
    if (WR_ARG(1)) wr_wr32(WR_ARG(1), total);
    if (WR_ARG(2)) wr_wr32(WR_ARG(2), cur);
    WR_RET(0, 3);
}

/* ---- 3D: no hardware provider (games fall back to 2D) ------------------------------------- */

STUB(ail_enumerate_3D_providers, 3, 0)
STUB(ail_open_3D_provider, 1, 1)
STUB(ail_close_3D_provider, 1, 0)
STUB(ail_3D_provider_attribute, 3, 0)
STUB(ail_set_3D_provider_preference, 3, 0)
STUB(ail_allocate_3D_sample_handle, 1, 0)
STUB(ail_release_3D_sample_handle, 1, 0)
STUB(ail_start_3D_sample, 1, 0)
STUB(ail_end_3D_sample, 1, 0)
STUB(ail_3D_sample_status, 1, SMP_DONE)
STUB(ail_set_3D_sample_loop_count, 2, 0)
STUB(ail_set_3D_sample_loop_block, 3, 0)
STUB(ail_set_3D_sample_playback_rate, 2, 0)
STUB(ail_set_3D_sample_float_distances, 5, 0)
STUB(ail_set_3D_position, 4, 0)
STUB(ail_set_3D_sample_volume, 2, 0)
STUB(ail_set_3D_sample_info, 2, 0)

static const wr_export mss_exports[] = {
    { "_AIL_startup@0", ail_startup }, { "_AIL_shutdown@0", ail_shutdown },
    { "_AIL_set_preference@8", ail_set_preference }, { "_AIL_get_preference@4", ail_get_preference },
    { "_AIL_set_digital_master_volume@8", ail_set_digital_master_volume },
    { "_AIL_waveOutOpen@16", ail_waveOutOpen }, { "_AIL_waveOutClose@4", ail_waveOutClose },
    { "_AIL_digital_handle_release@4", ail_digital_handle_release },
    { "_AIL_digital_handle_reacquire@4", ail_digital_handle_reacquire },
    { "_AIL_mem_alloc_lock@4", ail_mem_alloc_lock }, { "_AIL_mem_free_lock@4", ail_mem_free_lock },
    { "_AIL_delay@4", ail_delay },
    { "_AIL_allocate_sample_handle@4", ail_allocate_sample_handle },
    { "_AIL_release_sample_handle@4", ail_release_sample_handle },
    { "_AIL_init_sample@4", ail_init_sample }, { "_AIL_set_sample_type@12", ail_set_sample_type },
    { "_AIL_set_sample_address@12", ail_set_sample_address },
    { "_AIL_set_sample_playback_rate@8", ail_set_sample_playback_rate },
    { "_AIL_set_sample_volume@8", ail_set_sample_volume }, { "_AIL_set_sample_pan@8", ail_set_sample_pan },
    { "_AIL_set_sample_loop_count@8", ail_set_sample_loop_count },
    { "_AIL_set_sample_loop_block@12", ail_set_sample_loop_block },
    { "_AIL_start_sample@4", ail_start_sample }, { "_AIL_end_sample@4", ail_end_sample },
    { "_AIL_stop_sample@4", ail_stop_sample }, { "_AIL_resume_sample@4", ail_resume_sample },
    { "_AIL_sample_status@4", ail_sample_status },
    { "_AIL_set_sample_user_data@12", ail_set_sample_user_data }, { "_AIL_sample_user_data@8", ail_sample_user_data },
    { "_AIL_minimum_sample_buffer_size@12", ail_minimum_sample_buffer_size },
    { "_AIL_sample_buffer_ready@4", ail_sample_buffer_ready }, { "_AIL_load_sample_buffer@16", ail_load_sample_buffer },
    { "_AIL_sample_buffer_info@20", ail_sample_buffer_info },
    { "_AIL_register_EOB_callback@8", ail_register_EOB_callback }, { "_AIL_serve@0", ail_serve },
    { "_AIL_open_stream@12", ail_open_stream }, { "_AIL_close_stream@4", ail_close_stream },
    { "_AIL_start_stream@4", ail_start_stream }, { "_AIL_stream_status@4", ail_stream_status },
    { "_AIL_set_stream_volume@8", ail_set_stream_volume }, { "_AIL_stream_volume@4", ail_stream_volume },
    { "_AIL_set_stream_loop_count@8", ail_set_stream_loop_count },
    { "_AIL_stream_ms_position@12", ail_stream_ms_position },
    { "_AIL_set_stream_ms_position@8", ail_set_stream_ms_position },
    { "_AIL_stream_playback_rate@4", ail_stream_playback_rate },
    { "_AIL_set_stream_playback_rate@8", ail_set_stream_playback_rate },
    { "_AIL_enumerate_3D_providers@12", ail_enumerate_3D_providers },
    { "_AIL_open_3D_provider@4", ail_open_3D_provider }, { "_AIL_close_3D_provider@4", ail_close_3D_provider },
    { "_AIL_3D_provider_attribute@12", ail_3D_provider_attribute },
    { "_AIL_set_3D_provider_preference@12", ail_set_3D_provider_preference },
    { "_AIL_allocate_3D_sample_handle@4", ail_allocate_3D_sample_handle },
    { "_AIL_release_3D_sample_handle@4", ail_release_3D_sample_handle },
    { "_AIL_start_3D_sample@4", ail_start_3D_sample }, { "_AIL_end_3D_sample@4", ail_end_3D_sample },
    { "_AIL_3D_sample_status@4", ail_3D_sample_status },
    { "_AIL_set_3D_sample_loop_count@8", ail_set_3D_sample_loop_count },
    { "_AIL_set_3D_sample_loop_block@12", ail_set_3D_sample_loop_block },
    { "_AIL_set_3D_sample_playback_rate@8", ail_set_3D_sample_playback_rate },
    { "_AIL_set_3D_sample_float_distances@20", ail_set_3D_sample_float_distances },
    { "_AIL_set_3D_position@16", ail_set_3D_position },
    { "_AIL_set_3D_sample_volume@8", ail_set_3D_sample_volume },
    { "_AIL_set_3D_sample_info@8", ail_set_3D_sample_info },
    { NULL, NULL }
};

static wr_module mss_mod = { "mss32.dll", mss_exports, 0, 0 };

void wr_register_miles(void) { wr_module_register(&mss_mod); }
