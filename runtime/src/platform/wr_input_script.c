/*
 * winrecomp runtime - scripted input for tests (WR_INPUT=<file>).
 *
 * Lines: "<ms> <key> [hold_ms]" - at <ms> after start press <key> (an SDL
 * scancode name: Return, Escape, Up, Down, Left, Right, Space, A, ...) for
 * hold_ms (default 120). '#' starts a comment. The key reaches the guest both
 * as window messages and in DirectInput's keyboard state, like a real press.
 */
#include "wr_platform.h"
#include "wr.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ev { uint32_t at, hold; int sc; int state; } ev;   /* state 0 pending, 1 down, 2 done */
static ev g_ev[512];
static int g_nev = -1;
static uint8_t g_dik_down[256];

static void load(void)
{
    g_nev = 0;
    const char *path = getenv("WR_INPUT");
    if (!path || !*path)
        return;
    FILE *f = fopen(path, "r");
    if (!f) {
        wr_log("input script %s: cannot open", path);
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f) && g_nev < 512) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        unsigned at = 0, hold = 120;
        char key[64];
        int n = sscanf(line, "%u %63s %u", &at, key, &hold);
        if (n < 2)
            continue;
        SDL_Scancode sc = SDL_GetScancodeFromName(key);
        if (sc == SDL_SCANCODE_UNKNOWN) {
            wr_log("input script: unknown key '%s'", key);
            continue;
        }
        g_ev[g_nev++] = (ev){ at, hold, (int)sc, 0 };
    }
    fclose(f);
    wr_log("input script %s: %d presses", path, g_nev);
}

/* Called from the message pump: post_key(vk, dik, up) delivers messages. */
void wr_input_script_tick(void (*post_key)(uint8_t vk, uint8_t dik, int up))
{
    if (g_nev < 0)
        load();
    uint32_t now = (uint32_t)(wr_time_ns() / 1000000u);
    for (int i = 0; i < g_nev; i++) {
        ev *e = &g_ev[i];
        uint8_t vk, dik;
        if (e->state == 2 || !wr_key_from_sdl(e->sc, &vk, &dik))
            continue;
        if (e->state == 0 && now >= e->at) {
            e->state = 1;
            g_dik_down[dik] = 0x80;
            post_key(vk, dik, 0);
            wr_log("input script: %s down", SDL_GetScancodeName((SDL_Scancode)e->sc));
        } else if (e->state == 1 && now >= e->at + e->hold) {
            e->state = 2;
            g_dik_down[dik] = 0;
            post_key(vk, dik, 1);
        }
    }
}

/* OR the scripted keys into a DirectInput state. */
void wr_input_script_dik(uint8_t st[256])
{
    for (int i = 0; i < 256; i++)
        st[i] |= g_dik_down[i];
}
