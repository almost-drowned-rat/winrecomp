/*
 * winrecomp runtime - software mixer on SDL3 audio (see wr_audio.h).
 */
#include "wr_audio.h"
#include "wr.h"
#include "platform/wr_platform.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VOICES 64
#define OUT_RATE 48000

typedef struct stream {
    FILE *f;
    int format;                /* 1 PCM, 0x11 IMA ADPCM */
    int channels, bits;
    uint32_t rate, block_align, spb;   /* samples (frames) per block */
    long data_off;
    uint32_t data_bytes;
    uint32_t total_frames;
    int16_t *blk;              /* decoded block, interleaved */
    uint32_t blk_frames, blk_first;    /* frames in blk, index of its first frame */
} stream;

typedef struct voice {
    int state;
    int is_stream;
    const uint8_t *data;
    uint32_t bytes;
    int bits, channels;
    uint32_t rate;
    uint32_t frames;           /* total frames */
    double pos;                /* frame position */
    int loops;                 /* remaining extra passes; -1 forever */
    int loop_count;
    uint32_t loop_start, loop_end;   /* frames */
    float vol, pan;
    stream st;
    uint64_t started_ns;       /* headless timing */
    uint32_t start_frame;
} voice;

static voice g_v[MAX_VOICES];
static SDL_Mutex *g_mx;
static SDL_AudioStream *g_dev;
static float g_master = 1.0f;
static int g_paused;

static void lock(void) { if (g_mx) SDL_LockMutex(g_mx); }
static void unlock(void) { if (g_mx) SDL_UnlockMutex(g_mx); }

/* ---- the port's music (custom soundtracks): 44100 Hz stereo s16 pulled in the mixer ---- */
static wr_music_fn g_music;
static float g_music_vol = 1.0f;
static int16_t g_mbuf[2048 * 2];
static long g_mbuf_n, g_mbuf_i;      /* frames in the buffer, the next one */
static double g_mfrac;

void wr_audio_music(wr_music_fn fn)
{
    lock();
    g_music = fn;
    g_mbuf_n = g_mbuf_i = 0;
    g_mfrac = 0;
    unlock();
}

void wr_audio_music_volume(float vol)
{
    g_music_vol = vol < 0 ? 0 : vol > 1 ? 1 : vol;
}

/* one source frame (advances), 0 when the port has nothing */
static int music_frame(float *l, float *r)
{
    if (g_mbuf_i >= g_mbuf_n) {
        g_mbuf_n = g_music ? g_music(g_mbuf, 2048) : 0;
        g_mbuf_i = 0;
        if (g_mbuf_n <= 0) {
            g_mbuf_n = 0;
            return 0;
        }
    }
    *l = g_mbuf[2 * g_mbuf_i] * (1.0f / 32768.0f);
    *r = g_mbuf[2 * g_mbuf_i + 1] * (1.0f / 32768.0f);
    g_mbuf_i++;
    return 1;
}

static void mix_music(float *out, int frames)
{
    static float pl, pr, cl, cr;   /* the two source frames around the output point */
    float g = g_music_vol * g_master;
    for (int n = 0; n < frames; n++) {
        g_mfrac += 44100.0 / OUT_RATE;
        while (g_mfrac >= 1.0) {
            g_mfrac -= 1.0;
            pl = cl;
            pr = cr;
            if (!music_frame(&cl, &cr)) {
                cl = cr = 0;
                if (!g_music)
                    return;
            }
        }
        float t = (float)g_mfrac;
        out[2 * n] += (pl + (cl - pl) * t) * g;
        out[2 * n + 1] += (pr + (cr - pr) * t) * g;
    }
}

static int valid(int v) { return v >= 0 && v < MAX_VOICES && g_v[v].state != WR_VOICE_FREE; }

/* ---- IMA ADPCM ------------------------------------------------------------------- */

static const int16_t k_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767 };
static const int8_t k_idx[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static int16_t ima_nibble(int *pred, int *idx, int nib)
{
    int step = k_step[*idx];
    int diff = step >> 3;
    if (nib & 1) diff += step >> 2;
    if (nib & 2) diff += step >> 1;
    if (nib & 4) diff += step;
    if (nib & 8) diff = -diff;
    *pred += diff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += k_idx[nib & 15];
    if (*idx < 0) *idx = 0;
    if (*idx > 88) *idx = 88;
    return (int16_t)*pred;
}

/* decode the block holding frame `frame` into st->blk */
static int stream_load(stream *st, uint32_t frame)
{
    if (st->blk && frame >= st->blk_first && frame < st->blk_first + st->blk_frames)
        return 1;
    if (frame >= st->total_frames)
        return 0;
    if (st->format == 1) {
        uint32_t chunk = 2048;
        uint32_t first = frame - frame % chunk;
        uint32_t n = st->total_frames - first < chunk ? st->total_frames - first : chunk;
        uint32_t fbytes = (uint32_t)st->channels * (uint32_t)(st->bits / 8);
        uint8_t buf[2048 * 4];
        fseek(st->f, st->data_off + (long)(first * fbytes), SEEK_SET);
        size_t got = fread(buf, fbytes, n, st->f);
        for (size_t i = 0; i < got * (size_t)st->channels; i++)
            st->blk[i] = st->bits == 16 ? (int16_t)(buf[2 * i] | buf[2 * i + 1] << 8) : (int16_t)((buf[i] - 128) << 8);
        st->blk_first = first;
        st->blk_frames = (uint32_t)got;
        return got > 0;
    }
    /* IMA: block = per channel {s16 pred, u8 idx, u8 0}, then 4-byte words per channel */
    uint32_t b = frame / st->spb;
    uint8_t raw[8192];
    if (st->block_align > sizeof raw)
        return 0;
    fseek(st->f, st->data_off + (long)(b * st->block_align), SEEK_SET);
    size_t got = fread(raw, 1, st->block_align, st->f);
    if (got < (size_t)4 * st->channels)
        return 0;
    int ch = st->channels;
    int pred[2], idx[2];
    for (int c = 0; c < ch; c++) {
        pred[c] = (int16_t)(raw[4 * c] | raw[4 * c + 1] << 8);
        idx[c] = raw[4 * c + 2] > 88 ? 88 : raw[4 * c + 2];
        st->blk[c] = (int16_t)pred[c];
    }
    uint32_t nfr = 1;
    size_t p = (size_t)4 * ch;
    while (p + (size_t)4 * ch <= got && nfr < st->spb) {
        for (int c = 0; c < ch; c++) {
            const uint8_t *w = raw + p + 4 * (size_t)c;
            for (int k = 0; k < 8; k++) {
                int nib = (w[k >> 1] >> ((k & 1) * 4)) & 15;
                uint32_t f = nfr + (uint32_t)k;
                if (f < st->spb)
                    st->blk[f * (uint32_t)ch + (uint32_t)c] = ima_nibble(&pred[c], &idx[c], nib);
            }
        }
        nfr += 8;
        p += (size_t)4 * ch;
    }
    if (nfr > st->spb) nfr = st->spb;
    st->blk_first = b * st->spb;
    st->blk_frames = nfr;
    return 1;
}

static int stream_open(stream *st, const char *path)
{
    memset(st, 0, sizeof *st);
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fclose(f);
        return 0;
    }
    int have_fmt = 0;
    for (;;) {
        uint8_t ck[8];
        if (fread(ck, 1, 8, f) != 8)
            break;
        uint32_t n = (uint32_t)(ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24);
        if (!memcmp(ck, "fmt ", 4)) {
            uint8_t fm[64] = { 0 };
            fread(fm, 1, n < 64 ? n : 64, f);
            if (n > 64) fseek(f, (long)(n - 64), SEEK_CUR);
            st->format = fm[0] | fm[1] << 8;
            st->channels = fm[2] | fm[3] << 8;
            st->rate = (uint32_t)(fm[4] | fm[5] << 8 | fm[6] << 16 | (uint32_t)fm[7] << 24);
            st->block_align = (uint32_t)(fm[12] | fm[13] << 8);
            st->bits = fm[14] | fm[15] << 8;
            st->spb = st->format == 0x11 && n >= 20 ? (uint32_t)(fm[18] | fm[19] << 8) : 0;
            have_fmt = 1;
        } else if (!memcmp(ck, "data", 4)) {
            st->data_off = ftell(f);
            st->data_bytes = n;
            break;
        } else {
            fseek(f, (long)(n + (n & 1)), SEEK_CUR);
        }
    }
    if (!have_fmt || !st->data_off || st->channels < 1 || st->channels > 2 ||
        !(st->format == 1 || (st->format == 0x11 && st->block_align && st->spb))) {
        fclose(f);
        return 0;
    }
    if (st->format == 1) {
        st->total_frames = st->data_bytes / ((uint32_t)st->channels * (uint32_t)(st->bits / 8));
        st->blk = (int16_t *)malloc(2048 * 2 * sizeof(int16_t));
    } else {
        st->total_frames = (st->data_bytes / st->block_align) * st->spb;
        st->blk = (int16_t *)malloc((size_t)st->spb * 2 * sizeof(int16_t));
    }
    st->f = f;
    return 1;
}

static void stream_close(stream *st)
{
    if (st->f) fclose(st->f);
    free(st->blk);
    memset(st, 0, sizeof *st);
}

/* ---- mixing -------------------------------------------------------------------------- */

static inline void frame_at(voice *v, uint32_t fr, float *l, float *r)
{
    if (v->is_stream) {
        if (!stream_load(&v->st, fr)) { *l = *r = 0; return; }
        const int16_t *s = v->st.blk + (size_t)(fr - v->st.blk_first) * (size_t)v->st.channels;
        *l = s[0] * (1.0f / 32768.0f);
        *r = v->st.channels == 2 ? s[1] * (1.0f / 32768.0f) : *l;
        return;
    }
    if (v->bits == 16) {
        const int16_t *s = (const int16_t *)(const void *)(v->data + (size_t)fr * 2u * (size_t)v->channels);
        *l = s[0] * (1.0f / 32768.0f);
        *r = v->channels == 2 ? s[1] * (1.0f / 32768.0f) : *l;
    } else {
        const uint8_t *s = v->data + (size_t)fr * (size_t)v->channels;
        *l = (s[0] - 128) * (1.0f / 128.0f);
        *r = v->channels == 2 ? (s[1] - 128) * (1.0f / 128.0f) : *l;
    }
}

static void mix(float *out, int frames)
{
    memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    if (g_paused)
        return;
    for (int i = 0; i < MAX_VOICES; i++) {
        voice *v = &g_v[i];
        if (v->state != WR_VOICE_PLAYING || !v->frames)
            continue;
        uint32_t rate = v->is_stream ? (v->rate ? v->rate : v->st.rate) : v->rate;
        double step = (double)rate / OUT_RATE;
        float gl = v->vol * g_master * (v->pan > 0 ? 1.0f - v->pan : 1.0f);
        float gr = v->vol * g_master * (v->pan < 0 ? 1.0f + v->pan : 1.0f);
        uint32_t end = v->loop_end && v->loop_end <= v->frames ? v->loop_end : v->frames;
        for (int n = 0; n < frames; n++) {
            if (v->pos >= end) {
                if (v->loops == -1 || v->loops > 0) {
                    if (v->loops > 0) v->loops--;
                    v->pos = v->loop_start + (v->pos - end);
                    if (v->pos >= end) v->pos = v->loop_start;
                } else {
                    v->state = WR_VOICE_DONE;
                    break;
                }
            }
            uint32_t f0 = (uint32_t)v->pos;
            float t = (float)(v->pos - f0);
            float l0, r0, l1, r1;
            frame_at(v, f0, &l0, &r0);
            if (f0 + 1 < end) frame_at(v, f0 + 1, &l1, &r1); else { l1 = l0; r1 = r0; }
            out[2 * n] += (l0 + (l1 - l0) * t) * gl;
            out[2 * n + 1] += (r0 + (r1 - r0) * t) * gr;
            v->pos += step;
        }
    }
    if (g_music)
        mix_music(out, frames);
    for (int n = 0; n < 2 * frames; n++) {
        float s = out[n];
        out[n] = s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s;
    }
}

static void SDLCALL feed(void *ud, SDL_AudioStream *s, int additional, int total)
{
    (void)ud; (void)total;
    static int log = -1;
    static uint64_t last, t0, maxgap, maxmix, calls, frames_req, minq = ~0ull;
    uint64_t start = wr_time_ns();
    if (log < 0)
        log = getenv("WR_AUDIO_LOG") != NULL;
    if (log) {   /* WR_AUDIO_LOG=1: the device's requests, every 2 s */
        if (last && start - last > maxgap) maxgap = start - last;
        if (!t0) t0 = start;
        int q = SDL_GetAudioStreamQueued(s);
        if ((uint64_t)q < minq) minq = (uint64_t)q;
        calls++;
        frames_req += (uint64_t)(additional / 8);
    }
    static float buf[4096 * 2];
    while (additional > 0) {
        int frames = additional / 8;
        if (frames > 4096) frames = 4096;
        if (frames <= 0) frames = 1;
        lock();
        mix(buf, frames);
        unlock();
        SDL_PutAudioStreamData(s, buf, frames * 8);
        additional -= frames * 8;
    }
    if (log) {
        uint64_t end = wr_time_ns();
        if (end - start > maxmix) maxmix = end - start;
        last = start;
        if (end - t0 > 2000000000ull) {
            wr_log("audio: %llu calls, %llu frames asked (%.0f per call), longest gap %.1f ms, longest mix %.2f ms, "
                   "least queued %llu bytes", (unsigned long long)calls, (unsigned long long)frames_req,
                   calls ? (double)frames_req / (double)calls : 0.0, maxgap / 1e6, maxmix / 1e6, (unsigned long long)minq);
            t0 = end; calls = frames_req = maxgap = maxmix = 0; minq = ~0ull;
        }
    }
}

int wr_audio_init(void)
{
    if (!g_mx)
        g_mx = SDL_CreateMutex();
    const char *na = getenv("WR_NOAUDIO");
    if (wr_platform_headless() || (na && *na == '1'))
        return 0;   /* silent: state is kept, buffers finish by time */
#if defined(__ANDROID__)
    /* SDL's low-latency AAudio stream has a buffer of a few ms: every small stall of the mixer thread is a gap
     * (crackling sound, and Bink movies, paced by the sound, judder). A normal stream with ~21 ms is plenty. */
    SDL_SetHint(SDL_HINT_ANDROID_LOW_LATENCY_AUDIO, "0");
    /* AAudio called the mixer with gaps of 72..80 ms on the Odin 2 (underruns: crackling, Bink judder);
     * OpenSL ES at most 24 ms (measured with WR_AUDIO_LOG, 2026-11-26). SDL_AUDIO_DRIVER still overrides. */
    if (!SDL_GetHint(SDL_HINT_AUDIO_DRIVER))
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "opensles");
#endif
    if (!SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES))
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        wr_log("audio: SDL audio init failed: %s", SDL_GetError());
        return 0;
    }
    SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, OUT_RATE };
    g_dev = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (!g_dev) {
        wr_log("audio: no output device: %s", SDL_GetError());
        return 0;
    }
    SDL_ResumeAudioStreamDevice(g_dev);
    wr_log("audio: output %d Hz stereo float", OUT_RATE);
    return 1;
}

void wr_audio_shutdown(void)
{
    if (g_dev)
        SDL_DestroyAudioStream(g_dev);
    g_dev = NULL;
    for (int i = 0; i < MAX_VOICES; i++)
        if (g_v[i].is_stream)
            stream_close(&g_v[i].st);
}

void wr_audio_pause(int p) { lock(); g_paused = p; unlock(); }
void wr_audio_master(float vol) { lock(); g_master = vol; unlock(); }

int wr_voice_new(void)
{
    lock();
    for (int i = 0; i < MAX_VOICES; i++)
        if (g_v[i].state == WR_VOICE_FREE) {
            memset(&g_v[i], 0, sizeof g_v[i]);
            g_v[i].state = WR_VOICE_STOPPED;
            g_v[i].vol = 1.0f;
            g_v[i].loops = 0;
            g_v[i].loop_count = 1;
            unlock();
            return i;
        }
    unlock();
    WR_ONCE("audio: out of voices");
    return -1;
}

void wr_voice_free(int v)
{
    if (!valid(v))
        return;
    lock();
    if (g_v[v].is_stream)
        stream_close(&g_v[v].st);
    memset(&g_v[v], 0, sizeof g_v[v]);
    unlock();
}

void wr_voice_pcm(int v, const void *data, uint32_t bytes, int bits, int channels, uint32_t rate)
{
    if (!valid(v))
        return;
    lock();
    voice *x = &g_v[v];
    if (x->is_stream)
        stream_close(&x->st);
    x->is_stream = 0;
    x->data = (const uint8_t *)data;
    x->bytes = bytes;
    x->bits = bits == 8 ? 8 : 16;
    x->channels = channels == 2 ? 2 : 1;
    x->rate = rate ? rate : 22050;
    x->frames = bytes / (uint32_t)(x->channels * (x->bits / 8));
    x->pos = 0;
    x->loop_start = 0;
    x->loop_end = 0;
    x->state = WR_VOICE_STOPPED;
    unlock();
}

int wr_voice_stream(int v, const char *path)
{
    if (!valid(v))
        return 0;
    stream st;
    if (!stream_open(&st, path))
        return 0;
    lock();
    voice *x = &g_v[v];
    if (x->is_stream)
        stream_close(&x->st);
    x->st = st;
    x->is_stream = 1;
    x->rate = 0;
    x->frames = st.total_frames;
    x->channels = st.channels;
    x->pos = 0;
    x->loop_start = 0;
    x->loop_end = 0;
    x->state = WR_VOICE_STOPPED;
    unlock();
    return 1;
}

void wr_voice_rate(int v, uint32_t rate) { if (valid(v)) { lock(); g_v[v].rate = rate; unlock(); } }
uint32_t wr_voice_get_rate(int v)
{
    if (!valid(v)) return 0;
    return g_v[v].is_stream && !g_v[v].rate ? g_v[v].st.rate : g_v[v].rate;
}
void wr_voice_volume(int v, float vol) { if (valid(v)) { lock(); g_v[v].vol = vol; unlock(); } }
void wr_voice_pan(int v, float pan) { if (valid(v)) { lock(); g_v[v].pan = pan; unlock(); } }

void wr_voice_loop(int v, int count, uint32_t start, uint32_t end)
{
    if (!valid(v))
        return;
    lock();
    voice *x = &g_v[v];
    uint32_t fb = x->is_stream ? 1u : (uint32_t)(x->channels * (x->bits / 8));
    x->loop_count = count;
    x->loops = count == 0 ? -1 : count - 1;
    x->loop_start = x->is_stream ? 0 : start / fb;
    x->loop_end = x->is_stream || !end ? 0 : end / fb;
    unlock();
}

void wr_voice_play(int v)
{
    if (!valid(v))
        return;
    lock();
    voice *x = &g_v[v];
    if (x->state == WR_VOICE_DONE) {
        x->pos = 0;
        x->loops = x->loop_count == 0 ? -1 : x->loop_count - 1;
    }
    x->state = WR_VOICE_PLAYING;
    x->started_ns = wr_time_ns();
    x->start_frame = (uint32_t)x->pos;
    unlock();
}

void wr_voice_stop(int v) { if (valid(v)) { lock(); if (g_v[v].state == WR_VOICE_PLAYING) g_v[v].state = WR_VOICE_STOPPED; unlock(); } }
void wr_voice_end(int v) { if (valid(v)) { lock(); g_v[v].state = WR_VOICE_DONE; unlock(); } }

int wr_voice_status(int v)
{
    if (!valid(v))
        return WR_VOICE_FREE;
    voice *x = &g_v[v];
    /* no device: a buffer is done once its time has passed (loops: never) */
    if (!g_dev && x->state == WR_VOICE_PLAYING && x->loops != -1) {
        uint32_t rate = wr_voice_get_rate(v);
        double secs = rate ? (double)(x->frames - x->start_frame) * (x->loops + 1) / rate : 0;
        if ((double)(wr_time_ns() - x->started_ns) / 1e9 >= secs)
            x->state = WR_VOICE_DONE;
    }
    return x->state;
}

uint32_t wr_voice_ms(int v, uint32_t *total)
{
    if (!valid(v)) {
        if (total) *total = 0;
        return 0;
    }
    uint32_t rate = wr_voice_get_rate(v);
    if (!rate) rate = 22050;
    if (total) *total = (uint32_t)((uint64_t)g_v[v].frames * 1000u / rate);
    return (uint32_t)((uint64_t)g_v[v].pos * 1000u / rate);
}

void wr_voice_seek_ms(int v, uint32_t ms)
{
    if (!valid(v))
        return;
    lock();
    uint32_t rate = wr_voice_get_rate(v);
    double p = (double)ms * (rate ? rate : 22050) / 1000.0;
    if (p > g_v[v].frames) p = g_v[v].frames;
    g_v[v].pos = p;
    unlock();
}
