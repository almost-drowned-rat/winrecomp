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
/* The keyboard as a controller (option-c's layout; a port turns it on):
 * WASD left stick, arrows d-pad, Enter START, Backspace BACK, I/J/K/L =
 * Y/X/A/B, U/O = LB/RB, Z/P = LT/RT. Those keys then reach the game only
 * through the pad mapping (wr_keys_dik_state leaves them out). */
static int g_kb_pad;
static const struct { int sc, ctl; } k_kb_pad[] = {
    { SDL_SCANCODE_W, WR_PAD_LEFT_STICK_UP }, { SDL_SCANCODE_S, WR_PAD_LEFT_STICK_DOWN },
    { SDL_SCANCODE_A, WR_PAD_LEFT_STICK_LEFT }, { SDL_SCANCODE_D, WR_PAD_LEFT_STICK_RIGHT },
    { SDL_SCANCODE_UP, WR_PAD_DPAD_UP }, { SDL_SCANCODE_DOWN, WR_PAD_DPAD_DOWN },
    { SDL_SCANCODE_LEFT, WR_PAD_DPAD_LEFT }, { SDL_SCANCODE_RIGHT, WR_PAD_DPAD_RIGHT },
    { SDL_SCANCODE_RETURN, WR_PAD_START }, { SDL_SCANCODE_KP_ENTER, WR_PAD_START },
    { SDL_SCANCODE_BACKSPACE, WR_PAD_BACK },
    { SDL_SCANCODE_I, WR_PAD_NORTH }, { SDL_SCANCODE_J, WR_PAD_WEST }, { SDL_SCANCODE_K, WR_PAD_SOUTH },
    { SDL_SCANCODE_L, WR_PAD_EAST }, { SDL_SCANCODE_U, WR_PAD_LB }, { SDL_SCANCODE_O, WR_PAD_RB },
    { SDL_SCANCODE_Z, WR_PAD_LT }, { SDL_SCANCODE_P, WR_PAD_RT },
};

void wr_pad_keyboard_as_pad(int on) { g_kb_pad = on != 0; }
int wr_pad_keyboard_is_pad(void) { return g_kb_pad; }

/* 1 if SDL scancode sc is one of the keys the keyboard pad takes */
int wr_pad_keyboard_takes(int sc)
{
    if (!g_kb_pad)
        return 0;
    for (size_t i = 0; i < sizeof k_kb_pad / sizeof k_kb_pad[0]; i++)
        if (k_kb_pad[i].sc == sc)
            return 1;
    return 0;
}

static uint64_t kb_pad_mask(void)
{
    if (!g_kb_pad || !SDL_WasInit(SDL_INIT_VIDEO))
        return 0;
    int n = 0;
    const bool *st = SDL_GetKeyboardState(&n);
    uint64_t m = 0;
    for (size_t i = 0; i < sizeof k_kb_pad / sizeof k_kb_pad[0]; i++)
        if (k_kb_pad[i].sc < n && st[k_kb_pad[i].sc])
            m |= 1ull << k_kb_pad[i].ctl;
    return m;
}

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
                    wr_log("pad %d: %s (rumble %s)", k, SDL_GetGamepadName(g_pads[k]),
                           SDL_GetBooleanProperty(SDL_GetGamepadProperties(g_pads[k]), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN,
                                                  false) ? "yes" : "no");
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
    {   /* the keyboard as a controller */
        uint64_t km = kb_pad_mask();
        for (int c = 0; km && c < WR_PAD_N; c++)
            if (km >> c & 1) {
                uint8_t dik = g_map(c);
                if (dik)
                    now[dik] = 0x80;
            }
    }
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
    int have = 0;
    for (int k = 0; k < 4; k++)
        if (g_pads[k]) {
            *x = SDL_GetGamepadAxis(g_pads[k], SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
            *y = SDL_GetGamepadAxis(g_pads[k], SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
            have = 1;
            break;
        }
    uint64_t km = kb_pad_mask();   /* WASD as a full push when the real stick rests */
    if (km && *x * *x + *y * *y < 0.09f) {
        float kx = (float)((int)(km >> WR_PAD_LEFT_STICK_RIGHT & 1) - (int)(km >> WR_PAD_LEFT_STICK_LEFT & 1));
        float ky = (float)((int)(km >> WR_PAD_LEFT_STICK_DOWN & 1) - (int)(km >> WR_PAD_LEFT_STICK_UP & 1));
        if (kx || ky) {
            *x = kx;
            *y = ky;
            return 1;
        }
    }
    return have;
}

/* The d-pad of the first connected pad as a direction, -1/0/1 each (x right,
 * y down); 0 when there is none or nothing is pressed. */
int wr_pad_dpad(int *x, int *y)
{
    *x = *y = 0;
    uint64_t km = kb_pad_mask();
    if (km & (0xFull << WR_PAD_DPAD_UP)) {
        *x = (int)(km >> WR_PAD_DPAD_RIGHT & 1) - (int)(km >> WR_PAD_DPAD_LEFT & 1);
        *y = (int)(km >> WR_PAD_DPAD_DOWN & 1) - (int)(km >> WR_PAD_DPAD_UP & 1);
        if (*x || *y)
            return 1;
    }
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
    uint64_t m = kb_pad_mask();
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

/* The device's own vibrator (Android: handhelds like the Odin 2, whose built-in pad has no motor of its own -
 * SDL lists the phone vibrator as a haptic device). */
static SDL_Haptic *device_haptic(void)
{
    static int tried;
    static SDL_Haptic *h;
    if (tried)
        return h;
    tried = 1;
    if (!SDL_WasInit(SDL_INIT_HAPTIC) && !SDL_InitSubSystem(SDL_INIT_HAPTIC))
        return NULL;
    int n = 0;
    SDL_HapticID *ids = SDL_GetHaptics(&n);
    for (int i = 0; ids && i < n && !h; i++) {
        h = SDL_OpenHaptic(ids[i]);
        if (h && !SDL_InitHapticRumble(h)) {
            SDL_CloseHaptic(h);
            h = NULL;
        }
        if (h)
            wr_log("pad: rumble through the device vibrator (%s)", SDL_GetHapticNameForID(ids[i]));
    }
    SDL_free(ids);
    return h;
}

/* Rumble on every open controller: low / high motor 0..1, for ms milliseconds
 * (a newer call replaces a running one, as SDL does). A pad without motors (the Odin's built-in one) uses the
 * device vibrator instead. */
void wr_pad_rumble(float low, float high, uint32_t ms)
{
    int any = 0;
    for (int k = 0; k < 4; k++)
        if (g_pads[k] && SDL_GetBooleanProperty(SDL_GetGamepadProperties(g_pads[k]), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false))
            any = 1;
    if (!any) {
        SDL_Haptic *h = device_haptic();
        float st = low > high ? low : high;
        if (h && st > 0)
            SDL_PlayHapticRumble(h, st > 1 ? 1.0f : st, ms);
        return;
    }
    if (low < 0) low = 0;
    if (low > 1) low = 1;
    if (high < 0) high = 0;
    if (high > 1) high = 1;
    for (int k = 0; k < 4; k++)
        if (g_pads[k] && !SDL_RumbleGamepad(g_pads[k], (Uint16)(low * 65535.0f), (Uint16)(high * 65535.0f), ms))
            WR_ONCE("pad: rumble failed: %s", SDL_GetError());
}
