/*
 * winrecomp runtime - silent Miles Sound System (mss32) and absent Bink
 * (binkw32), until the real implementations (M5).
 *
 * Miles answers as a working digital driver whose samples and streams finish
 * at once (SMP_DONE), so the game runs its normal sound path. 3D providers are
 * not offered. BinkOpen fails, so movies are skipped.
 *
 * The decorated names carry the stdcall argument bytes: _AIL_x@8 pops 8.
 */
#include "../win32/w32_internal.h"

#include <time.h>

#define SMP_DONE 2u

#define STUB(fn, nargs, expr) WR_HANDLER(fn) { (void)base; WR_RET((expr), (nargs)); }

static uint32_t fake_obj(void) { return wr_galloc(0x200); }

/* ---- Miles -------------------------------------------------------------------------- */

STUB(ail_startup, 0, 1)
STUB(ail_shutdown, 0, 0)
STUB(ail_set_preference, 2, 0)
STUB(ail_set_digital_master_volume, 2, 0)
STUB(ail_waveOutClose, 1, 0)
STUB(ail_digital_handle_release, 1, 1)
STUB(ail_digital_handle_reacquire, 1, 1)
STUB(ail_mem_free_lock, 1, (wr_heap_free(WR_ARG(0)), 0))

WR_HANDLER(ail_waveOutOpen)
{
    (void)base;
    uint32_t pdrv = WR_ARG(0), pwo = WR_ARG(1);
    if (pdrv)
        wr_wr32(pdrv, fake_obj());
    if (pwo)
        wr_wr32(pwo, 0);
    wr_log("Miles: digital driver opened (silent until M5)");
    WR_RET(0, 4);
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
    struct timespec ts = { 0, (long)ticks * 16666667L };
    if (ticks < 120)
        nanosleep(&ts, NULL);
    WR_RET(0, 1);
}

/* 2D samples */
STUB(ail_allocate_sample_handle, 1, fake_obj())
STUB(ail_release_sample_handle, 1, 0)
STUB(ail_init_sample, 1, 0)
STUB(ail_set_sample_type, 3, 0)
STUB(ail_set_sample_address, 3, 0)
STUB(ail_set_sample_playback_rate, 2, 0)
STUB(ail_set_sample_volume, 2, 0)
STUB(ail_set_sample_pan, 2, 0)
STUB(ail_set_sample_loop_count, 2, 0)
STUB(ail_set_sample_loop_block, 3, 0)
STUB(ail_start_sample, 1, 0)
STUB(ail_end_sample, 1, 0)
STUB(ail_sample_status, 1, SMP_DONE)

/* streams */
STUB(ail_open_stream, 3, fake_obj())
STUB(ail_close_stream, 1, 0)
STUB(ail_start_stream, 1, 0)
STUB(ail_stream_status, 1, SMP_DONE)
STUB(ail_set_stream_volume, 2, 0)
STUB(ail_stream_volume, 1, 127)
STUB(ail_set_stream_loop_count, 2, 0)
STUB(ail_set_stream_ms_position, 2, 0)
STUB(ail_stream_playback_rate, 1, 22050)
STUB(ail_set_stream_playback_rate, 2, 0)

WR_HANDLER(ail_stream_ms_position)
{
    (void)base;
    if (WR_ARG(1)) wr_wr32(WR_ARG(1), 0);
    if (WR_ARG(2)) wr_wr32(WR_ARG(2), 0);
    WR_RET(0, 3);
}

/* 3D: no providers */
STUB(ail_enumerate_3D_providers, 3, 0)
STUB(ail_open_3D_provider, 1, 1)          /* M3D_NOTINIT-style failure */
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
    { "_AIL_set_preference@8", ail_set_preference },
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
    { "_AIL_sample_status@4", ail_sample_status },
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

/* ---- Bink ------------------------------------------------------------------------------ */

/* A movie of one frame that is over at once: frame counters (+0x08..+0x14
 * in the BINK header) all 1, so the game's "frame == frames" test ends the
 * playback through its normal path (it treats a failed BinkOpen as fatal). */
WR_HANDLER(bink_Open)
{
    (void)base;
    const char *name = wr_str(WR_ARG(0));
    wr_log("BinkOpen(%s): skipped (one empty frame) until M5", name ? name : "");
    uint32_t b = wr_galloc(0x200);
    wr_wr32(b + 0x00, 640);
    wr_wr32(b + 0x04, 480);
    for (uint32_t o = 0x08; o <= 0x14; o += 4)
        wr_wr32(b + o, 1);
    wr_wr32(b + 0x18, 30);    /* frame rate */
    wr_wr32(b + 0x1C, 1);
    WR_RET(b, 2);
}

WR_HANDLER(bink_BufferOpen)
{
    (void)base;
    uint32_t b = wr_galloc(0x200);
    wr_wr32(b + 0x00, 640);
    wr_wr32(b + 0x04, 480);
    WR_RET(b, 4);
}

STUB(bink_OpenMiles, 1, 0)
STUB(bink_SetSoundSystem, 2, 1)
STUB(bink_SetIOSize, 1, 0)
STUB(bink_BufferSetDDPrimary, 1, 0)
STUB(bink_Wait, 1, 0)
STUB(bink_DoFrame, 1, 0)
STUB(bink_CopyToBuffer, 7, 0)
STUB(bink_BufferLock, 1, 0)
STUB(bink_BufferBlit, 3, 0)
STUB(bink_BufferUnlock, 1, 0)
STUB(bink_NextFrame, 1, 0)
STUB(bink_Close, 1, (wr_heap_free(WR_ARG(0)), 0))
STUB(bink_BufferSetOffset, 3, 0)
STUB(bink_BufferCheckWinPos, 3, 0)
STUB(bink_BufferClose, 1, (wr_heap_free(WR_ARG(0)), 0))
STUB(bink_GetSummary, 2, 0)

static const wr_export bink_exports[] = {
    { "_BinkOpenMiles@4", bink_OpenMiles }, { "_BinkSetSoundSystem@8", bink_SetSoundSystem },
    { "_BinkSetIOSize@4", bink_SetIOSize }, { "_BinkOpen@8", bink_Open },
    { "_BinkBufferSetDDPrimary@4", bink_BufferSetDDPrimary }, { "_BinkWait@4", bink_Wait },
    { "_BinkDoFrame@4", bink_DoFrame }, { "_BinkCopyToBuffer@28", bink_CopyToBuffer },
    { "_BinkBufferLock@4", bink_BufferLock }, { "_BinkBufferBlit@12", bink_BufferBlit },
    { "_BinkBufferUnlock@4", bink_BufferUnlock }, { "_BinkBufferOpen@16", bink_BufferOpen },
    { "_BinkNextFrame@4", bink_NextFrame }, { "_BinkClose@4", bink_Close },
    { "_BinkBufferSetOffset@12", bink_BufferSetOffset }, { "_BinkBufferCheckWinPos@12", bink_BufferCheckWinPos },
    { "_BinkBufferClose@4", bink_BufferClose }, { "_BinkGetSummary@8", bink_GetSummary },
    { NULL, NULL }
};

static wr_module mss_mod = { "mss32.dll", mss_exports, 0 };
static wr_module bink_mod = { "binkw32.dll", bink_exports, 0 };

void wr_register_miles_bink_silent(void)
{
    wr_module_register(&mss_mod);
    wr_module_register(&bink_mod);
}
