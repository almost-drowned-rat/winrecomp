/*
 * winrecomp runtime - USER32: windows, the message queue, timers, dialogs.
 *
 * A top-level guest window is an SDL window (Vulkan-capable); child windows
 * and dialogs only exist as records. Host events are turned into the WM_*
 * messages Windows would post, the guest's message loop takes them out with
 * PeekMessageA and DispatchMessageA calls its window procedure.
 */
#include "w32_internal.h"
#include "../platform/wr_platform.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

#define WM_CREATE        0x0001
#define WM_DESTROY       0x0002
#define WM_MOVE          0x0003
#define WM_SIZE          0x0005
#define WM_ACTIVATE      0x0006
#define WM_SETFOCUS      0x0007
#define WM_KILLFOCUS     0x0008
#define WM_PAINT         0x000F
#define WM_CLOSE         0x0010
#define WM_QUIT          0x0012
#define WM_ERASEBKGND    0x0014
#define WM_SHOWWINDOW    0x0018
#define WM_ACTIVATEAPP   0x001C
#define WM_SETCURSOR     0x0020
#define WM_NCCREATE      0x0081
#define WM_NCDESTROY     0x0082
#define WM_KEYDOWN       0x0100
#define WM_KEYUP         0x0101
#define WM_CHAR          0x0102
#define WM_SYSKEYDOWN    0x0104
#define WM_SYSKEYUP      0x0105
#define WM_SYSCHAR       0x0106
#define WM_SYSCOMMAND    0x0112
#define WM_TIMER         0x0113
#define WM_MOUSEMOVE     0x0200
#define WM_LBUTTONDOWN   0x0201
#define WM_LBUTTONUP     0x0202
#define WM_RBUTTONDOWN   0x0204
#define WM_RBUTTONUP     0x0205

#define GWL_WNDPROC   (-4)
#define GWL_HINSTANCE (-6)
#define GWL_ID        (-12)
#define GWL_STYLE     (-16)
#define GWL_EXSTYLE   (-20)
#define GWL_USERDATA  (-21)

/* ---- platform --------------------------------------------------------------------- */

static int g_sdl_up, g_headless;
static SDL_Window *g_main_sdl;

int wr_platform_headless(void) { return g_headless; }

int wr_platform_init(void)
{
    if (g_sdl_up)
        return 1;
    const char *h = getenv("WR_HEADLESS");
    g_headless = h && *h && *h != '0';
    if (g_headless) {
        g_sdl_up = 1;
        wr_log("platform: headless (no window)");
        return 1;
    }
    /* SIGINT/SIGTERM must end the process, not become an SDL quit event a
     * hung guest never reads. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        wr_log("platform: SDL_Init failed: %s", SDL_GetError());
        return 0;
    }
    g_sdl_up = 1;
    return 1;
}

void wr_platform_quit(void)
{
    if (g_sdl_up && !g_headless)
        SDL_Quit();
    g_sdl_up = 0;
}

SDL_Window *wr_main_window(void) { return g_main_sdl; }

void wr_main_window_size(int *w, int *h)
{
    *w = 640;
    *h = 480;
    if (g_main_sdl)
        SDL_GetWindowSizeInPixels(g_main_sdl, w, h);
}

void wr_display_size(int *w, int *h)
{
    *w = 1920;
    *h = 1080;
    if (g_headless || !g_sdl_up)
        return;
    const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (m) {
        *w = m->w;
        *h = m->h;
    }
}

/* ---- window records ---------------------------------------------------------------- */

typedef struct wclass { char name[64]; uint32_t wndproc, style, icon, cursor, inst; } wclass;
static wclass g_classes[32];
static int g_nclasses;

typedef struct wwin {
    int used;
    uint32_t hwnd, wndproc, parent, id, style, exstyle, userdata, inst;
    int x, y, w, h;
    char title[128];
    char cls[64];
    SDL_Window *sdl;
    int shown;
} wwin;

#define MAX_WIN 64
#define HWND_BASE 0x00010010u
static wwin g_wins[MAX_WIN];

static wwin *win_get(uint32_t h)
{
    uint32_t i = (h - HWND_BASE) / 4u;
    if (h < HWND_BASE || (h & 3u) || i >= MAX_WIN || !g_wins[i].used)
        return NULL;
    return &g_wins[i];
}

static wwin *main_win(void)
{
    for (int i = 0; i < MAX_WIN; i++)
        if (g_wins[i].used && g_wins[i].sdl)
            return &g_wins[i];
    for (int i = 0; i < MAX_WIN; i++)
        if (g_wins[i].used && !g_wins[i].parent)
            return &g_wins[i];
    return NULL;
}

/* ---- message queue ------------------------------------------------------------------ */

typedef struct wmsg { uint32_t hwnd, msg, w, l, time; int32_t x, y; uint32_t chr; } wmsg;

#define QSIZE 512
static wmsg g_q[QSIZE];
static int g_qhead, g_qlen;

static void post(uint32_t hwnd, uint32_t msg, uint32_t w, uint32_t l, uint32_t chr)
{
    if (g_qlen >= QSIZE) {
        WR_ONCE("user32: message queue full, dropping messages");
        return;
    }
    wmsg *m = &g_q[(g_qhead + g_qlen++) % QSIZE];
    m->hwnd = hwnd;
    m->msg = msg;
    m->w = w;
    m->l = l;
    m->time = wr_time_ms();
    m->x = m->y = 0;
    m->chr = chr;
}

static uint32_t call_wndproc(X86Ctx *ctx, uint32_t hwnd, uint32_t msg, uint32_t w, uint32_t l)
{
    wwin *win = win_get(hwnd);
    if (!win || !win->wndproc)
        return 0;
    uint32_t args[4] = { hwnd, msg, w, l };
    return wr_call_guest(ctx, win->wndproc, 4, args);
}

/* ---- timers -------------------------------------------------------------------------- */

typedef struct wtimer { int used; uint32_t hwnd, id, ms, proc, due, pending; } wtimer;
static wtimer g_timers[32];

static void run_timers(void)
{
    uint32_t now = wr_time_ms();
    for (int i = 0; i < 32; i++) {
        wtimer *t = &g_timers[i];
        if (!t->used || t->pending || (int32_t)(now - t->due) < 0)
            continue;
        t->pending = 1;
        t->due = now + (t->ms ? t->ms : 1);
        post(t->hwnd, WM_TIMER, t->id, t->proc, 0);
    }
}

/* ---- host events ----------------------------------------------------------------------- */

static uint32_t key_lparam(uint8_t dik, int up, int repeat)
{
    uint32_t l = 1u | (uint32_t)(dik & 0x7Fu) << 16;
    if (dik & 0x80u)
        l |= 1u << 24;
    if (up)
        l |= 3u << 30;
    else if (repeat)
        l |= 1u << 30;
    return l;
}

extern volatile uint32_t wr_heartbeat;

void w32_pump_events(X86Ctx *ctx)
{
    (void)ctx;
    wr_heartbeat++;
    run_timers();
    if (g_headless || !g_sdl_up)
        return;
    wwin *mw = main_win();
    uint32_t hwnd = mw ? mw->hwnd : 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (hwnd)
                post(hwnd, WM_CLOSE, 0, 0, 0);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            uint8_t vk, dik;
            if (!hwnd || !wr_key_from_sdl((int)e.key.scancode, &vk, &dik))
                break;
            int up = e.type == SDL_EVENT_KEY_UP;
            int alt = (e.key.mod & SDL_KMOD_ALT) != 0;
            uint32_t msg = (alt || vk == 0x79 /* F10 */ || vk == 0x12)
                               ? (up ? WM_SYSKEYUP : WM_SYSKEYDOWN)
                               : (up ? WM_KEYUP : WM_KEYDOWN);
            uint32_t chr = 0;
            if (!up) {
                SDL_Keycode k = SDL_GetKeyFromScancode(e.key.scancode, e.key.mod, false);
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) chr = '\r';
                else if (k == SDLK_BACKSPACE) chr = 8;
                else if (k == SDLK_ESCAPE) chr = 27;
                else if (k == SDLK_TAB) chr = 9;
                else if (k >= 32 && k < 0x100 && !(e.key.mod & SDL_KMOD_CTRL)) chr = (uint32_t)k;
            }
            post(hwnd, msg, vk, key_lparam(dik, up, e.key.repeat), chr);
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            if (hwnd) {
                post(hwnd, WM_ACTIVATEAPP, 1, 0, 0);
                post(hwnd, WM_ACTIVATE, 1, 0, 0);
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (hwnd) {
                post(hwnd, WM_ACTIVATE, 0, 0, 0);
                post(hwnd, WM_ACTIVATEAPP, 0, 0, 0);
            }
            break;
        default:
            break;
        }
    }
}

/* ---- class / window creation -------------------------------------------------------- */

WR_HANDLER(u_RegisterClassA)
{
    (void)base;
    uint32_t wc = WR_ARG(0);
    if (g_nclasses >= 32)
        WR_RET(0, 1);
    wclass *c = &g_classes[g_nclasses];
    c->style = wr_rd32(wc + 0);
    c->wndproc = wr_rd32(wc + 4);
    c->inst = wr_rd32(wc + 16);
    c->icon = wr_rd32(wc + 20);
    c->cursor = wr_rd32(wc + 24);
    uint32_t name = wr_rd32(wc + 36);
    if (name < 0x10000u)
        snprintf(c->name, sizeof c->name, "#%u", name);
    else
        snprintf(c->name, sizeof c->name, "%s", wr_str(name));
    wr_log("RegisterClassA(%s, wndproc 0x%08X)", c->name, c->wndproc);
    g_nclasses++;
    WR_RET(0xC000u + (uint32_t)g_nclasses, 1);
}

static wclass *find_class(const char *name)
{
    for (int i = 0; i < g_nclasses; i++)
        if (!strcasecmp(g_classes[i].name, name))
            return &g_classes[i];
    return NULL;
}

WR_HANDLER(u_CreateWindowExA)
{
    uint32_t exstyle = WR_ARG(0), cls_va = WR_ARG(1), name_va = WR_ARG(2), style = WR_ARG(3);
    int32_t x = (int32_t)WR_ARG(4), y = (int32_t)WR_ARG(5), w = (int32_t)WR_ARG(6), h = (int32_t)WR_ARG(7);
    uint32_t parent = WR_ARG(8), menu = WR_ARG(9), inst = WR_ARG(10), param = WR_ARG(11);
    char cls[64];
    if (cls_va < 0x10000u)
        snprintf(cls, sizeof cls, "#%u", cls_va);
    else
        snprintf(cls, sizeof cls, "%s", wr_str(cls_va));
    wclass *c = find_class(cls);
    int slot = -1;
    for (int i = 0; i < MAX_WIN; i++)
        if (!g_wins[i].used) {
            slot = i;
            break;
        }
    if (slot < 0)
        WR_RET(0, 12);
    wwin *win = &g_wins[slot];
    memset(win, 0, sizeof *win);
    win->used = 1;
    win->hwnd = HWND_BASE + 4u * (uint32_t)slot;
    win->wndproc = c ? c->wndproc : 0;
    win->parent = parent;
    win->id = menu;
    win->style = style;
    win->exstyle = exstyle;
    win->inst = inst;
    if (w == (int32_t)0x80000000 || w <= 0) w = 640;
    if (h == (int32_t)0x80000000 || h <= 0) h = 480;
    if (x == (int32_t)0x80000000) x = 0;
    if (y == (int32_t)0x80000000) y = 0;
    win->x = x; win->y = y; win->w = w; win->h = h;
    snprintf(win->cls, sizeof win->cls, "%s", cls);
    snprintf(win->title, sizeof win->title, "%s", name_va ? wr_str(name_va) : "");
    wr_log("CreateWindowExA(%s, \"%s\", style 0x%08X, %dx%d) -> 0x%X%s", cls, win->title, style, w, h,
           win->hwnd, c ? "" : " (unknown class)");

    if (!parent && !g_headless && g_sdl_up && !g_main_sdl) {
        win->sdl = SDL_CreateWindow(win->title[0] ? win->title : "winrecomp", w, h,
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
        if (!win->sdl)
            wr_log("CreateWindowExA: SDL_CreateWindow failed: %s", SDL_GetError());
        g_main_sdl = win->sdl;
    }

    /* CREATESTRUCTA for WM_NCCREATE / WM_CREATE */
    uint32_t cs = wr_galloc(48);
    wr_wr32(cs + 0, param);
    wr_wr32(cs + 4, inst);
    wr_wr32(cs + 8, menu);
    wr_wr32(cs + 12, parent);
    wr_wr32(cs + 16, (uint32_t)h);
    wr_wr32(cs + 20, (uint32_t)w);
    wr_wr32(cs + 24, (uint32_t)y);
    wr_wr32(cs + 28, (uint32_t)x);
    wr_wr32(cs + 32, style);
    wr_wr32(cs + 36, name_va);
    wr_wr32(cs + 40, cls_va);
    wr_wr32(cs + 44, exstyle);
    uint32_t hwnd = win->hwnd;
    uint32_t r = call_wndproc(ctx, hwnd, WM_NCCREATE, 0, cs);
    if (win_get(hwnd) && win->wndproc && !r) {
        wr_log("CreateWindowExA: WM_NCCREATE refused");
    }
    r = call_wndproc(ctx, hwnd, WM_CREATE, 0, cs);
    wr_heap_free(cs);
    if ((int32_t)r == -1) {
        wr_log("CreateWindowExA: WM_CREATE returned -1, window destroyed");
        if (win->sdl) {
            SDL_DestroyWindow(win->sdl);
            g_main_sdl = NULL;
        }
        win->used = 0;
        WR_RET(0, 12);
    }
    if (style & 0x10000000u) {   /* WS_VISIBLE */
        if (win->sdl)
            SDL_ShowWindow(win->sdl);
        win->shown = 1;
    }
    WR_RET(hwnd, 12);
}

WR_HANDLER(u_DestroyWindow)
{
    uint32_t hwnd = WR_ARG(0);
    wwin *win = win_get(hwnd);
    if (!win)
        WR_RET(0, 1);
    call_wndproc(ctx, hwnd, WM_DESTROY, 0, 0);
    call_wndproc(ctx, hwnd, WM_NCDESTROY, 0, 0);
    win = win_get(hwnd);
    if (win) {
        if (win->sdl) {
            SDL_DestroyWindow(win->sdl);
            if (g_main_sdl == win->sdl)
                g_main_sdl = NULL;
        }
        win->used = 0;
    }
    WR_RET(1, 1);
}

WR_HANDLER(u_ShowWindow)
{
    uint32_t hwnd = WR_ARG(0), cmd = WR_ARG(1);
    wwin *win = win_get(hwnd);
    if (!win)
        WR_RET(0, 2);
    int was = win->shown;
    if (cmd == 0) {   /* SW_HIDE */
        if (win->sdl)
            SDL_HideWindow(win->sdl);
        win->shown = 0;
        WR_RET(was, 2);
    }
    if (win->sdl)
        SDL_ShowWindow(win->sdl);
    win->shown = 1;
    if (!was) {
        call_wndproc(ctx, hwnd, WM_SHOWWINDOW, 1, 0);
        call_wndproc(ctx, hwnd, WM_ACTIVATEAPP, 1, 0);
        call_wndproc(ctx, hwnd, WM_ACTIVATE, 1, 0);
        call_wndproc(ctx, hwnd, WM_SETFOCUS, 0, 0);
        call_wndproc(ctx, hwnd, WM_SIZE, 0, (uint32_t)win->h << 16 | (uint32_t)(win->w & 0xFFFF));
    }
    WR_RET(was, 2);
}

WR_HANDLER(u_UpdateWindow) { (void)ctx; (void)base; WR_RET(1, 1); }
WR_HANDLER(u_SetForegroundWindow)
{
    (void)base;
    wwin *win = win_get(WR_ARG(0));
    if (win && win->sdl)
        SDL_RaiseWindow(win->sdl);
    WR_RET(win != NULL, 1);
}
WR_HANDLER(u_EnableWindow) { (void)ctx; (void)base; WR_RET(0, 2); }
WR_HANDLER(u_GetParent)
{
    (void)base;
    wwin *win = win_get(WR_ARG(0));
    WR_RET(win ? win->parent : 0, 1);
}
WR_HANDLER(u_GetDlgCtrlID)
{
    (void)base;
    wwin *win = win_get(WR_ARG(0));
    WR_RET(win ? win->id : 0, 1);
}

WR_HANDLER(u_FindWindowExA)
{
    (void)base;
    const char *cls = WR_ARG(2) >= 0x10000u ? wr_str(WR_ARG(2)) : NULL;
    const char *name = wr_str(WR_ARG(3));
    WR_TRACE("FindWindowExA(%s, %s) -> 0", cls ? cls : "", name ? name : "");
    WR_RET(0, 4);   /* no other instance, no foreign windows */
}

WR_HANDLER(u_GetWindowLongA)
{
    (void)base;
    wwin *win = win_get(WR_ARG(0));
    int32_t idx = (int32_t)WR_ARG(1);
    if (!win)
        WR_RET(0, 2);
    uint32_t v = 0;
    switch (idx) {
    case GWL_WNDPROC: v = win->wndproc; break;
    case GWL_HINSTANCE: v = win->inst; break;
    case GWL_ID: v = win->id; break;
    case GWL_STYLE: v = win->style; break;
    case GWL_EXSTYLE: v = win->exstyle; break;
    case GWL_USERDATA: v = win->userdata; break;
    default: WR_ONCE("GetWindowLongA: index %d", idx); break;
    }
    WR_RET(v, 2);
}

WR_HANDLER(u_SetWindowLongA)
{
    (void)base;
    wwin *win = win_get(WR_ARG(0));
    int32_t idx = (int32_t)WR_ARG(1);
    uint32_t v = WR_ARG(2), old = 0;
    if (!win)
        WR_RET(0, 3);
    switch (idx) {
    case GWL_WNDPROC: old = win->wndproc; win->wndproc = v; break;
    case GWL_ID: old = win->id; win->id = v; break;
    case GWL_STYLE: old = win->style; win->style = v; break;
    case GWL_EXSTYLE: old = win->exstyle; win->exstyle = v; break;
    case GWL_USERDATA: old = win->userdata; win->userdata = v; break;
    default: WR_ONCE("SetWindowLongA: index %d", idx); break;
    }
    WR_RET(old, 3);
}

static void write_rect(uint32_t p, int l, int t, int r, int b)
{
    wr_wr32(p + 0, (uint32_t)l);
    wr_wr32(p + 4, (uint32_t)t);
    wr_wr32(p + 8, (uint32_t)r);
    wr_wr32(p + 12, (uint32_t)b);
}

WR_HANDLER(u_GetWindowRect)
{
    (void)base;
    wwin *win = win_get(WR_ARG(0));
    if (!win)
        WR_RET(0, 2);
    write_rect(WR_ARG(1), win->x, win->y, win->x + win->w, win->y + win->h);
    WR_RET(1, 2);
}

WR_HANDLER(u_GetClientRect)
{
    (void)base;
    wwin *win = win_get(WR_ARG(0));
    if (!win)
        WR_RET(0, 2);
    write_rect(WR_ARG(1), 0, 0, win->w, win->h);
    WR_RET(1, 2);
}

WR_HANDLER(u_SetWindowPos)
{
    (void)base;
    wwin *win = win_get(WR_ARG(0));
    uint32_t flags = WR_ARG(6);
    if (!win)
        WR_RET(0, 7);
    if (!(flags & 0x2u)) {   /* SWP_NOMOVE */
        win->x = (int32_t)WR_ARG(2);
        win->y = (int32_t)WR_ARG(3);
    }
    if (!(flags & 0x1u)) {   /* SWP_NOSIZE */
        int32_t w = (int32_t)WR_ARG(4), h = (int32_t)WR_ARG(5);
        if (w > 0 && h > 0) {
            win->w = w;
            win->h = h;
        }
    }
    WR_TRACE("SetWindowPos(0x%X, %d,%d %dx%d, flags 0x%X)", win->hwnd, win->x, win->y, win->w, win->h, flags);
    WR_RET(1, 7);
}

WR_HANDLER(u_SetWindowTextA)
{
    (void)base;
    wwin *win = win_get(WR_ARG(0));
    if (!win)
        WR_RET(0, 2);
    snprintf(win->title, sizeof win->title, "%s", wr_str(WR_ARG(1)) ? wr_str(WR_ARG(1)) : "");
    if (win->sdl)
        SDL_SetWindowTitle(win->sdl, win->title);
    WR_RET(1, 2);
}

WR_HANDLER(u_GetWindowTextA)
{
    (void)base;
    wwin *win = win_get(WR_ARG(0));
    if (!win || !WR_ARG(2))
        WR_RET(0, 3);
    WR_RET(wr_put_str(WR_ARG(1), WR_ARG(2), win->title), 3);
}

/* ---- messages ----------------------------------------------------------------------- */

static int msg_match(const wmsg *m, uint32_t hwnd, uint32_t lo, uint32_t hi)
{
    if (hwnd && hwnd != 0xFFFFFFFFu && m->hwnd != hwnd)
        return 0;
    if ((lo || hi) && (m->msg < lo || m->msg > hi))
        return 0;
    return 1;
}

static void write_msg(uint32_t p, const wmsg *m)
{
    wr_wr32(p + 0, m->hwnd);
    wr_wr32(p + 4, m->msg);
    wr_wr32(p + 8, m->w);
    wr_wr32(p + 12, m->l);
    wr_wr32(p + 16, m->time);
    wr_wr32(p + 20, (uint32_t)m->x);
    wr_wr32(p + 24, (uint32_t)m->y);
}

/* The character TranslateMessage will produce for the key message it is
 * handed: remembered per dispatched key-down, keyed by its time + lParam. */
static uint32_t g_last_key_l, g_last_key_t, g_last_chr;

WR_HANDLER(u_PeekMessageA)
{
    (void)base;
    uint32_t out = WR_ARG(0), hwnd = WR_ARG(1), lo = WR_ARG(2), hi = WR_ARG(3), remove = WR_ARG(4);
    w32_pump_events(ctx);
    for (int i = 0; i < g_qlen; i++) {
        wmsg *m = &g_q[(g_qhead + i) % QSIZE];
        if (!msg_match(m, hwnd, lo, hi))
            continue;
        wmsg copy = *m;
        if (remove & 1u) {
            for (int k = i; k > 0; k--)
                g_q[(g_qhead + k) % QSIZE] = g_q[(g_qhead + k - 1) % QSIZE];
            g_qhead = (g_qhead + 1) % QSIZE;
            g_qlen--;
            if (copy.msg == WM_TIMER)
                for (int t = 0; t < 32; t++)
                    if (g_timers[t].used && g_timers[t].hwnd == copy.hwnd && g_timers[t].id == copy.w)
                        g_timers[t].pending = 0;
        }
        if (copy.msg == WM_KEYDOWN || copy.msg == WM_SYSKEYDOWN) {
            g_last_key_l = copy.l;
            g_last_key_t = copy.time;
            g_last_chr = copy.chr;
        }
        write_msg(out, &copy);
        WR_RET(1, 5);
    }
    WR_RET(0, 5);
}

WR_HANDLER(u_TranslateMessage)
{
    (void)base;
    uint32_t p = WR_ARG(0);
    uint32_t msg = wr_rd32(p + 4), l = wr_rd32(p + 12), t = wr_rd32(p + 16);
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && l == g_last_key_l && t == g_last_key_t && g_last_chr) {
        post(wr_rd32(p), msg == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR, g_last_chr, l, 0);
        g_last_chr = 0;
        WR_RET(1, 1);
    }
    WR_RET(0, 1);
}

WR_HANDLER(u_DispatchMessageA)
{
    (void)base;
    uint32_t p = WR_ARG(0);
    uint32_t hwnd = wr_rd32(p), msg = wr_rd32(p + 4), w = wr_rd32(p + 8), l = wr_rd32(p + 12);
    if (msg == WM_TIMER && l) {
        uint32_t args[4] = { hwnd, WM_TIMER, w, wr_time_ms() };
        wr_call_guest(ctx, l, 4, args);
        WR_RET(0, 1);
    }
    WR_RET(call_wndproc(ctx, hwnd, msg, w, l), 1);
}

WR_HANDLER(u_PostMessageA)
{
    (void)ctx; (void)base;
    post(WR_ARG(0), WR_ARG(1), WR_ARG(2), WR_ARG(3), 0);
    WR_RET(1, 4);
}

WR_HANDLER(u_SendMessageA)
{
    (void)base;
    WR_RET(call_wndproc(ctx, WR_ARG(0), WR_ARG(1), WR_ARG(2), WR_ARG(3)), 4);
}

WR_HANDLER(u_PostQuitMessage)
{
    (void)ctx; (void)base;
    wwin *mw = main_win();
    post(mw ? mw->hwnd : 0, WM_QUIT, WR_ARG(0), 0, 0);
    WR_RET(0, 1);
}

WR_HANDLER(u_CallWindowProcA)
{
    (void)base;
    uint32_t proc = WR_ARG(0);
    uint32_t args[4] = { WR_ARG(1), WR_ARG(2), WR_ARG(3), WR_ARG(4) };
    WR_RET(proc ? wr_call_guest(ctx, proc, 4, args) : 0, 5);
}

WR_HANDLER(u_DefWindowProcA)
{
    uint32_t hwnd = WR_ARG(0), msg = WR_ARG(1);
    uint32_t r = 0;
    switch (msg) {
    case WM_CLOSE: {
        /* DestroyWindow, inline: the guest's WM_DESTROY usually posts WM_QUIT */
        uint32_t save = g_esp;
        call_wndproc(ctx, hwnd, WM_DESTROY, 0, 0);
        g_esp = save;
        wwin *win = win_get(hwnd);
        if (win) {
            if (win->sdl) {
                SDL_DestroyWindow(win->sdl);
                if (g_main_sdl == win->sdl)
                    g_main_sdl = NULL;
            }
            win->used = 0;
        }
        break;
    }
    case WM_NCCREATE: r = 1; break;
    case WM_ERASEBKGND: r = 1; break;
    case WM_SETCURSOR: r = 0; break;
    case WM_SYSCOMMAND:
        if ((WR_ARG(2) & 0xFFF0u) == 0xF060u)   /* SC_CLOSE */
            post(hwnd, WM_CLOSE, 0, 0, 0);
        break;
    default: break;
    }
    (void)base;
    WR_RET(r, 4);
}

/* ---- timers --------------------------------------------------------------------------- */

WR_HANDLER(u_SetTimer)
{
    (void)ctx; (void)base;
    uint32_t hwnd = WR_ARG(0), id = WR_ARG(1), ms = WR_ARG(2), proc = WR_ARG(3);
    for (int i = 0; i < 32; i++)
        if (g_timers[i].used && g_timers[i].hwnd == hwnd && g_timers[i].id == id) {
            g_timers[i].ms = ms;
            g_timers[i].proc = proc;
            g_timers[i].due = wr_time_ms() + ms;
            WR_RET(id ? id : 1, 4);
        }
    for (int i = 0; i < 32; i++)
        if (!g_timers[i].used) {
            if (!hwnd && !id)
                id = 0x100u + (uint32_t)i;
            g_timers[i] = (wtimer){ 1, hwnd, id, ms, proc, wr_time_ms() + ms, 0 };
            WR_TRACE("SetTimer(0x%X, %u, %u ms)", hwnd, id, ms);
            WR_RET(id, 4);
        }
    WR_RET(0, 4);
}

WR_HANDLER(u_KillTimer)
{
    (void)ctx; (void)base;
    for (int i = 0; i < 32; i++)
        if (g_timers[i].used && g_timers[i].hwnd == WR_ARG(0) && g_timers[i].id == WR_ARG(1)) {
            g_timers[i].used = 0;
            WR_RET(1, 2);
        }
    WR_RET(0, 2);
}

/* ---- dialogs (records only; the guest's own dialogs are not shown) ------------------- */

WR_HANDLER(u_DialogBoxParamA)
{
    (void)base;
    uint32_t tmpl = WR_ARG(1);
    wr_log("DialogBoxParamA(template %s%u, proc 0x%08X): dialogs are not shown, answering IDCANCEL",
           tmpl < 0x10000u ? "#" : "", tmpl < 0x10000u ? tmpl : 0, WR_ARG(3));
    WR_RET(2, 5);
}
WR_HANDLER(u_EndDialog) { (void)ctx; (void)base; WR_RET(1, 2); }
WR_HANDLER(u_GetDlgItem) { (void)ctx; (void)base; WR_RET(0, 2); }
WR_HANDLER(u_SetDlgItemTextA) { (void)ctx; (void)base; WR_RET(0, 3); }
WR_HANDLER(u_GetDlgItemTextA)
{
    (void)base;
    if (WR_ARG(3))
        wr_wr8(WR_ARG(2), 0);
    WR_RET(0, 4);
}
WR_HANDLER(u_SendDlgItemMessageA) { (void)ctx; (void)base; WR_RET(0, 5); }

/* ---- misc ------------------------------------------------------------------------------- */

WR_HANDLER(u_MessageBoxA)
{
    (void)base;
    const char *text = wr_str(WR_ARG(1)), *cap = wr_str(WR_ARG(2));
    uint32_t type = WR_ARG(3) & 0xFu;
    wr_log("MessageBoxA [%s] %s (type 0x%X)", cap ? cap : "", text ? text : "", WR_ARG(3));
    if (!g_headless && g_sdl_up && (WR_ARG(3) & 0x10u))   /* MB_ICONHAND: an error */
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, cap ? cap : "", text ? text : "", g_main_sdl);
    uint32_t r = type == 4 || type == 3 ? 6u /* IDYES */ : 1u /* IDOK */;
    WR_RET(r, 4);
}

WR_HANDLER(u_GetSystemMetrics)
{
    (void)base;
    int w, h;
    wr_display_size(&w, &h);
    uint32_t i = WR_ARG(0), v = 0;
    switch (i) {
    case 0: v = (uint32_t)w; break;        /* SM_CXSCREEN */
    case 1: v = (uint32_t)h; break;        /* SM_CYSCREEN */
    case 4: v = 19; break;                 /* SM_CYCAPTION */
    case 5: case 6: v = 1; break;          /* SM_CXBORDER / SM_CYBORDER */
    case 7: case 8: v = 3; break;          /* SM_CXDLGFRAME */
    case 16: v = (uint32_t)w; break;       /* SM_CXFULLSCREEN */
    case 17: v = (uint32_t)h - 19; break;
    case 32: case 33: v = 4; break;        /* SM_CXFRAME */
    case 43: v = 3; break;                 /* SM_CMOUSEBUTTONS */
    case 80: v = 1; break;                 /* SM_CMONITORS */
    default: WR_ONCE("GetSystemMetrics(%u) answered 0", i); break;
    }
    WR_RET(v, 1);
}

static int g_cursor_count;
WR_HANDLER(u_ShowCursor)
{
    (void)ctx; (void)base;
    g_cursor_count += WR_ARG(0) ? 1 : -1;
    if (!g_headless && g_sdl_up) {
        if (g_cursor_count >= 0) SDL_ShowCursor(); else SDL_HideCursor();
    }
    WR_RET((uint32_t)g_cursor_count, 1);
}

WR_HANDLER(u_LoadIconA) { (void)ctx; (void)base; WR_RET(0x00020001u, 2); }
WR_HANDLER(u_LoadCursorA) { (void)ctx; (void)base; WR_RET(0x00020003u, 2); }
WR_HANDLER(u_GetDC) { (void)ctx; (void)base; WR_RET(0x00030001u, 1); }
WR_HANDLER(u_ReleaseDC) { (void)ctx; (void)base; WR_RET(1, 2); }

WR_HANDLER(u_GetKeyboardLayoutNameA)
{
    (void)ctx; (void)base;
    wr_put_str(WR_ARG(0), 9, "00000409");
    WR_RET(1, 1);
}

/* wsprintfA(buf, fmt, ...): cdecl. %[-][0][width][.prec][l|h]{d,i,u,x,X,c,s,%} */
WR_HANDLER(u_wsprintfA)
{
    (void)base;
    uint32_t out = WR_ARG(0);
    const char *f = wr_str(WR_ARG(1));
    char buf[1025];
    size_t n = 0;
    int ai = 2;
    while (f && *f && n < 1024) {
        if (*f != '%') {
            buf[n++] = *f++;
            continue;
        }
        const char *start = f++;
        char spec[32];
        size_t sl = 0;
        spec[sl++] = '%';
        while (*f && strchr("-+ #0", *f) && sl < 20) spec[sl++] = *f++;
        while (*f >= '0' && *f <= '9' && sl < 24) spec[sl++] = *f++;
        if (*f == '.') { spec[sl++] = *f++; while (*f >= '0' && *f <= '9' && sl < 28) spec[sl++] = *f++; }
        while (*f == 'l' || *f == 'h') f++;
        char conv = *f ? *f++ : 0;
        char piece[512];
        int pl = 0;
        spec[sl] = 0;
        switch (conv) {
        case 'd': case 'i': spec[sl] = 'd'; spec[sl + 1] = 0; pl = snprintf(piece, sizeof piece, spec, (int32_t)WR_ARG(ai++)); break;
        case 'u': spec[sl] = 'u'; spec[sl + 1] = 0; pl = snprintf(piece, sizeof piece, spec, WR_ARG(ai++)); break;
        case 'x': case 'X': spec[sl] = conv; spec[sl + 1] = 0; pl = snprintf(piece, sizeof piece, spec, WR_ARG(ai++)); break;
        case 'c': piece[0] = (char)WR_ARG(ai++); pl = 1; break;
        case 's': { const char *s = wr_str(WR_ARG(ai++)); spec[sl] = 's'; spec[sl + 1] = 0;
                    pl = snprintf(piece, sizeof piece, spec, s ? s : "(null)"); break; }
        case '%': piece[0] = '%'; pl = 1; break;
        default: pl = (int)(f - start); memcpy(piece, start, (size_t)pl); break;
        }
        if (pl > (int)sizeof piece - 1) pl = (int)sizeof piece - 1;
        for (int k = 0; k < pl && n < 1024; k++)
            buf[n++] = piece[k];
    }
    buf[n] = 0;
    wr_put_str(out, 1025, buf);
    WR_RET_C(n);
}

#define E(n) { #n, u_##n }
static const wr_export u_exports[] = {
    E(PostMessageA), E(FindWindowExA), E(CreateWindowExA), E(LoadIconA), E(LoadCursorA), E(RegisterClassA),
    E(GetSystemMetrics), E(PeekMessageA), E(TranslateMessage), E(DispatchMessageA), E(PostQuitMessage),
    E(SetForegroundWindow), E(ShowWindow), E(SendDlgItemMessageA), E(SendMessageA), E(GetDlgItem),
    E(SetDlgItemTextA), E(GetDlgItemTextA), E(SetWindowTextA), E(GetWindowTextA), E(MessageBoxA),
    E(EnableWindow), E(EndDialog), E(GetWindowLongA), E(CallWindowProcA), E(GetParent), E(GetDlgCtrlID),
    E(KillTimer), E(SetWindowLongA), E(GetWindowRect), E(SetTimer), E(DialogBoxParamA), E(DestroyWindow),
    E(GetKeyboardLayoutNameA), E(wsprintfA), E(ShowCursor), E(UpdateWindow), E(SetWindowPos), E(ReleaseDC),
    E(GetDC), E(GetClientRect), E(DefWindowProcA),
    { NULL, NULL }
};
#undef E

static wr_module u_module = { "USER32.dll", u_exports, 0 };

void w32_register_user32(void) { wr_module_register(&u_module); }

/* Main window handle for other modules (DirectInput cooperative level etc.). */
uint32_t w32_main_hwnd(void)
{
    wwin *w = main_win();
    return w ? w->hwnd : 0;
}

/* Client size of a window (for native video modules). */
int w32_window_client(uint32_t hwnd, int *w, int *h)
{
    wwin *win = win_get(hwnd);
    if (!win)
        return 0;
    *w = win->w;
    *h = win->h;
    return 1;
}
