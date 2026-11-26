/*
 * winrecomp runtime - PERFORMANCE overlay (FPS / FRAME TIME / GAME SPEED), the
 * one status-overlay layout of option-c (overlay/status_overlay.c): a dark
 * panel in the top-left corner, refreshed twice a second.
 *   FPS        presented frames per second
 *   FRAME TIME the longest gap between two frames in the last half second
 *   GAME SPEED game logic ticks per second against the game's own rate
 *              (the port counts ticks with wr_overlay_game_tick)
 * Drawn in the present pass after the picture: WR_DUMP_FRAMES never holds it.
 */
#include "wr_vk.h"
#include "wr.h"
#include "status_overlay.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_mask;                 /* WR_PERF_* */
static int g_pos_x = -1, g_pos_y = -1;   /* the panel's top left in window pixels (-1: the corner) */

void wr_overlay_set_pos(int x, int y) { g_pos_x = x; g_pos_y = y; }
static double g_tick_hz = 30.0;    /* the game's own logic rate */
static uint64_t g_ticks;

static uint64_t g_t0, g_last, g_max;
static uint32_t g_n;
static uint64_t g_ticks0;
static char g_txt[3][16];

static SoPanel g_panel;
static wr_vk_texture *g_tex;
static uint32_t g_tex_w, g_tex_h, g_tex_gen;
static uint32_t *g_rgba;

void wr_overlay_set(int mask) { g_mask = mask & WR_PERF_ALL; }
int  wr_overlay_get(void) { return g_mask; }
void wr_overlay_set_tick_rate(double hz) { if (hz > 0) g_tick_hz = hz; }
void wr_overlay_game_tick(void) { g_ticks++; }

/* "fps+frametime", "all", "off" (the option-c settings value) */
int wr_overlay_parse(const char *v)
{
    if (!v)
        return 0;
    if (!strcmp(v, "all"))
        return WR_PERF_ALL;
    int m = 0;
    char buf[64];
    snprintf(buf, sizeof buf, "%s", v);
    for (char *t = strtok(buf, "+ "); t; t = strtok(NULL, "+ ")) {
        if (!strcmp(t, "fps")) m |= WR_PERF_FPS;
        else if (!strcmp(t, "frametime")) m |= WR_PERF_FRAMETIME;
        else if (!strcmp(t, "speed")) m |= WR_PERF_SPEED;
    }
    return m;
}

void wr_overlay_format(int mask, char *out, size_t cap)
{
    if ((mask & WR_PERF_ALL) == WR_PERF_ALL) { snprintf(out, cap, "all"); return; }
    if (!(mask & WR_PERF_ALL)) { snprintf(out, cap, "off"); return; }
    snprintf(out, cap, "%s%s%s%s%s",
             mask & WR_PERF_FPS ? "fps" : "",
             (mask & WR_PERF_FPS) && (mask & (WR_PERF_FRAMETIME | WR_PERF_SPEED)) ? "+" : "",
             mask & WR_PERF_FRAMETIME ? "frametime" : "",
             (mask & WR_PERF_FRAMETIME) && (mask & WR_PERF_SPEED) ? "+" : "",
             mask & WR_PERF_SPEED ? "speed" : "");
}

static void measure(void)
{
    uint64_t now = wr_time_ns();
    if (g_last) {
        uint64_t dt = now - g_last;
        if (dt > g_max)
            g_max = dt;
        g_n++;
    } else {
        g_t0 = now;
        g_ticks0 = g_ticks;
        snprintf(g_txt[0], sizeof g_txt[0], "--");
        snprintf(g_txt[1], sizeof g_txt[1], "--");
        snprintf(g_txt[2], sizeof g_txt[2], "--");
    }
    g_last = now;
    if (now - g_t0 >= 500000000ull && g_n) {
        double secs = (double)(now - g_t0) / 1e9;
        double fps = g_n / secs, ms = (double)g_max / 1e6;
        double sp = (double)(g_ticks - g_ticks0) / secs / g_tick_hz * 100.0;
        snprintf(g_txt[0], sizeof g_txt[0], "%u", (unsigned)(fps + 0.5));
        snprintf(g_txt[1], sizeof g_txt[1], "%.1f", ms > 999.0 ? 999.0 : ms);
        if (g_ticks == g_ticks0)
            snprintf(g_txt[2], sizeof g_txt[2], "--");   /* no game logic (menus, movies) */
        else
            snprintf(g_txt[2], sizeof g_txt[2], "%u", (unsigned)(sp > 999.0 ? 999.0 : sp + 0.5));
        g_t0 = now;
        g_n = 0;
        g_max = 0;
        g_ticks0 = g_ticks;
    }
}

/* Before the present pass (uploads are not allowed inside a render pass). */
void wr_overlay_prepare(void)
{
    measure();
    if (!g_mask || !so_ready())
        return;
    char l[3][32];
    SoLine ln[3];
    int n = 0;
    if (g_mask & WR_PERF_FPS) { snprintf(l[n], sizeof l[n], "%s FPS", g_txt[0]); ln[n].text = l[n]; ln[n].rgb = SO_TEXT; n++; }
    if (g_mask & WR_PERF_FRAMETIME) { snprintf(l[n], sizeof l[n], "%s MS", g_txt[1]); ln[n].text = l[n]; ln[n].rgb = SO_TEXT; n++; }
    if (g_mask & WR_PERF_SPEED) { snprintf(l[n], sizeof l[n], "SPEED %s %%", g_txt[2]); ln[n].text = l[n]; ln[n].rgb = SO_TEXT; n++; }
    if (so_render(&g_panel, ln, n, (int)g_vk.extent.height) != 0 || !g_panel.w || !g_panel.h)
        return;
    if (g_tex && g_tex_gen == g_panel.gen)
        return;
    uint32_t w = (uint32_t)g_panel.w, h = (uint32_t)g_panel.h;
    uint32_t *rgba = realloc(g_rgba, (size_t)w * h * 4);
    if (!rgba)
        return;
    g_rgba = rgba;
    for (uint32_t i = 0; i < w * h; i++) {   /* BGRA -> RGBA */
        const uint8_t *p = g_panel.px + i * 4;
        g_rgba[i] = (uint32_t)p[2] | (uint32_t)p[1] << 8 | (uint32_t)p[0] << 16 | 0xFF000000u;
    }
    if (g_tex && (g_tex_w < w || g_tex_h < h)) {
        wr_vk_texture_destroy(g_tex);
        g_tex = NULL;
    }
    if (!g_tex) {
        g_tex_w = w + 64;   /* room to grow without a new texture */
        g_tex_h = h + 16;
        g_tex = wr_vk_texture_create(g_tex_w, g_tex_h, NULL,
                                     wr_vk_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE));
        if (!g_tex)
            return;
    }
    wr_vk_texture_update(g_tex, 0, 0, w, h, g_rgba, w);
    g_tex_gen = g_panel.gen;
}

/* Inside the present pass, after the picture. */
void wr_overlay_draw(VkCommandBuffer cmd)
{
    if (!g_mask || !g_tex || !g_panel.w)
        return;
    int hgt = (int)g_vk.extent.height;
    int margin = (hgt / 48 > 14 ? hgt / 48 : 14) / 2;
    int left = g_pos_x >= 0 ? g_pos_x : margin, top = g_pos_y >= 0 ? g_pos_y : margin;
    wr_vk_blit_texture_part(cmd, g_tex, left, top, g_panel.w, g_panel.h,
                            (float)g_panel.w / (float)g_tex_w, (float)g_panel.h / (float)g_tex_h);
}
