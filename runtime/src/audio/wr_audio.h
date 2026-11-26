/*
 * winrecomp runtime - software mixer on SDL3 audio.
 *
 * Voices play either a PCM buffer (host pointer, usually into guest memory)
 * or a WAV file streamed from disk (PCM or IMA ADPCM, decoded block by block).
 * The mixer runs on SDL's audio thread; every setter takes a short lock.
 * Headless runs (no audio device) keep all state and report "done" for
 * buffers once their duration has passed, so a game's status polling works.
 */
#ifndef WR_AUDIO_H
#define WR_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { WR_VOICE_FREE = 0, WR_VOICE_STOPPED, WR_VOICE_PLAYING, WR_VOICE_DONE };

int  wr_audio_init(void);                 /* 1 = device open (0: silent, still usable) */
void wr_audio_shutdown(void);
void wr_audio_pause(int paused);          /* focus loss etc. */
void wr_audio_master(float vol);          /* 0..1 */
/* the port's music: fn fills up to `frames` 44100 Hz stereo s16 frames and returns how many (0 = silence
 * for now); called in the mixer thread under the audio lock. NULL = off. */
typedef long (*wr_music_fn)(int16_t *out, long frames);
void wr_audio_music(wr_music_fn fn);
void wr_audio_music_volume(float vol);   /* 0..1 */

int  wr_voice_new(void);                  /* -1 if none left */
void wr_voice_free(int v);
/* PCM: bits 8 (unsigned) or 16 (signed LE), channels 1/2 */
void wr_voice_pcm(int v, const void *data, uint32_t bytes, int bits, int channels, uint32_t rate);
/* Stream a WAV file (host path); 0 if it cannot be opened/parsed */
int  wr_voice_stream(int v, const char *host_path);
void wr_voice_rate(int v, uint32_t rate);
uint32_t wr_voice_get_rate(int v);
void wr_voice_volume(int v, float vol);   /* 0..1 */
void wr_voice_pan(int v, float pan);      /* -1 left .. 0 centre .. +1 right */
/* loops: 0 = forever, n = play n times; region in bytes (end 0 = to the end) */
void wr_voice_loop(int v, int count, uint32_t start, uint32_t end);
void wr_voice_queue(int v, const void *data, uint32_t bytes);   /* next buffer, seamless (same format) */
uint32_t wr_voice_switches(int v);   /* how often the voice moved on to a queued buffer */
void wr_voice_play(int v);
void wr_voice_stop(int v);                /* pause: play resumes */
void wr_voice_end(int v);                 /* stop and mark done */
int  wr_voice_status(int v);
/* stream position / length in ms */
uint32_t wr_voice_ms(int v, uint32_t *total);
void wr_voice_seek_ms(int v, uint32_t ms);

#ifdef __cplusplus
}
#endif
#endif
