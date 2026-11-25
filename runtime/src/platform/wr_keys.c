/*
 * winrecomp runtime - SDL scancode -> Windows virtual key and PC set-1 scan
 * code (= DirectInput DIK_* value; 0x80 | code marks an E0-extended key).
 */
#include "wr_platform.h"

#include <SDL3/SDL.h>

typedef struct keymap { int sdl; uint8_t vk; uint8_t dik; } keymap;

static const keymap k_keys[] = {
    { SDL_SCANCODE_ESCAPE, 0x1B, 0x01 },
    { SDL_SCANCODE_1, '1', 0x02 }, { SDL_SCANCODE_2, '2', 0x03 }, { SDL_SCANCODE_3, '3', 0x04 },
    { SDL_SCANCODE_4, '4', 0x05 }, { SDL_SCANCODE_5, '5', 0x06 }, { SDL_SCANCODE_6, '6', 0x07 },
    { SDL_SCANCODE_7, '7', 0x08 }, { SDL_SCANCODE_8, '8', 0x09 }, { SDL_SCANCODE_9, '9', 0x0A },
    { SDL_SCANCODE_0, '0', 0x0B },
    { SDL_SCANCODE_MINUS, 0xBD, 0x0C }, { SDL_SCANCODE_EQUALS, 0xBB, 0x0D },
    { SDL_SCANCODE_BACKSPACE, 0x08, 0x0E }, { SDL_SCANCODE_TAB, 0x09, 0x0F },
    { SDL_SCANCODE_Q, 'Q', 0x10 }, { SDL_SCANCODE_W, 'W', 0x11 }, { SDL_SCANCODE_E, 'E', 0x12 },
    { SDL_SCANCODE_R, 'R', 0x13 }, { SDL_SCANCODE_T, 'T', 0x14 }, { SDL_SCANCODE_Y, 'Y', 0x15 },
    { SDL_SCANCODE_U, 'U', 0x16 }, { SDL_SCANCODE_I, 'I', 0x17 }, { SDL_SCANCODE_O, 'O', 0x18 },
    { SDL_SCANCODE_P, 'P', 0x19 },
    { SDL_SCANCODE_LEFTBRACKET, 0xDB, 0x1A }, { SDL_SCANCODE_RIGHTBRACKET, 0xDD, 0x1B },
    { SDL_SCANCODE_RETURN, 0x0D, 0x1C }, { SDL_SCANCODE_LCTRL, 0x11, 0x1D },
    { SDL_SCANCODE_A, 'A', 0x1E }, { SDL_SCANCODE_S, 'S', 0x1F }, { SDL_SCANCODE_D, 'D', 0x20 },
    { SDL_SCANCODE_F, 'F', 0x21 }, { SDL_SCANCODE_G, 'G', 0x22 }, { SDL_SCANCODE_H, 'H', 0x23 },
    { SDL_SCANCODE_J, 'J', 0x24 }, { SDL_SCANCODE_K, 'K', 0x25 }, { SDL_SCANCODE_L, 'L', 0x26 },
    { SDL_SCANCODE_SEMICOLON, 0xBA, 0x27 }, { SDL_SCANCODE_APOSTROPHE, 0xDE, 0x28 },
    { SDL_SCANCODE_GRAVE, 0xC0, 0x29 }, { SDL_SCANCODE_LSHIFT, 0x10, 0x2A },
    { SDL_SCANCODE_BACKSLASH, 0xDC, 0x2B },
    { SDL_SCANCODE_Z, 'Z', 0x2C }, { SDL_SCANCODE_X, 'X', 0x2D }, { SDL_SCANCODE_C, 'C', 0x2E },
    { SDL_SCANCODE_V, 'V', 0x2F }, { SDL_SCANCODE_B, 'B', 0x30 }, { SDL_SCANCODE_N, 'N', 0x31 },
    { SDL_SCANCODE_M, 'M', 0x32 },
    { SDL_SCANCODE_COMMA, 0xBC, 0x33 }, { SDL_SCANCODE_PERIOD, 0xBE, 0x34 },
    { SDL_SCANCODE_SLASH, 0xBF, 0x35 }, { SDL_SCANCODE_RSHIFT, 0x10, 0x36 },
    { SDL_SCANCODE_KP_MULTIPLY, 0x6A, 0x37 }, { SDL_SCANCODE_LALT, 0x12, 0x38 },
    { SDL_SCANCODE_SPACE, 0x20, 0x39 }, { SDL_SCANCODE_CAPSLOCK, 0x14, 0x3A },
    { SDL_SCANCODE_F1, 0x70, 0x3B }, { SDL_SCANCODE_F2, 0x71, 0x3C }, { SDL_SCANCODE_F3, 0x72, 0x3D },
    { SDL_SCANCODE_F4, 0x73, 0x3E }, { SDL_SCANCODE_F5, 0x74, 0x3F }, { SDL_SCANCODE_F6, 0x75, 0x40 },
    { SDL_SCANCODE_F7, 0x76, 0x41 }, { SDL_SCANCODE_F8, 0x77, 0x42 }, { SDL_SCANCODE_F9, 0x78, 0x43 },
    { SDL_SCANCODE_F10, 0x79, 0x44 },
    { SDL_SCANCODE_NUMLOCKCLEAR, 0x90, 0x45 }, { SDL_SCANCODE_SCROLLLOCK, 0x91, 0x46 },
    { SDL_SCANCODE_KP_7, 0x67, 0x47 }, { SDL_SCANCODE_KP_8, 0x68, 0x48 }, { SDL_SCANCODE_KP_9, 0x69, 0x49 },
    { SDL_SCANCODE_KP_MINUS, 0x6D, 0x4A },
    { SDL_SCANCODE_KP_4, 0x64, 0x4B }, { SDL_SCANCODE_KP_5, 0x65, 0x4C }, { SDL_SCANCODE_KP_6, 0x66, 0x4D },
    { SDL_SCANCODE_KP_PLUS, 0x6B, 0x4E },
    { SDL_SCANCODE_KP_1, 0x61, 0x4F }, { SDL_SCANCODE_KP_2, 0x62, 0x50 }, { SDL_SCANCODE_KP_3, 0x63, 0x51 },
    { SDL_SCANCODE_KP_0, 0x60, 0x52 }, { SDL_SCANCODE_KP_PERIOD, 0x6E, 0x53 },
    { SDL_SCANCODE_NONUSBACKSLASH, 0xE2, 0x56 },
    { SDL_SCANCODE_F11, 0x7A, 0x57 }, { SDL_SCANCODE_F12, 0x7B, 0x58 },
    /* E0-extended keys */
    { SDL_SCANCODE_KP_ENTER, 0x0D, 0x9C }, { SDL_SCANCODE_RCTRL, 0x11, 0x9D },
    { SDL_SCANCODE_KP_DIVIDE, 0x6F, 0xB5 }, { SDL_SCANCODE_PRINTSCREEN, 0x2C, 0xB7 },
    { SDL_SCANCODE_RALT, 0x12, 0xB8 }, { SDL_SCANCODE_PAUSE, 0x13, 0xC5 },
    { SDL_SCANCODE_HOME, 0x24, 0xC7 }, { SDL_SCANCODE_UP, 0x26, 0xC8 }, { SDL_SCANCODE_PAGEUP, 0x21, 0xC9 },
    { SDL_SCANCODE_LEFT, 0x25, 0xCB }, { SDL_SCANCODE_RIGHT, 0x27, 0xCD }, { SDL_SCANCODE_END, 0x23, 0xCF },
    { SDL_SCANCODE_DOWN, 0x28, 0xD0 }, { SDL_SCANCODE_PAGEDOWN, 0x22, 0xD1 },
    { SDL_SCANCODE_INSERT, 0x2D, 0xD2 }, { SDL_SCANCODE_DELETE, 0x2E, 0xD3 },
    { SDL_SCANCODE_LGUI, 0x5B, 0xDB }, { SDL_SCANCODE_RGUI, 0x5C, 0xDC }, { SDL_SCANCODE_APPLICATION, 0x5D, 0xDD },
};

int wr_key_from_sdl(int sdl_scancode, uint8_t *vk, uint8_t *dik)
{
    for (size_t i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        if (k_keys[i].sdl == sdl_scancode) {
            if (vk) *vk = k_keys[i].vk;
            if (dik) *dik = k_keys[i].dik;
            return 1;
        }
    return 0;
}

void wr_keys_dik_state(uint8_t out[256])
{
    int n = 0;
    const bool *st = SDL_GetKeyboardState(&n);
    for (int i = 0; i < 256; i++)
        out[i] = 0;
    for (size_t i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        if (k_keys[i].sdl < n && st[k_keys[i].sdl])
            out[k_keys[i].dik] = 0x80;
}

int wr_key_vk_down(uint8_t vk)
{
    int n = 0;
    const bool *st = SDL_GetKeyboardState(&n);
    for (size_t i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        if (k_keys[i].vk == vk && k_keys[i].sdl < n && st[k_keys[i].sdl])
            return 1;
    return 0;
}
