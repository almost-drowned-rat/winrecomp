/*
 * winrecomp runtime - game controllers (SDL3 gamepads) as virtual keys.
 *
 * The port supplies a table pad control -> DirectInput key code (DIK), and
 * may compute the code at run time (GTA 2 looks up the key the game's own
 * control settings assign to an action). Pressed controls show up in
 * DirectInput's keyboard state and as WM_KEYDOWN/WM_KEYUP edges, so menus
 * and gameplay both see them like real keys.
 */
#include "wr_platform.h"
#include "wr.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Gamepad *g_pads[4];
static wr_pad_map_fn g_map;
static uint8_t g_down[256];        /* virtual DIK state from pads */
static int g_inited;
static int g_log;                  /* WR_PAD_LOG=1: every press */
static uint64_t g_ctl_prev[4];

void wr_pad_set_map(wr_pad_map_fn fn) { g_map = fn; }

static void open_pads(void)
{
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n && i < 4; i++) {
        int have = 0;
        for (int k = 0; k < 4; k++)
            if (g_pads[k] && SDL_GetGamepadID(g_pads[k]) == ids[i])
                have = 1;
        if (have)
            continue;
        for (int k = 0; k < 4; k++)
            if (!g_pads[k]) {
                g_pads[k] = SDL_OpenGamepad(ids[i]);
                if (g_pads[k])
                    wr_log("pad %d: %s", k, SDL_GetGamepadName(g_pads[k]));
                break;
            }
    }
    SDL_free(ids);
}

void wr_pad_event(const void *ev)
{
    const SDL_Event *e = (const SDL_Event *)ev;
    if (e->type == SDL_EVENT_GAMEPAD_ADDED) {
        open_pads();
    } else if (e->type == SDL_EVENT_GAMEPAD_REMOVED) {
        for (int k = 0; k < 4; k++)
            if (g_pads[k] && SDL_GetGamepadID(g_pads[k]) == e->gdevice.which) {
                SDL_CloseGamepad(g_pads[k]);
                g_pads[k] = NULL;
                wr_log("pad %d: removed", k);
            }
    }
}

int wr_pad_count(void)
{
    int n = 0;
    for (int k = 0; k < 4; k++)
        n += g_pads[k] != NULL;
    return n;
}

/* Poll every pad, map to virtual keys, report edges. */
void wr_pad_tick(void (*post_key)(uint8_t vk, uint8_t dik, int up))
{
    if (wr_platform_headless() || !g_map)
        return;
    if (!g_inited) {
        g_inited = 1;
        const char *e = getenv("WR_PAD_LOG");
        g_log = e && *e == '1';
        open_pads();
    }
    uint8_t now[256];
    uint64_t ctl_now[4] = { 0, 0, 0, 0 };
    memset(now, 0, sizeof now);
    for (int k = 0; k < 4; k++) {
        SDL_Gamepad *p = g_pads[k];
        if (!p)
            continue;
        for (int c = 0; c < WR_PAD_N; c++) {
            if (c > WR_PAD_DPAD_RIGHT && c < WR_PAD_LEFT_STICK_LEFT)
                continue;
            int on;
            switch (c) {
            case WR_PAD_LEFT_STICK_LEFT:  on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX) < -16000; break;
            case WR_PAD_LEFT_STICK_RIGHT: on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX) > 16000; break;
            case WR_PAD_LEFT_STICK_UP:    on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY) < -16000; break;
            case WR_PAD_LEFT_STICK_DOWN:  on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY) > 16000; break;
            case WR_PAD_LT: on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000; break;
            case WR_PAD_RT: on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000; break;
            case WR_PAD_RIGHT_STICK_LEFT:  on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX) < -16000; break;
            case WR_PAD_RIGHT_STICK_RIGHT: on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX) > 16000; break;
            case WR_PAD_RIGHT_STICK_UP:    on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY) < -16000; break;
            case WR_PAD_RIGHT_STICK_DOWN:  on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY) > 16000; break;
            default: on = SDL_GetGamepadButton(p, (SDL_GamepadButton)c); break;
            }
            if (!on)
                continue;
            uint8_t dik = g_map(c);
            if (dik)
                now[dik] = 0x80;
            if (g_log && !(g_ctl_prev[k] >> c & 1))
                wr_log("pad %d: control %d -> DIK 0x%02X", k, c, dik);
            ctl_now[k] |= 1ull << c;
        }
    }
    for (int i = 1; i < 256; i++) {
        if ((now[i] ^ g_down[i]) & 0x80) {
            uint8_t vk = 0;
            for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++) {
                uint8_t v2, d2;
                if (wr_key_from_sdl(sc, &v2, &d2) && d2 == i) {
                    vk = v2;
                    break;
                }
            }
            if (post_key)
                post_key(vk, (uint8_t)i, !(now[i] & 0x80));
        }
    }
    memcpy(g_down, now, sizeof now);
    memcpy(g_ctl_prev, ctl_now, sizeof ctl_now);
}

void wr_pad_dik(uint8_t st[256])
{
    for (int i = 0; i < 256; i++)
        st[i] |= g_down[i];
}

/* The left stick of the first connected pad, -1..1 each (x right, y down);
 * 0 when there is none. WR_FAKE_STICK="x,y" (dev runs) overrides it. */
int wr_pad_stick(float *x, float *y)
{
    *x = *y = 0;
    const char *f = getenv("WR_FAKE_STICK");
    if (f && *f && sscanf(f, "%f,%f", x, y) == 2)
        return 1;
    for (int k = 0; k < 4; k++)
        if (g_pads[k]) {
            *x = SDL_GetGamepadAxis(g_pads[k], SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
            *y = SDL_GetGamepadAxis(g_pads[k], SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
            return 1;
        }
    return 0;
}

/* The d-pad of the first connected pad as a direction, -1/0/1 each (x right,
 * y down); 0 when there is none or nothing is pressed. */
int wr_pad_dpad(int *x, int *y)
{
    *x = *y = 0;
    for (int k = 0; k < 4; k++)
        if (g_pads[k]) {
            SDL_Gamepad *p = g_pads[k];
            *x = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) - SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
            *y = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN) - SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP);
            return *x || *y;
        }
    return 0;
}

/* Bit c set for every control c held on any pad (same thresholds as the
 * key mapping): for "press a button" rebinding. */
uint64_t wr_pad_controls(void)
{
    uint64_t m = 0;
    for (int k = 0; k < 4; k++) {
        SDL_Gamepad *p = g_pads[k];
        if (!p)
            continue;
        for (int c = 0; c < WR_PAD_N; c++) {
            if (c > WR_PAD_DPAD_RIGHT && c < WR_PAD_LEFT_STICK_LEFT)
                continue;
            int on;
            switch (c) {
            case WR_PAD_LEFT_STICK_LEFT:  on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX) < -16000; break;
            case WR_PAD_LEFT_STICK_RIGHT: on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX) > 16000; break;
            case WR_PAD_LEFT_STICK_UP:    on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY) < -16000; break;
            case WR_PAD_LEFT_STICK_DOWN:  on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY) > 16000; break;
            case WR_PAD_LT: on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000; break;
            case WR_PAD_RT: on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000; break;
            case WR_PAD_RIGHT_STICK_LEFT:  on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX) < -16000; break;
            case WR_PAD_RIGHT_STICK_RIGHT: on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX) > 16000; break;
            case WR_PAD_RIGHT_STICK_UP:    on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY) < -16000; break;
            case WR_PAD_RIGHT_STICK_DOWN:  on = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY) > 16000; break;
            default: on = SDL_GetGamepadButton(p, (SDL_GamepadButton)c); break;
            }
            if (on)
                m |= 1ull << c;
        }
    }
    return m;
}

/* Names in the Xbox layout (positions, as SDL reports them) */
const char *wr_pad_control_name(int c)
{
    static const char *const n[WR_PAD_N] = {
        [WR_PAD_SOUTH] = "A", [WR_PAD_EAST] = "B", [WR_PAD_WEST] = "X", [WR_PAD_NORTH] = "Y",
        [WR_PAD_BACK] = "SELECT", [WR_PAD_GUIDE] = "HOME", [WR_PAD_START] = "START", [WR_PAD_LSTICK] = "L3",
        [WR_PAD_RSTICK] = "R3", [WR_PAD_LB] = "LB", [WR_PAD_RB] = "RB", [WR_PAD_DPAD_UP] = "D-PAD UP",
        [WR_PAD_DPAD_DOWN] = "D-PAD DOWN", [WR_PAD_DPAD_LEFT] = "D-PAD LEFT", [WR_PAD_DPAD_RIGHT] = "D-PAD RIGHT",
        [WR_PAD_LEFT_STICK_LEFT] = "STICK LEFT", [WR_PAD_LEFT_STICK_RIGHT] = "STICK RIGHT",
        [WR_PAD_LEFT_STICK_UP] = "STICK UP", [WR_PAD_LEFT_STICK_DOWN] = "STICK DOWN", [WR_PAD_LT] = "LT",
        [WR_PAD_RT] = "RT", [WR_PAD_RIGHT_STICK_LEFT] = "R-STICK LEFT", [WR_PAD_RIGHT_STICK_RIGHT] = "R-STICK RIGHT",
        [WR_PAD_RIGHT_STICK_UP] = "R-STICK UP", [WR_PAD_RIGHT_STICK_DOWN] = "R-STICK DOWN",
    };
    return c >= 0 && c < WR_PAD_N && n[c] ? n[c] : "-";
}

/* The right stick of the first connected pad, -1..1 each (x right, y down). */
int wr_pad_stick_right(float *x, float *y)
{
    *x = *y = 0;
    const char *f = getenv("WR_FAKE_RSTICK");
    if (f && *f && sscanf(f, "%f,%f", x, y) == 2)
        return 1;
    for (int k = 0; k < 4; k++)
        if (g_pads[k]) {
            *x = SDL_GetGamepadAxis(g_pads[k], SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
            *y = SDL_GetGamepadAxis(g_pads[k], SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
            return 1;
        }
    return 0;
}
