/*
 * winrecomp runtime - core interface.
 *
 * A lifted Win32 PE runs against this runtime: its image is mapped into a
 * 4 GiB guest address space at its own ImageBase, its import address table is
 * filled with synthetic addresses (0xFE000000 + 4*n) that recomp_lookup_kernel
 * resolves to native handlers, and every handler works on the guest register
 * set (X86Ctx) the generated code passes along.
 *
 * Guest address map (32-bit):
 *   0x00000000-0x0000FFFF  no access (null pointer guard)
 *   0x00010000-0x003EFFFF  main thread stack (grows down from WR_STACK_TOP)
 *   ImageBase ...          the PE image (gta2.exe: 0x00400000-0x00678000)
 *   0x01000000-0x0FEFFFFF  heap arena (HeapAlloc / LocalAlloc / runtime data)
 *   0x10000000-0x3FFFFFFF  further PE modules at their image bases (binkw32 ...)
 *   0x40000000-0x7FFCFFFF  VirtualAlloc regions
 *   0x7FFD0000-0x7FFDFFFF  PEB / TEB
 *   0xFE000000-0xFE00FFFF  synthetic import / COM method addresses (not memory)
 */
#ifndef WR_H
#define WR_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "recomp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- guest address space ------------------------------------------------ */

#define WR_GUEST_SPAN      0x100000000ull
#define WR_NULL_GUARD      0x00010000u
#define WR_STACK_TOP       0x003F0000u
#define WR_STACK_SIZE      0x00300000u
#define WR_HEAP_LO         0x01000000u
#define WR_HEAP_HI         0x0FF00000u
#define WR_MODULES_LO      0x10000000u
#define WR_MODULES_HI      0x40000000u
#define WR_VMEM_LO         0x40000000u
#define WR_VMEM_HI         0x7FFD0000u
#define WR_PEB_VA          0x7FFD0000u
#define WR_TEB_VA          0x7FFDE000u
#define WR_THUNK_BASE      0xFE000000u
#define WR_THUNK_MAX       0x4000u

/* Reserve the guest space; sets g_xbox_mem_offset. 0 on failure. */
int  wr_mem_init(void);
/* Make [va, va+size) readable/writable (no-op where already committed). */
int  wr_mem_commit(uint32_t va, uint32_t size);
/* Give pages back; they read as zero when committed again. */
void wr_mem_decommit(uint32_t va, uint32_t size);
/* Is [va, va+size) inside mapped, committed guest memory? */
int  wr_mem_valid(uint32_t va, uint32_t size);

static inline void *wr_ptr(uint32_t va) { return (void *)(uintptr_t)XBOX_PTR(va); }
static inline uint32_t wr_va(const void *p)
{
    return (uint32_t)((uintptr_t)p - (uintptr_t)g_xbox_mem_offset);
}
static inline uint32_t wr_rd32(uint32_t va) { return MEM32(va); }
static inline void wr_wr32(uint32_t va, uint32_t v) { MEM32(va) = v; }
static inline uint16_t wr_rd16(uint32_t va) { return MEM16(va); }
static inline void wr_wr16(uint32_t va, uint16_t v) { MEM16(va) = v; }
static inline uint8_t wr_rd8(uint32_t va) { return MEM8(va); }
static inline void wr_wr8(uint32_t va, uint8_t v) { MEM8(va) = v; }

/* Guest C string helpers. wr_str returns a host pointer into guest memory
 * (NULL for va == 0). */
const char *wr_str(uint32_t va);
/* Copy a NUL-terminated UTF-16 guest string into a host buffer (latin-1 cut). */
size_t wr_wstr_to_host(uint32_t va, char *out, size_t cap);
/* strncpy into guest memory, always terminated when cap > 0. Returns the
 * length written without the terminator. */
size_t wr_put_str(uint32_t va, uint32_t cap, const char *s);

/* ---- guest heap ----------------------------------------------------------- */

uint32_t wr_heap_alloc(uint32_t size, int zero);
uint32_t wr_heap_realloc(uint32_t va, uint32_t size, int zero, int in_place_only);
int      wr_heap_free(uint32_t va);
uint32_t wr_heap_size(uint32_t va);          /* 0xFFFFFFFF if not a heap block */
int      wr_heap_check(void);                /* walk + validate, 1 = sane */
/* A heap allocation the runtime keeps for good (strings, structs). */
uint32_t wr_galloc(uint32_t size);
uint32_t wr_gstrdup(const char *s);

/* ---- virtual memory (VirtualAlloc family) -------------------------------- */

#define WR_MEM_COMMIT   0x1000u
#define WR_MEM_RESERVE  0x2000u
#define WR_MEM_DECOMMIT 0x4000u
#define WR_MEM_RELEASE  0x8000u
#define WR_MEM_FREE     0x10000u
#define WR_MEM_PRIVATE  0x20000u
#define WR_MEM_IMAGE    0x1000000u

uint32_t wr_vmem_alloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t protect);
int      wr_vmem_free(uint32_t addr, uint32_t size, uint32_t type);
/* Fills a 28-byte MEMORY_BASIC_INFORMATION; returns 28 or 0. */
uint32_t wr_vmem_query(uint32_t addr, uint32_t mbi_va);
/* Mark a fixed range (image, stack, heap arena) for VirtualQuery. */
void     wr_vmem_note_fixed(uint32_t lo, uint32_t hi, uint32_t type, uint32_t protect);

/* ---- synthetic addresses / native modules -------------------------------- */

typedef void (*wr_handler_t)(RECOMP_PARAMS);

typedef struct wr_export {
    const char  *name;        /* "GetTickCount", or "#5" for an ordinal */
    wr_handler_t fn;
} wr_export;

typedef struct wr_module {
    const char      *name;     /* "KERNEL32.dll" (matched case-insensitively) */
    const wr_export *exports;  /* native exports, terminated by { NULL, NULL };
                                  for a guest module: overrides of its own */
    uint32_t         handle;   /* guest HMODULE, assigned at registration */
    uint32_t         guest_base; /* a lifted PE module: its image base (exports
                                    resolve to guest code), 0 = native only */
} wr_module;

/* Register a native module (import source, LoadLibrary target). */
void wr_module_register(wr_module *m);
wr_module *wr_module_find(const char *name);
wr_module *wr_module_by_handle(uint32_t handle);
/* Synthetic guest address for a native handler (allocated once per fn). */
uint32_t wr_thunk_for(wr_handler_t fn, const char *label);
/* Address of a module export, 0 if the module does not have it. */
uint32_t wr_module_proc(wr_module *m, const char *name);
const char *wr_thunk_label(uint32_t va);

/* ---- PE image ------------------------------------------------------------- */

typedef struct wr_image {
    uint32_t base, size, entry;
    uint32_t code_lo, code_hi;
    uint32_t iat_lo, iat_hi;
    char     host_path[1024];
    uint8_t  sha1[20];
} wr_image;

extern wr_image g_wr_image;

/* The generated dispatch of one lifted module (tools/lift.sh prefixes them). */
typedef recomp_func_t (*wr_lookup_fn)(uint32_t va);
typedef int (*wr_dispatch_init_fn)(void);

/* Map the main PE file and bind its imports. Unknown imports are bound to a
 * handler that stops with the import's name. 0 on failure (message logged).
 * Load lifted DLLs first: the exe's imports bind to them. */
int wr_pe_load(const char *host_path, wr_lookup_fn lookup, wr_dispatch_init_fn init);
/* Map a lifted DLL at its image base, bind its imports and register it as
 * module `name` (exports -> its guest code; `overrides` are native exports
 * that win over its own). DllMain runs in wr_run, before the exe's entry. */
wr_module *wr_pe_load_dll(const char *host_path, const char *name, const wr_export *overrides,
                          wr_lookup_fn lookup, wr_dispatch_init_fn init);
/* Run DllMain(DLL_PROCESS_ATTACH) of every lifted DLL (wr_run does it). */
void wr_pe_attach_dlls(X86Ctx *ctx);

/* ---- calling into the guest ------------------------------------------------ */

/* Call guest code at va as stdcall/cdecl with n 32-bit arguments (args[0]
 * is the first). The guest's own epilogue pops what its convention says;
 * the stack pointer is restored either way. Returns eax. */
uint32_t wr_call_guest(X86Ctx *ctx, uint32_t va, int n, const uint32_t *args);

/* Cooperative guest threads. There is one guest thread; a port can take over
 * a CreateThread whose start routine it knows and run that thread's loop as
 * steps instead. A step runs on the guest thread from Sleep and the message
 * pump (points where a real thread could have run too) and returns the ms
 * until it wants to run again, or < 0 when the thread has ended. */
typedef int (*wr_coop_step_fn)(X86Ctx *ctx, uint32_t param);
/* return a step function for (start, param), or NULL to refuse the thread */
typedef wr_coop_step_fn (*wr_thread_hook_fn)(X86Ctx *ctx, uint32_t start, uint32_t param);
void wr_set_thread_hook(wr_thread_hook_fn hook);
void wr_coop_run(X86Ctx *ctx);

/* Miles (libs/miles.c): report half the rate of stereo IMA ADPCM streams
 * (default on; WR_MILES_HALF_STEREO_RATE=0/1 wins) */
void wr_miles_set_half_stereo_rate(int on);
/* every one-shot sample start: its data, length, volume (0..1) and rate (the port: rumble on explosions) */
extern void (*g_wr_miles_start_hook)(uint32_t addr, uint32_t len, float vol, uint32_t rate);

/* The port's settings file saves/settings.ini (wr_settings.c): key=value,
 * unknown lines kept, atomic writes; <prefix>_<KEY> env overrides one run. */
/* F12 was pressed: the port's renderer captures the frame into
 * data/captures/<YYYYMMDD-HHMMSS>-frame<N>/ (the option-p layout; made by
 * wr_capture_dir) and clears the flag. */
extern int g_wr_screenshot_request;
/* 1 while a port menu over the game reads the keyboard / pads itself
 * (wr_keys_dik_state): DirectInput gives the game no keys. */
extern int g_wr_input_captured;
/* DIK codes the port keeps from the game (it reads them itself), e.g. Esc
 * opening the port's pause menu instead of the game's prompt. */
extern uint8_t g_wr_key_hidden[256];
int  wr_capture_dir(uint64_t frame, char *out, size_t cap);
void wr_restart(void);   /* start the game again (RESTART NOW); never returns */

void wr_settings_init(const char *env_prefix);       /* after wr_paths_init */
const char *wr_settings_get(const char *key, const char *def);
int  wr_settings_get_int(const char *key, int def);
void wr_settings_set(const char *key, const char *value);   /* writes at once */
void wr_settings_set_int(const char *key, int value);
const char *wr_settings_key_at(int i);   /* the i-th key in the file, NULL past the end */

/* Registry values set by the port (win32/misc.c; saves/registry.txt, written at once) */
void wr_reg_set_dword(const char *key, const char *name, uint32_t value);
int  wr_reg_get_dword(const char *key, const char *name, uint32_t *out);
void wr_reg_set_str(const char *key, const char *name, const char *str);
int  wr_reg_get_str(const char *key, const char *name, char *out, size_t cap);
#define WR_CALL_RET_VA 0xFEFFFFF0u

/* ---- handler helpers ------------------------------------------------------ */

/* Argument i (0-based) of a stdcall/cdecl call: the return address is at esp. */
#define WR_ARG(i)       MEM32(g_esp + 4u + 4u * (uint32_t)(i))
/* Return from a stdcall handler with n argument dwords. */
#define WR_RET(val, n)  do { g_eax = (uint32_t)(val); g_esp += 4u + 4u * (uint32_t)(n); return; } while (0)
/* Return from a cdecl handler: the caller pops the arguments. */
#define WR_RET_C(val)   do { g_eax = (uint32_t)(val); g_esp += 4u; return; } while (0)
#define WR_HANDLER(fn)  static void fn(X86Ctx *__restrict ctx, uint8_t *base)

/* ---- handles ---------------------------------------------------------------- */

enum wr_htype {
    WR_H_NONE = 0, WR_H_FILE, WR_H_FIND, WR_H_EVENT, WR_H_MUTEX, WR_H_HEAP,
    WR_H_REGKEY, WR_H_CONSOLE, WR_H_OTHER
};

typedef struct wr_hobj {
    int   type;
    void *data;
    void (*close)(void *data);
} wr_hobj;

uint32_t wr_handle_new(int type, void *data, void (*close)(void *data));
wr_hobj *wr_handle_get(uint32_t h, int type);   /* type 0 = any */
int      wr_handle_close(uint32_t h);

/* ---- last error ------------------------------------------------------------ */

extern uint32_t g_wr_last_error;
#define WR_SETERR(e) (g_wr_last_error = (uint32_t)(e))

/* ---- portable layout + paths ----------------------------------------------- */

typedef struct wr_paths {
    char root[1024];    /* the folder holding the exe */
    char game[1024];    /* game/  : the game's own files, read only */
    char saves[1024];   /* saves/ : player data, never deleted */
    char data[1024];    /* data/  : logs, caches, safe to delete */
    char media[1024];   /* media/ : player's media, read only */
} wr_paths;

extern wr_paths g_wr_paths;

/* Virtual Windows location of the game (the port sets it, e.g. "C:\\GTA2"):
 * GetModuleFileName answers vroot + "\\" + exe name, and every guest path
 * under it maps to game/ (reads, saves/ first) or saves/ (writes). */
extern char g_wr_vroot[128];
extern char g_wr_exe_name[64];

void wr_paths_init(const char *root);
/* Resolve a guest path for reading: saves/ overlay first, then game/.
 * Returns 1 and the host path if the file exists. Case-insensitive. */
int  wr_path_resolve_read(const char *guest, char *host, size_t cap);
/* Host path for writing (under saves/), creating directories. */
int  wr_path_resolve_write(const char *guest, char *host, size_t cap);
/* Normalised guest-relative path ("data/wil.gmp"), 0 if outside the root. */
int  wr_path_relative(const char *guest, char *rel, size_t cap);
/* Current directory, virtual (g_wr_vroot or below). */
extern char g_wr_cwd[512];
/* Case-insensitive host lookup of rel below dir; 1 if found. */
int  wr_path_ci(const char *dir, const char *rel, char *host, size_t cap);
/* A folder read after saves/ and before game/ (up to 4). */
void wr_path_add_read_dir(const char *dir);
/* the port may read another file in place of one: rel path in ('/'), a replacement or NULL */
extern const char *(*g_wr_read_map)(const char *rel);

/* ---- logging ------------------------------------------------------------------ */

void wr_log_set_name(const char *name);   /* before wr_log_init: data/<name>.log (+ <name>.previous.log) */
void wr_log_init(void);
void wr_log_header(const char *title, const char *version);   /* version, system, cpu, ram, folder */
/* Fatal signal / exception report into the log. */
void wr_crash_init(void);
void wr_log(const char *fmt, ...);
void wr_logv(const char *fmt, va_list ap);
void wr_fatal(const char *fmt, ...);
/* API tracing (WR_TRACE_API=1). */
extern int g_wr_trace_api;
#define WR_TRACE(...) do { if (g_wr_trace_api) wr_log(__VA_ARGS__); } while (0)
/* Log once per call site. */
#define WR_ONCE(...) do { static int _once; if (!_once) { _once = 1; wr_log(__VA_ARGS__); } } while (0)

/* ---- time ------------------------------------------------------------------- */

uint64_t wr_time_ns(void);       /* monotonic since start */
uint32_t wr_time_ms(void);       /* GetTickCount / timeGetTime */

/* ---- process ---------------------------------------------------------------- */

/* Run the image's entry point on the main guest thread; returns the exit code. */
int  wr_run(int argc, char **argv);
void wr_exit(uint32_t code);     /* ExitProcess */
/* The guest command line (ANSI, guest address). */
extern uint32_t g_wr_cmdline_va;
extern X86Ctx  *g_wr_main_ctx;

/* Native replacement for a guest function (checked before generated code). */
void wr_manual_register(uint32_t va, recomp_func_t fn);

/* Modules every PE port gets (win32 layer). */
void wr_win32_register(void);

#ifdef __cplusplus
}
#endif
#endif /* WR_H */
