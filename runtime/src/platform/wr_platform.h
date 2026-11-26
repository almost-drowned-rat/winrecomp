/* winrecomp runtime - host platform layer (SDL3). */
#ifndef WR_PLATFORM_H
#define WR_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;

/* Keyboard: SDL scancode -> Windows VK + set-1 scan code (DIK). */
int  wr_key_from_sdl(int sdl_scancode, uint8_t *vk, uint8_t *dik);
void wr_keys_dik_state(uint8_t out[256]);
int  wr_key_vk_down(uint8_t vk);

/* Game controllers -> virtual keys. Controls 0..14 are SDL gamepad buttons
 * (SOUTH 0, EAST 1, WEST 2, NORTH 3, BACK 4, GUIDE 5, START 6, LSTICK 7,
 * RSTICK 8, LB 9, RB 10, DPAD up 11 / down 12 / left 13 / right 14); the
 * analog directions and triggers follow. The port maps each to a DIK code
 * (0 = unused). */
enum {
    WR_PAD_SOUTH = 0, WR_PAD_EAST, WR_PAD_WEST, WR_PAD_NORTH, WR_PAD_BACK, WR_PAD_GUIDE, WR_PAD_START,
    WR_PAD_LSTICK, WR_PAD_RSTICK, WR_PAD_LB, WR_PAD_RB, WR_PAD_DPAD_UP, WR_PAD_DPAD_DOWN, WR_PAD_DPAD_LEFT,
    WR_PAD_DPAD_RIGHT,
    WR_PAD_LEFT_STICK_LEFT = 32, WR_PAD_LEFT_STICK_RIGHT, WR_PAD_LEFT_STICK_UP, WR_PAD_LEFT_STICK_DOWN,
    WR_PAD_LT, WR_PAD_RT, WR_PAD_N
};
typedef uint8_t (*wr_pad_map_fn)(int control);
void wr_pad_set_map(wr_pad_map_fn fn);
void wr_pad_event(const void *sdl_event);
void wr_pad_tick(void (*post_key)(uint8_t vk, uint8_t dik, int up));
void wr_pad_dik(uint8_t st[256]);
/* left stick of the first pad, -1..1 (x right, y down); 0 = no pad */
int  wr_pad_stick(float *x, float *y);
int  wr_pad_count(void);

/* Scripted test input (WR_INPUT). */
void wr_input_script_tick(void (*post_key)(uint8_t vk, uint8_t dik, int up));
void wr_input_script_dik(uint8_t st[256]);

/* Host windowing. wr_platform_init starts SDL video/events/gamepads once;
 * headless runs (WR_HEADLESS=1) create no window. */
int  wr_platform_init(void);
int  wr_platform_headless(void);
void wr_platform_quit(void);

/* The game's main (top-level) window, or NULL. The renderer draws here. */
struct SDL_Window *wr_main_window(void);
/* Pixel size of the main window's drawable area. */
void wr_main_window_size(int *w, int *h);

/* Desktop mode for GetSystemMetrics. */
void wr_display_size(int *w, int *h);

#ifdef __cplusplus
}
#endif
#endif
