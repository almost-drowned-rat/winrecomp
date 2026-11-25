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
