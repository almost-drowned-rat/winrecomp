/* winrecomp runtime - shared helpers of the Win32 layer. */
#ifndef W32_INTERNAL_H
#define W32_INTERNAL_H

#include "wr.h"

#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
#  define strtok_r strtok_s
#  define strcasecmp _stricmp
#  define strncasecmp _strnicmp
#else
#  include <strings.h>
#endif

#define W32_TRUE  1u
#define W32_FALSE 0u
#define W32_INVALID_HANDLE 0xFFFFFFFFu

/* Common error codes */
#define ERROR_SUCCESS             0
#define ERROR_FILE_NOT_FOUND      2
#define ERROR_PATH_NOT_FOUND      3
#define ERROR_ACCESS_DENIED       5
#define ERROR_INVALID_HANDLE      6
#define ERROR_NOT_ENOUGH_MEMORY   8
#define ERROR_NO_MORE_FILES       18
#define ERROR_FILE_EXISTS         80
#define ERROR_INVALID_PARAMETER   87
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_MOD_NOT_FOUND       126
#define ERROR_PROC_NOT_FOUND      127
#define ERROR_ALREADY_EXISTS      183
#define ERROR_MORE_DATA           234
#define ERROR_NO_MORE_ITEMS       259

/* CP1252 <-> UTF-16 for 0x80..0x9F; everything else is Latin-1. */
uint16_t w32_cp1252_to_u16(uint8_t c);
uint8_t  w32_u16_to_cp1252(uint16_t w, int *defaulted);

/* Character class bits (CT_CTYPE1) for a Latin-1 code point. */
uint16_t w32_ctype1(uint16_t c);
int      w32_tolower(int c);
int      w32_toupper(int c);

/* Registry persistence (advapi32): save at exit. */
void w32_registry_save(void);
void w32_registry_load(void);

/* Window system (user32): pump host events into the guest message queue. */
void w32_pump_events(X86Ctx *ctx);

/* Process exit: save state, quit the window system, exit. */
void w32_exit(uint32_t code);

#endif
