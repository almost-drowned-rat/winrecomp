/*
 * winrecomp runtime - KERNEL32.
 *
 * Every handler pops exactly the argument bytes of the real stdcall
 * signature (WR_RET(value, ndwords)); a wrong count shifts the guest stack.
 */
#include "w32_internal.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#  define ftruncate_(f, n) _chsize_s(_fileno(f), (n))
#else
#  include <dirent.h>
#  include <unistd.h>
#  define ftruncate_(f, n) ftruncate(fileno(f), (off_t)(n))
#endif

/* ---- text helpers ------------------------------------------------------------- */

static const uint16_t k1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

uint16_t w32_cp1252_to_u16(uint8_t c)
{
    return (c >= 0x80 && c < 0xA0) ? k1252[c - 0x80] : c;
}

uint8_t w32_u16_to_cp1252(uint16_t w, int *defaulted)
{
    if (w < 0x80 || (w >= 0xA0 && w < 0x100))
        return (uint8_t)w;
    for (int i = 0; i < 32; i++)
        if (k1252[i] == w)
            return (uint8_t)(0x80 + i);
    if (defaulted)
        *defaulted = 1;
    return '?';
}

int w32_tolower(int c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
        return c + 32;
    return c;
}

int w32_toupper(int c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
        return c - 32;
    return c;
}

uint16_t w32_ctype1(uint16_t c)
{
    uint16_t t = 0;
    if (c > 0xFF)
        return 0x100;   /* treat as alpha */
    if (w32_toupper(c) == c && w32_tolower(c) != c) t |= 0x001 | 0x100;   /* C1_UPPER | C1_ALPHA */
    if (w32_tolower(c) == c && w32_toupper(c) != c) t |= 0x002 | 0x100;   /* C1_LOWER | C1_ALPHA */
    if (c == 0xDF || c == 0xFF) t |= 0x002 | 0x100;
    if (c >= '0' && c <= '9') t |= 0x004 | 0x080;
    if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) t |= 0x080;
    if (c == ' ' || (c >= 9 && c <= 13)) t |= 0x008;
    if (c == ' ' || c == '\t') t |= 0x040;
    if (c < 32 || c == 127) t |= 0x020;
    if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126) ||
        (c >= 0xA1 && c <= 0xBF) || c == 0xD7 || c == 0xF7)
        t |= 0x010;
    return t;
}

/* ---- process / module --------------------------------------------------------- */

WR_HANDLER(k_GetCommandLineA) { (void)base; WR_RET(g_wr_cmdline_va, 0); }

WR_HANDLER(k_GetStartupInfoA)
{
    (void)base;
    uint32_t p = WR_ARG(0);
    memset(wr_ptr(p), 0, 68);
    wr_wr32(p, 68);
    WR_RET(0, 1);
}

WR_HANDLER(k_GetVersionExA)
{
    (void)base;
    uint32_t p = WR_ARG(0);
    uint32_t size = wr_rd32(p);
    if (size != 148 && size != 156) {
        WR_SETERR(ERROR_INSUFFICIENT_BUFFER);
        WR_RET(0, 1);
    }
    memset((char *)wr_ptr(p) + 4, 0, size - 4);
    wr_wr32(p + 4, 5);       /* Windows XP */
    wr_wr32(p + 8, 1);
    wr_wr32(p + 12, 2600);
    wr_wr32(p + 16, 2);      /* VER_PLATFORM_WIN32_NT */
    wr_put_str(p + 20, 128, "Service Pack 3");
    if (size == 156) {
        wr_wr16(p + 148, 3);  /* wServicePackMajor */
        wr_wr8(p + 154, 1);   /* wProductType: workstation */
    }
    WR_RET(1, 1);
}

WR_HANDLER(k_GetSystemInfo)
{
    (void)base;
    uint32_t p = WR_ARG(0);
    memset(wr_ptr(p), 0, 36);
    wr_wr32(p + 4, 4096);
    wr_wr32(p + 8, 0x00010000u);
    wr_wr32(p + 12, 0x7FFEFFFFu);
    wr_wr32(p + 16, 1);
    wr_wr32(p + 20, 1);
    wr_wr32(p + 24, 586);
    wr_wr32(p + 28, 0x10000u);
    wr_wr16(p + 32, 6);
    WR_RET(0, 1);
}

WR_HANDLER(k_GetCurrentProcess) { (void)base; WR_RET(0xFFFFFFFFu, 0); }
WR_HANDLER(k_GetCurrentProcessId) { (void)base; WR_RET(1, 0); }
WR_HANDLER(k_GetCurrentThreadId) { (void)base; WR_RET(2, 0); }

WR_HANDLER(k_ExitProcess)
{
    (void)base;
    w32_exit(WR_ARG(0));
}

WR_HANDLER(k_TerminateProcess)
{
    (void)base;
    if (WR_ARG(0) == 0xFFFFFFFFu)
        w32_exit(WR_ARG(1));
    WR_RET(0, 2);
}

WR_HANDLER(k_GetModuleHandleA)
{
    (void)base;
    const char *name = wr_str(WR_ARG(0));
    if (!name)
        WR_RET(g_wr_image.base, 1);
    if (!strcasecmp(name, g_wr_exe_name))
        WR_RET(g_wr_image.base, 1);
    wr_module *m = wr_module_find(name);
    WR_TRACE("GetModuleHandleA(%s) -> %s", name, m ? "native" : "none");
    if (!m) {
        WR_SETERR(ERROR_MOD_NOT_FOUND);
        WR_RET(0, 1);
    }
    WR_RET(m->handle, 1);
}

WR_HANDLER(k_GetModuleFileNameA)
{
    (void)base;
    uint32_t h = WR_ARG(0), buf = WR_ARG(1), n = WR_ARG(2);
    char path[512];
    wr_module *m = h && h != g_wr_image.base ? wr_module_by_handle(h) : NULL;
    if (m)
        snprintf(path, sizeof path, "C:\\WINDOWS\\system32\\%s", m->name);
    else
        snprintf(path, sizeof path, "%s\\%s", g_wr_vroot, g_wr_exe_name);
    if (!n)
        WR_RET(0, 3);
    size_t len = wr_put_str(buf, n, path);
    if (len < strlen(path))
        WR_SETERR(ERROR_INSUFFICIENT_BUFFER);
    WR_RET(len, 3);
}

WR_HANDLER(k_LoadLibraryA)
{
    (void)base;
    const char *name = wr_str(WR_ARG(0));
    wr_module *m = name ? wr_module_find(name) : NULL;
    wr_log("LoadLibraryA(%s) -> %s", name ? name : "(null)", m ? "native module" : "not found");
    if (!m) {
        WR_SETERR(ERROR_MOD_NOT_FOUND);
        WR_RET(0, 1);
    }
    WR_RET(m->handle, 1);
}

WR_HANDLER(k_FreeLibrary) { (void)base; WR_RET(1, 1); }

WR_HANDLER(k_GetProcAddress)
{
    (void)base;
    uint32_t h = WR_ARG(0), name_va = WR_ARG(1);
    char name[96];
    if (name_va < 0x10000u)
        snprintf(name, sizeof name, "#%u", name_va);
    else
        snprintf(name, sizeof name, "%s", wr_str(name_va));
    wr_module *m = wr_module_by_handle(h);
    uint32_t addr = m ? wr_module_proc(m, name) : 0;
    wr_log("GetProcAddress(%s, %s) -> 0x%08X", m ? m->name : "?", name, addr);
    if (!addr)
        WR_SETERR(ERROR_PROC_NOT_FOUND);
    WR_RET(addr, 2);
}

static uint32_t g_unhandled_filter;
WR_HANDLER(k_SetUnhandledExceptionFilter)
{
    (void)base;
    uint32_t old = g_unhandled_filter;
    g_unhandled_filter = WR_ARG(0);
    WR_RET(old, 1);
}
WR_HANDLER(k_UnhandledExceptionFilter) { (void)base; WR_RET(0, 1); }

/* RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue). Nothing in
 * the runtime raises guest exceptions, so this only ever runs from code that
 * unwinds deliberately: pop SEH frames up to the target without calling the
 * handlers. */
WR_HANDLER(k_RtlUnwind)
{
    (void)base;
    uint32_t target = WR_ARG(0), retval = WR_ARG(3);
    uint32_t teb = ctx->fs_base;
    uint32_t frame = wr_rd32(teb);
    int n = 0;
    while (frame != 0xFFFFFFFFu && frame != target && n++ < 1000)
        frame = wr_rd32(frame);
    wr_wr32(teb, target ? target : 0xFFFFFFFFu);
    wr_log("RtlUnwind: frames up to 0x%08X dropped (handlers not run)", target);
    WR_RET(retval, 4);
}

WR_HANDLER(k_SetConsoleCtrlHandler) { (void)base; WR_RET(1, 2); }
WR_HANDLER(k_OutputDebugStringA)
{
    (void)base;
    const char *s = wr_str(WR_ARG(0));
    if (s) {
        char b[512];
        snprintf(b, sizeof b, "%s", s);
        size_t l = strlen(b);
        while (l && (b[l - 1] == '\n' || b[l - 1] == '\r'))
            b[--l] = 0;
        wr_log("guest: %s", b);
    }
    WR_RET(0, 1);
}

/* ---- errors / formatting ------------------------------------------------------ */

WR_HANDLER(k_GetLastError) { (void)base; WR_RET(g_wr_last_error, 0); }

WR_HANDLER(k_FormatMessageA)
{
    (void)base;
    uint32_t flags = WR_ARG(0), id = WR_ARG(2), buf = WR_ARG(4), n = WR_ARG(5);
    char msg[64];
    snprintf(msg, sizeof msg, "Error %u.\r\n", id);
    uint32_t len = (uint32_t)strlen(msg);
    if (flags & 0x100u) {   /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
        uint32_t want = n > len + 1 ? n : len + 1;
        uint32_t p = wr_heap_alloc(want, 1);
        wr_put_str(p, want, msg);
        wr_wr32(buf, p);
        WR_RET(len, 7);
    }
    if (n <= len)
        WR_RET(0, 7);
    wr_put_str(buf, n, msg);
    WR_RET(len, 7);
}

WR_HANDLER(k_LocalFree)
{
    (void)base;
    wr_heap_free(WR_ARG(0));
    WR_RET(0, 1);
}

/* ---- heap / virtual memory ---------------------------------------------------- */

#define HEAP_ZERO_MEMORY 0x8u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10u

WR_HANDLER(k_HeapCreate)
{
    (void)base;
    uint32_t h = wr_handle_new(WR_H_HEAP, NULL, NULL);
    WR_RET(h, 3);
}

WR_HANDLER(k_HeapDestroy)
{
    (void)base;
    wr_handle_close(WR_ARG(0));
    WR_RET(1, 1);
}

WR_HANDLER(k_HeapAlloc)
{
    (void)base;
    uint32_t p = wr_heap_alloc(WR_ARG(2), (WR_ARG(1) & HEAP_ZERO_MEMORY) != 0);
    if (!p)
        WR_SETERR(ERROR_NOT_ENOUGH_MEMORY);
    WR_RET(p, 3);
}

WR_HANDLER(k_HeapReAlloc)
{
    (void)base;
    uint32_t flags = WR_ARG(1);
    uint32_t p = wr_heap_realloc(WR_ARG(2), WR_ARG(3), (flags & HEAP_ZERO_MEMORY) != 0,
                                 (flags & HEAP_REALLOC_IN_PLACE_ONLY) != 0);
    WR_RET(p, 4);
}

WR_HANDLER(k_HeapFree) { (void)base; WR_RET(wr_heap_free(WR_ARG(2)), 3); }
WR_HANDLER(k_HeapSize) { (void)base; WR_RET(wr_heap_size(WR_ARG(2)), 3); }

WR_HANDLER(k_VirtualAlloc)
{
    (void)base;
    uint32_t p = wr_vmem_alloc(WR_ARG(0), WR_ARG(1), WR_ARG(2), WR_ARG(3));
    WR_TRACE("VirtualAlloc(0x%08X, 0x%X, 0x%X) -> 0x%08X", WR_ARG(0), WR_ARG(1), WR_ARG(2), p);
    WR_RET(p, 4);
}

WR_HANDLER(k_VirtualFree) { (void)base; WR_RET(wr_vmem_free(WR_ARG(0), WR_ARG(1), WR_ARG(2)), 3); }
WR_HANDLER(k_VirtualQuery) { (void)base; WR_RET(wr_vmem_query(WR_ARG(0), WR_ARG(1)), 3); }

WR_HANDLER(k_VirtualProtect)
{
    (void)base;
    if (WR_ARG(3))
        wr_wr32(WR_ARG(3), 4);   /* PAGE_READWRITE */
    WR_RET(1, 4);
}

WR_HANDLER(k_IsBadReadPtr) { (void)base; WR_RET(WR_ARG(1) && !wr_mem_valid(WR_ARG(0), WR_ARG(1)), 2); }
WR_HANDLER(k_IsBadWritePtr) { (void)base; WR_RET(WR_ARG(1) && !wr_mem_valid(WR_ARG(0), WR_ARG(1)), 2); }
WR_HANDLER(k_IsBadCodePtr)
{
    (void)base;
    uint32_t p = WR_ARG(0);
    int ok = (p >= g_wr_image.code_lo && p < g_wr_image.code_hi) || recomp_lookup_kernel(p) != NULL;
    WR_RET(!ok, 1);
}

/* ---- strings / locale ---------------------------------------------------------- */

WR_HANDLER(k_lstrlenA)
{
    (void)base;
    const char *s = wr_str(WR_ARG(0));
    WR_RET(s ? strlen(s) : 0, 1);
}

WR_HANDLER(k_GetACP) { (void)base; WR_RET(1252, 0); }
WR_HANDLER(k_GetOEMCP) { (void)base; WR_RET(437, 0); }

WR_HANDLER(k_GetCPInfo)
{
    (void)base;
    uint32_t p = WR_ARG(1);
    memset(wr_ptr(p), 0, 20);
    wr_wr32(p, 1);
    wr_wr8(p + 4, '?');
    WR_RET(1, 2);
}

WR_HANDLER(k_MultiByteToWideChar)
{
    (void)base;
    uint32_t src = WR_ARG(2), dst = WR_ARG(4);
    int32_t n = (int32_t)WR_ARG(3), cap = (int32_t)WR_ARG(5);
    if (n < 0)
        n = (int32_t)strlen(wr_str(src)) + 1;
    if (cap == 0)
        WR_RET(n, 6);
    if (cap < n) {
        WR_SETERR(ERROR_INSUFFICIENT_BUFFER);
        WR_RET(0, 6);
    }
    for (int32_t i = 0; i < n; i++)
        wr_wr16(dst + 2u * (uint32_t)i, w32_cp1252_to_u16(wr_rd8(src + (uint32_t)i)));
    WR_RET(n, 6);
}

WR_HANDLER(k_WideCharToMultiByte)
{
    (void)base;
    uint32_t src = WR_ARG(2), dst = WR_ARG(4), used = WR_ARG(7);
    int32_t n = (int32_t)WR_ARG(3), cap = (int32_t)WR_ARG(5);
    if (n < 0) {
        n = 0;
        while (wr_rd16(src + 2u * (uint32_t)n))
            n++;
        n++;
    }
    if (cap == 0)
        WR_RET(n, 8);
    if (cap < n) {
        WR_SETERR(ERROR_INSUFFICIENT_BUFFER);
        WR_RET(0, 8);
    }
    int def = 0;
    for (int32_t i = 0; i < n; i++)
        wr_wr8(dst + (uint32_t)i, w32_u16_to_cp1252(wr_rd16(src + 2u * (uint32_t)i), &def));
    if (used)
        wr_wr32(used, (uint32_t)def);
    WR_RET(n, 8);
}

WR_HANDLER(k_GetStringTypeA)
{
    (void)base;
    uint32_t type = WR_ARG(1), src = WR_ARG(2), out = WR_ARG(4);
    int32_t n = (int32_t)WR_ARG(3);
    if (n < 0)
        n = (int32_t)strlen(wr_str(src)) + 1;
    for (int32_t i = 0; i < n; i++)
        wr_wr16(out + 2u * (uint32_t)i, type == 1 ? w32_ctype1(wr_rd8(src + (uint32_t)i)) : 0);
    WR_RET(1, 5);
}

WR_HANDLER(k_GetStringTypeW)
{
    (void)base;
    uint32_t type = WR_ARG(0), src = WR_ARG(1), out = WR_ARG(3);
    int32_t n = (int32_t)WR_ARG(2);
    if (n < 0) {
        n = 0;
        while (wr_rd16(src + 2u * (uint32_t)n))
            n++;
        n++;
    }
    for (int32_t i = 0; i < n; i++)
        wr_wr16(out + 2u * (uint32_t)i, type == 1 ? w32_ctype1(wr_rd16(src + 2u * (uint32_t)i)) : 0);
    WR_RET(1, 4);
}

#define LCMAP_LOWERCASE 0x100u
#define LCMAP_UPPERCASE 0x200u

static uint32_t lcmap(uint32_t flags, uint32_t src, int32_t n, uint32_t dst, int32_t cap, int wide)
{
    if (n < 0) {
        n = 0;
        while (wide ? wr_rd16(src + 2u * (uint32_t)n) : wr_rd8(src + (uint32_t)n))
            n++;
        n++;
    }
    if (cap == 0)
        return (uint32_t)n;
    if (cap < n)
        return 0;
    for (int32_t i = 0; i < n; i++) {
        int c = wide ? wr_rd16(src + 2u * (uint32_t)i) : wr_rd8(src + (uint32_t)i);
        if (c < 0x100) {
            if (flags & LCMAP_LOWERCASE) c = w32_tolower(c);
            if (flags & LCMAP_UPPERCASE) c = w32_toupper(c);
        }
        if (wide)
            wr_wr16(dst + 2u * (uint32_t)i, (uint16_t)c);
        else
            wr_wr8(dst + (uint32_t)i, (uint8_t)c);
    }
    return (uint32_t)n;
}

WR_HANDLER(k_LCMapStringA)
{
    (void)base;
    WR_RET(lcmap(WR_ARG(1), WR_ARG(2), (int32_t)WR_ARG(3), WR_ARG(4), (int32_t)WR_ARG(5), 0), 6);
}

WR_HANDLER(k_LCMapStringW)
{
    (void)base;
    WR_RET(lcmap(WR_ARG(1), WR_ARG(2), (int32_t)WR_ARG(3), WR_ARG(4), (int32_t)WR_ARG(5), 1), 6);
}

static uint32_t compare(uint32_t flags, uint32_t a, int32_t na, uint32_t b, int32_t nb, int wide)
{
    for (int32_t i = 0;; i++) {
        int ea = na >= 0 ? i >= na : 0, eb = nb >= 0 ? i >= nb : 0;
        int ca = ea ? 0 : (wide ? wr_rd16(a + 2u * (uint32_t)i) : wr_rd8(a + (uint32_t)i));
        int cb = eb ? 0 : (wide ? wr_rd16(b + 2u * (uint32_t)i) : wr_rd8(b + (uint32_t)i));
        if (flags & 1u) {   /* NORM_IGNORECASE */
            ca = w32_tolower(ca);
            cb = w32_tolower(cb);
        }
        if (ca != cb)
            return ca < cb ? 1 : 3;
        if (!ca)
            return 2;
    }
}

WR_HANDLER(k_CompareStringA)
{
    (void)base;
    WR_RET(compare(WR_ARG(1), WR_ARG(2), (int32_t)WR_ARG(3), WR_ARG(4), (int32_t)WR_ARG(5), 0), 6);
}

WR_HANDLER(k_CompareStringW)
{
    (void)base;
    WR_RET(compare(WR_ARG(1), WR_ARG(2), (int32_t)WR_ARG(3), WR_ARG(4), (int32_t)WR_ARG(5), 1), 6);
}

WR_HANDLER(k_GetLocaleInfoA)
{
    (void)base;
    uint32_t type = WR_ARG(1) & 0xFFFFu, out = WR_ARG(2), cap = WR_ARG(3);
    const char *v;
    switch (type) {
    case 0x1004: v = "1252"; break;       /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x000B: v = "437"; break;        /* LOCALE_IDEFAULTCODEPAGE */
    case 0x0001: v = "0409"; break;       /* LOCALE_ILANGUAGE */
    case 0x0002: v = "English (United States)"; break;
    case 0x0003: v = "ENU"; break;        /* LOCALE_SABBREVLANGNAME */
    case 0x0007: v = "USA"; break;        /* LOCALE_SABBREVCTRYNAME */
    case 0x000E: v = "."; break;          /* LOCALE_SDECIMAL */
    case 0x000F: v = ","; break;          /* LOCALE_STHOUSAND */
    default: v = ""; WR_ONCE("GetLocaleInfoA: type 0x%X answered empty", type); break;
    }
    uint32_t need = (uint32_t)strlen(v) + 1;
    if (!cap)
        WR_RET(need, 4);
    if (cap < need) {
        WR_SETERR(ERROR_INSUFFICIENT_BUFFER);
        WR_RET(0, 4);
    }
    wr_put_str(out, cap, v);
    WR_RET(need, 4);
}

/* ---- environment ---------------------------------------------------------------- */

static uint32_t g_env_a, g_env_w;

WR_HANDLER(k_GetEnvironmentStrings)
{
    (void)base;
    if (!g_env_a)
        g_env_a = wr_galloc(4);
    WR_RET(g_env_a, 0);
}

WR_HANDLER(k_GetEnvironmentStringsW)
{
    (void)base;
    if (!g_env_w)
        g_env_w = wr_galloc(8);
    WR_RET(g_env_w, 0);
}

WR_HANDLER(k_FreeEnvironmentStringsA) { (void)base; WR_RET(1, 1); }
WR_HANDLER(k_FreeEnvironmentStringsW) { (void)base; WR_RET(1, 1); }
WR_HANDLER(k_SetEnvironmentVariableA) { (void)base; WR_RET(1, 2); }

/* ---- time --------------------------------------------------------------------------- */

WR_HANDLER(k_GetTickCount) { (void)base; WR_RET(wr_time_ms(), 0); }

WR_HANDLER(k_QueryPerformanceCounter)
{
    (void)base;
    uint64_t v = wr_time_ns() / 1000u;   /* 1 MHz */
    wr_wr32(WR_ARG(0), (uint32_t)v);
    wr_wr32(WR_ARG(0) + 4, (uint32_t)(v >> 32));
    WR_RET(1, 1);
}

WR_HANDLER(k_Sleep)
{
    (void)base;
    uint32_t ms = WR_ARG(0);
    wr_coop_run(ctx);
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
#endif
    WR_RET(0, 1);
}

/* FILETIME: 100 ns units since 1601-01-01. */
static uint64_t now_filetime(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100u;
}

WR_HANDLER(k_GetSystemTimeAsFileTime)
{
    (void)base;
    uint64_t ft = now_filetime();
    wr_wr32(WR_ARG(0), (uint32_t)ft);
    wr_wr32(WR_ARG(0) + 4, (uint32_t)(ft >> 32));
    WR_RET(0, 1);
}

WR_HANDLER(k_FileTimeToLocalFileTime)
{
    (void)base;
    wr_wr32(WR_ARG(1), wr_rd32(WR_ARG(0)));
    wr_wr32(WR_ARG(1) + 4, wr_rd32(WR_ARG(0) + 4));
    WR_RET(1, 2);
}

WR_HANDLER(k_FileTimeToSystemTime)
{
    (void)base;
    uint64_t ft = wr_rd32(WR_ARG(0)) | (uint64_t)wr_rd32(WR_ARG(0) + 4) << 32;
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    uint32_t p = WR_ARG(1);
    wr_wr16(p + 0, (uint16_t)(tmv.tm_year + 1900));
    wr_wr16(p + 2, (uint16_t)(tmv.tm_mon + 1));
    wr_wr16(p + 4, (uint16_t)tmv.tm_wday);
    wr_wr16(p + 6, (uint16_t)tmv.tm_mday);
    wr_wr16(p + 8, (uint16_t)tmv.tm_hour);
    wr_wr16(p + 10, (uint16_t)tmv.tm_min);
    wr_wr16(p + 12, (uint16_t)tmv.tm_sec);
    wr_wr16(p + 14, (uint16_t)((ft / 10000u) % 1000u));
    WR_RET(1, 2);
}

WR_HANDLER(k_GetTimeZoneInformation)
{
    (void)base;
    memset(wr_ptr(WR_ARG(0)), 0, 172);
    WR_RET(0, 1);   /* TIME_ZONE_ID_UNKNOWN */
}

/* ---- synchronisation (one guest thread: no waits needed) ----------------------- */

typedef struct named { char name[128]; int type; } named;

static void named_close(void *d) { free(d); }

static uint32_t find_named(const char *name, int type)
{
    if (!name)
        return 0;
    for (uint32_t h = 0x1000u + 4u; h < 0x1000u + 4u * 4096u; h += 4u) {
        wr_hobj *o = wr_handle_get(h, type);
        if (o && o->data && !strcmp(((named *)o->data)->name, name))
            return h;
    }
    return 0;
}

static uint32_t create_named(const char *name, int type)
{
    uint32_t h = find_named(name, type);
    if (h) {
        WR_SETERR(ERROR_ALREADY_EXISTS);
        return wr_handle_new(type, NULL, NULL);   /* a second handle to "it" */
    }
    named *n = (named *)calloc(1, sizeof *n);
    if (name)
        snprintf(n->name, sizeof n->name, "%s", name);
    WR_SETERR(0);
    return wr_handle_new(type, n, named_close);
}

WR_HANDLER(k_CreateEventA) { (void)base; WR_RET(create_named(wr_str(WR_ARG(3)), WR_H_EVENT), 4); }
WR_HANDLER(k_CreateMutexA)
{
    (void)base;
    const char *name = wr_str(WR_ARG(2));
    uint32_t h = create_named(name, WR_H_MUTEX);
    WR_TRACE("CreateMutexA(%s) -> 0x%X (err %u)", name ? name : "", h, g_wr_last_error);
    WR_RET(h, 3);
}
WR_HANDLER(k_OpenMutexA)
{
    (void)base;
    const char *name = wr_str(WR_ARG(2));
    uint32_t h = find_named(name, WR_H_MUTEX);
    WR_TRACE("OpenMutexA(%s) -> %s", name ? name : "", h ? "found" : "none");
    if (!h) {
        WR_SETERR(ERROR_FILE_NOT_FOUND);
        WR_RET(0, 3);
    }
    WR_RET(wr_handle_new(WR_H_MUTEX, NULL, NULL), 3);
}
WR_HANDLER(k_ReleaseMutex) { (void)base; WR_RET(1, 1); }

/* ---- files ---------------------------------------------------------------------- */

typedef struct wfile {
    FILE *f;
    int   console;    /* 1 in, 2 out, 3 err */
    char  path[1200];
} wfile;

static void wfile_close(void *d)
{
    wfile *w = (wfile *)d;
    if (w->f)
        fclose(w->f);
    free(w);
}

static uint32_t g_std[3];

static void ensure_std(void)
{
    if (g_std[0])
        return;
    for (int i = 0; i < 3; i++) {
        wfile *w = (wfile *)calloc(1, sizeof *w);
        w->console = i + 1;
        g_std[i] = wr_handle_new(WR_H_CONSOLE, w, wfile_close);
    }
}

WR_HANDLER(k_GetStdHandle)
{
    (void)base;
    ensure_std();
    int32_t which = (int32_t)WR_ARG(0);
    uint32_t h = which == -10 ? g_std[0] : which == -11 ? g_std[1] : which == -12 ? g_std[2] : W32_INVALID_HANDLE;
    WR_RET(h, 1);
}

WR_HANDLER(k_SetStdHandle) { (void)base; WR_RET(1, 2); }
WR_HANDLER(k_SetHandleCount) { (void)base; WR_RET(WR_ARG(0), 1); }

WR_HANDLER(k_GetFileType)
{
    (void)base;
    if (wr_handle_get(WR_ARG(0), WR_H_CONSOLE))
        WR_RET(2, 1);   /* FILE_TYPE_CHAR */
    if (wr_handle_get(WR_ARG(0), WR_H_FILE))
        WR_RET(1, 1);   /* FILE_TYPE_DISK */
    WR_RET(0, 1);
}

static int copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb");
    if (!a)
        return 0;
    FILE *b = fopen(to, "wb");
    if (!b) {
        fclose(a);
        return 0;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        fwrite(buf, 1, n, b);
    fclose(a);
    fclose(b);
    return 1;
}

#define GENERIC_READ  0x80000000u
#define GENERIC_WRITE 0x40000000u

WR_HANDLER(k_CreateFileA)
{
    (void)base;
    const char *name = wr_str(WR_ARG(0));
    uint32_t access = WR_ARG(1), disp = WR_ARG(4);
    char rd[1200], wrp[1200];
    int exists = name && wr_path_resolve_read(name, rd, sizeof rd);
    struct stat st;
    if (exists && stat(rd, &st) == 0 && S_ISDIR(st.st_mode))
        exists = 0;
    const char *mode = NULL;
    const char *host = rd;
    int writing = (access & GENERIC_WRITE) != 0;

    if (!name) {
        WR_SETERR(ERROR_PATH_NOT_FOUND);
        WR_RET(W32_INVALID_HANDLE, 7);
    }
    switch (disp) {
    case 1: /* CREATE_NEW */
        if (exists) { WR_SETERR(ERROR_FILE_EXISTS); WR_RET(W32_INVALID_HANDLE, 7); }
        mode = "w+b"; break;
    case 2: /* CREATE_ALWAYS */
        mode = "w+b"; break;
    case 3: /* OPEN_EXISTING */
        if (!exists) { WR_SETERR(ERROR_FILE_NOT_FOUND); WR_RET(W32_INVALID_HANDLE, 7); }
        mode = writing ? "r+b" : "rb"; break;
    case 4: /* OPEN_ALWAYS */
        mode = exists ? (writing ? "r+b" : "rb") : "w+b"; break;
    case 5: /* TRUNCATE_EXISTING */
        if (!exists) { WR_SETERR(ERROR_FILE_NOT_FOUND); WR_RET(W32_INVALID_HANDLE, 7); }
        mode = "w+b"; break;
    default:
        WR_SETERR(ERROR_INVALID_PARAMETER);
        WR_RET(W32_INVALID_HANDLE, 7);
    }
    if (writing || mode[0] == 'w') {
        if (!wr_path_resolve_write(name, wrp, sizeof wrp)) {
            WR_SETERR(ERROR_ACCESS_DENIED);
            WR_RET(W32_INVALID_HANDLE, 7);
        }
        /* Writing a file that only exists in game/: work on a copy in saves/. */
        if (exists && mode[0] == 'r' && strcmp(rd, wrp) != 0 && !copy_file(rd, wrp)) {
            WR_SETERR(ERROR_ACCESS_DENIED);
            WR_RET(W32_INVALID_HANDLE, 7);
        }
        host = wrp;
    }
    FILE *f = fopen(host, mode);
    WR_TRACE("CreateFileA(%s, 0x%X, %u) -> %s %s", name, access, disp, host, f ? "ok" : "FAILED");
    if (!f) {
        WR_SETERR(ERROR_PATH_NOT_FOUND);
        WR_RET(W32_INVALID_HANDLE, 7);
    }
    wfile *w = (wfile *)calloc(1, sizeof *w);
    w->f = f;
    snprintf(w->path, sizeof w->path, "%s", host);
    WR_SETERR(exists && (disp == 2 || disp == 4) ? ERROR_ALREADY_EXISTS : 0);
    WR_RET(wr_handle_new(WR_H_FILE, w, wfile_close), 7);
}

static wfile *get_file(uint32_t h)
{
    wr_hobj *o = wr_handle_get(h, WR_H_FILE);
    if (!o)
        o = wr_handle_get(h, WR_H_CONSOLE);
    return o ? (wfile *)o->data : NULL;
}

WR_HANDLER(k_ReadFile)
{
    (void)base;
    wfile *w = get_file(WR_ARG(0));
    uint32_t buf = WR_ARG(1), n = WR_ARG(2), outn = WR_ARG(3);
    if (!w) {
        WR_SETERR(ERROR_INVALID_HANDLE);
        WR_RET(0, 5);
    }
    size_t got = 0;
    if (w->f && n) {
        if (!wr_mem_valid(buf, n)) {
            wr_log("ReadFile: buffer 0x%08X+0x%X not valid guest memory", buf, n);
            WR_SETERR(ERROR_INVALID_PARAMETER);
            WR_RET(0, 5);
        }
        got = fread(wr_ptr(buf), 1, n, w->f);
    }
    if (outn)
        wr_wr32(outn, (uint32_t)got);
    WR_RET(1, 5);
}

WR_HANDLER(k_WriteFile)
{
    (void)base;
    wfile *w = get_file(WR_ARG(0));
    uint32_t buf = WR_ARG(1), n = WR_ARG(2), outn = WR_ARG(3);
    if (!w) {
        WR_SETERR(ERROR_INVALID_HANDLE);
        WR_RET(0, 5);
    }
    size_t put = n;
    if (w->console) {
        char line[1024];
        size_t k = n < sizeof line - 1 ? n : sizeof line - 1;
        memcpy(line, wr_ptr(buf), k);
        line[k] = 0;
        while (k && (line[k - 1] == '\n' || line[k - 1] == '\r'))
            line[--k] = 0;
        wr_log("guest %s: %s", w->console == 3 ? "stderr" : "stdout", line);
    } else if (w->f) {
        put = fwrite(wr_ptr(buf), 1, n, w->f);
    }
    if (outn)
        wr_wr32(outn, (uint32_t)put);
    WR_RET(put == n, 5);
}

WR_HANDLER(k_SetFilePointer)
{
    (void)base;
    wfile *w = get_file(WR_ARG(0));
    int64_t dist = (int32_t)WR_ARG(1);
    uint32_t hi = WR_ARG(2), method = WR_ARG(3);
    if (!w || !w->f) {
        WR_SETERR(ERROR_INVALID_HANDLE);
        WR_RET(0xFFFFFFFFu, 4);
    }
    if (hi)
        dist = (int64_t)((uint64_t)wr_rd32(hi) << 32 | (uint32_t)WR_ARG(1));
    int whence = method == 1 ? SEEK_CUR : method == 2 ? SEEK_END : SEEK_SET;
    if (fseek(w->f, (long)dist, whence) != 0) {
        WR_SETERR(131);   /* ERROR_NEGATIVE_SEEK */
        WR_RET(0xFFFFFFFFu, 4);
    }
    long pos = ftell(w->f);
    if (hi)
        wr_wr32(hi, 0);
    WR_SETERR(0);
    WR_RET((uint32_t)pos, 4);
}

WR_HANDLER(k_SetEndOfFile)
{
    (void)base;
    wfile *w = get_file(WR_ARG(0));
    if (!w || !w->f)
        WR_RET(0, 1);
    fflush(w->f);
    WR_RET(ftruncate_(w->f, ftell(w->f)) == 0, 1);
}

WR_HANDLER(k_FlushFileBuffers)
{
    (void)base;
    wfile *w = get_file(WR_ARG(0));
    if (w && w->f)
        fflush(w->f);
    WR_RET(1, 1);
}

WR_HANDLER(k_CloseHandle)
{
    (void)base;
    uint32_t h = WR_ARG(0);
    if (!wr_handle_close(h)) {
        WR_SETERR(ERROR_INVALID_HANDLE);
        WR_RET(0, 1);
    }
    WR_RET(1, 1);
}

WR_HANDLER(k_GetFileAttributesA)
{
    (void)base;
    const char *name = wr_str(WR_ARG(0));
    char host[1200];
    struct stat st;
    if (!name || !wr_path_resolve_read(name, host, sizeof host) || stat(host, &st) != 0) {
        WR_TRACE("GetFileAttributesA(%s) -> missing", name ? name : "");
        WR_SETERR(ERROR_FILE_NOT_FOUND);
        WR_RET(0xFFFFFFFFu, 1);
    }
    WR_RET(S_ISDIR(st.st_mode) ? 0x10u : 0x20u, 1);
}

/* ---- directories / paths ------------------------------------------------------------ */

static void to_backslash_abs(const char *guest, char *out, size_t cap)
{
    char rel[1024];
    if (wr_path_relative(guest, rel, sizeof rel)) {
        if (rel[0])
            snprintf(out, cap, "%s\\%s", g_wr_vroot, rel);
        else
            snprintf(out, cap, "%s", g_wr_vroot);
    } else {
        snprintf(out, cap, "%s", guest);
    }
    for (char *p = out; *p; p++)
        if (*p == '/')
            *p = '\\';
}

WR_HANDLER(k_GetFullPathNameA)
{
    (void)base;
    const char *name = wr_str(WR_ARG(0));
    uint32_t cap = WR_ARG(1), buf = WR_ARG(2), part = WR_ARG(3);
    char full[1200];
    to_backslash_abs(name ? name : "", full, sizeof full);
    uint32_t need = (uint32_t)strlen(full);
    if (cap <= need)
        WR_RET(need + 1, 4);
    wr_put_str(buf, cap, full);
    if (part) {
        const char *s = strrchr(full, '\\');
        wr_wr32(part, s ? buf + (uint32_t)(s - full) + 1 : buf);
    }
    WR_RET(need, 4);
}

WR_HANDLER(k_GetDriveTypeA)
{
    (void)base;
    const char *r = wr_str(WR_ARG(0));
    uint32_t t = (!r || ((r[0] | 32) == (g_wr_vroot[0] | 32) && r[1] == ':')) ? 3u : 1u;
    wr_log("GetDriveTypeA(%s) -> %u", r ? r : "(cwd)", t);
    WR_RET(t, 1);
}

WR_HANDLER(k_SetCurrentDirectoryA)
{
    (void)base;
    const char *p = wr_str(WR_ARG(0));
    char rel[1024], host[1200];
    if (!p || !wr_path_relative(p, rel, sizeof rel)) {
        wr_log("SetCurrentDirectoryA(%s): outside the game folder", p ? p : "");
        WR_SETERR(ERROR_PATH_NOT_FOUND);
        WR_RET(0, 1);
    }
    if (rel[0] && !wr_path_ci(g_wr_paths.game, rel, host, sizeof host) &&
        !wr_path_ci(g_wr_paths.saves, rel, host, sizeof host)) {
        WR_SETERR(ERROR_PATH_NOT_FOUND);
        WR_RET(0, 1);
    }
    to_backslash_abs(p, g_wr_cwd, sizeof g_wr_cwd);
    WR_RET(1, 1);
}

WR_HANDLER(k_GetCurrentDirectoryA)
{
    (void)base;
    uint32_t cap = WR_ARG(0), buf = WR_ARG(1);
    uint32_t need = (uint32_t)strlen(g_wr_cwd);
    if (cap <= need)
        WR_RET(need + 1, 2);
    wr_put_str(buf, cap, g_wr_cwd);
    WR_RET(need, 2);
}

/* FindFirstFileA / FindNextFileA: game/ and saves/ merged, saves/ wins. */
typedef struct wfind {
    char   names[1024][260];
    uint8_t dir[1024];
    uint32_t size[1024];
    int    n, at;
} wfind;

static int wild_match(const char *pat, const char *s)
{
    if (!*pat)
        return !*s;
    if (*pat == '*') {
        for (; *s; s++)
            if (wild_match(pat + 1, s))
                return 1;
        return wild_match(pat + 1, s);
    }
    if (!*s)
        return 0;
    if (*pat == '?' || w32_tolower((unsigned char)*pat) == w32_tolower((unsigned char)*s))
        return wild_match(pat + 1, s + 1);
    return 0;
}

static void find_scan(wfind *f, const char *hostdir, const char *pat)
{
#if defined(_WIN32)
    char spec[1300];
    snprintf(spec, sizeof spec, "%s/*", hostdir);
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA(spec, &d);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const char *nm = d.cFileName;
#else
    DIR *dd = opendir(hostdir);
    if (!dd)
        return;
    struct dirent *e;
    while ((e = readdir(dd))) {
        const char *nm = e->d_name;
#endif
        if (!wild_match(pat, nm))
            continue;
        int dup = 0;
        for (int i = 0; i < f->n; i++)
            if (!strcasecmp(f->names[i], nm))
                dup = 1;
        if (dup || f->n >= 1024)
            continue;
        char full[1600];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", hostdir, nm);
        if (stat(full, &st) != 0)
            continue;
        snprintf(f->names[f->n], 260, "%s", nm);
        f->dir[f->n] = S_ISDIR(st.st_mode) ? 1 : 0;
        f->size[f->n] = (uint32_t)st.st_size;
        f->n++;
#if defined(_WIN32)
    } while (FindNextFileA(h, &d));
    FindClose(h);
#else
    }
    closedir(dd);
#endif
}

static void find_fill(wfind *f, uint32_t p)
{
    memset(wr_ptr(p), 0, 320);
    wr_wr32(p, f->dir[f->at] ? 0x10u : 0x20u);
    wr_wr32(p + 32, f->size[f->at]);
    wr_put_str(p + 44, 260, f->names[f->at]);
    f->at++;
}

static void find_close(void *d) { free(d); }

WR_HANDLER(k_FindFirstFileA)
{
    (void)base;
    const char *spec = wr_str(WR_ARG(0));
    uint32_t out = WR_ARG(1);
    char rel[1024];
    if (!spec || !wr_path_relative(spec, rel, sizeof rel)) {
        WR_SETERR(ERROR_PATH_NOT_FOUND);
        WR_RET(W32_INVALID_HANDLE, 2);
    }
    char *slash = strrchr(rel, '/');
    const char *pat = slash ? slash + 1 : rel;
    char dir[1024] = "";
    if (slash) {
        *slash = 0;
        snprintf(dir, sizeof dir, "%s", rel);
    }
    wfind *f = (wfind *)calloc(1, sizeof *f);
    char host[1200];
    if (!dir[0] || wr_path_ci(g_wr_paths.saves, dir, host, sizeof host))
        find_scan(f, dir[0] ? host : g_wr_paths.saves, pat);
    if (!dir[0] || wr_path_ci(g_wr_paths.game, dir, host, sizeof host))
        find_scan(f, dir[0] ? host : g_wr_paths.game, pat);
    WR_TRACE("FindFirstFileA(%s) -> %d entries", spec, f->n);
    if (!f->n) {
        free(f);
        WR_SETERR(ERROR_FILE_NOT_FOUND);
        WR_RET(W32_INVALID_HANDLE, 2);
    }
    find_fill(f, out);
    WR_RET(wr_handle_new(WR_H_FIND, f, find_close), 2);
}

WR_HANDLER(k_FindNextFileA)
{
    (void)base;
    wr_hobj *o = wr_handle_get(WR_ARG(0), WR_H_FIND);
    wfind *f = o ? (wfind *)o->data : NULL;
    if (!f || f->at >= f->n) {
        WR_SETERR(ERROR_NO_MORE_FILES);
        WR_RET(0, 2);
    }
    find_fill(f, WR_ARG(1));
    WR_RET(1, 2);
}

WR_HANDLER(k_FindClose) { (void)base; WR_RET(wr_handle_close(WR_ARG(0)), 1); }

/* GetPrivateProfileStringA(app, key, default, out, size, file) */
WR_HANDLER(k_GetPrivateProfileStringA)
{
    (void)base;
    const char *app = wr_str(WR_ARG(0)), *key = wr_str(WR_ARG(1)), *def = wr_str(WR_ARG(2));
    uint32_t out = WR_ARG(3), cap = WR_ARG(4);
    const char *file = wr_str(WR_ARG(5));
    char host[1200], result[1024];
    snprintf(result, sizeof result, "%s", def ? def : "");
    int found = 0;
    if (file && app && key && wr_path_resolve_read(file, host, sizeof host)) {
        FILE *f = fopen(host, "rb");
        if (f) {
            char line[1024];
            int in_app = 0;
            while (fgets(line, sizeof line, f)) {
                char *s = line;
                while (*s == ' ' || *s == '\t') s++;
                char *e = s + strlen(s);
                while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
                if (*s == '[') {
                    char *c = strchr(s, ']');
                    if (c) *c = 0;
                    in_app = !strcasecmp(s + 1, app);
                    continue;
                }
                char *eq = strchr(s, '=');
                if (!in_app || !eq)
                    continue;
                *eq = 0;
                char *k = s, *ke = eq;
                while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
                if (strcasecmp(k, key))
                    continue;
                char *v = eq + 1;
                while (*v == ' ' || *v == '\t') v++;
                size_t vl = strlen(v);
                if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) {
                    v[vl - 1] = 0;
                    v++;
                }
                snprintf(result, sizeof result, "%s", v);
                found = 1;
                break;
            }
            fclose(f);
        }
    } else if (!app || !key) {
        WR_ONCE("GetPrivateProfileStringA: section/key enumeration not implemented");
    }
    WR_TRACE("GetPrivateProfileStringA([%s] %s, %s) -> %s%s", app ? app : "", key ? key : "",
             file ? file : "", result, found ? "" : " (default)");
    if (!cap)
        WR_RET(0, 6);
    WR_RET(wr_put_str(out, cap, result), 6);
}

/* ---- used by lifted DLLs (binkw32) ------------------------------------------------- */

WR_HANDLER(k_SetErrorMode) { (void)base; WR_RET(0, 1); }
WR_HANDLER(k_GetVersion) { (void)base; WR_RET(0x0A280105u, 0); }   /* 5.1 build 2600 (XP), NT */
WR_HANDLER(k_DisableThreadLibraryCalls) { (void)base; WR_RET(1, 1); }

/* ---- cooperative threads ---------------------------------------------------------------- */

#define MAX_COOP 8
static struct { wr_coop_step_fn step; uint32_t param; uint64_t due_ns; } g_coop[MAX_COOP];
static wr_thread_hook_fn g_thread_hook;
static int g_coop_busy;

void wr_set_thread_hook(wr_thread_hook_fn hook) { g_thread_hook = hook; }

void wr_coop_run(X86Ctx *ctx)
{
    if (g_coop_busy)
        return;         /* a step that sleeps does not re-enter */
    g_coop_busy = 1;
    uint64_t now = wr_time_ns();
    for (int i = 0; i < MAX_COOP; i++) {
        if (!g_coop[i].step || now < g_coop[i].due_ns)
            continue;
        int ms = g_coop[i].step(ctx, g_coop[i].param);
        if (ms < 0)
            g_coop[i].step = NULL;
        else
            g_coop[i].due_ns = now + (uint64_t)ms * 1000000ull;
    }
    g_coop_busy = 0;
}

/* Real guest threads are not supported (one guest thread). A thread the port
 * knows becomes a cooperative one; others are refused - callers that check
 * the result fall back to working synchronously. */
WR_HANDLER(k_CreateThread)
{
    (void)base;
    uint32_t start = WR_ARG(2), param = WR_ARG(3), ptid = WR_ARG(5);
    wr_coop_step_fn step = g_thread_hook ? g_thread_hook(ctx, start, param) : NULL;
    if (step) {
        for (int i = 0; i < MAX_COOP; i++)
            if (!g_coop[i].step) {
                g_coop[i].step = step;
                g_coop[i].param = param;
                g_coop[i].due_ns = 0;
                if (ptid)
                    wr_wr32(ptid, 0x200u + (uint32_t)i);
                wr_log("CreateThread(start 0x%08X): runs cooperatively", start);
                WR_RET(wr_handle_new(WR_H_OTHER, NULL, NULL), 6);
            }
    }
    wr_log("CreateThread(start 0x%08X): guest threads not supported, refused", start);
    WR_SETERR(8);
    WR_RET(0, 6);
}

WR_HANDLER(k_RaiseException)
{
    (void)base;
    wr_fatal("RaiseException(0x%08X, flags %u) from 0x%08X", WR_ARG(0), WR_ARG(1), wr_rd32(g_esp));
}

/* ---- module table ------------------------------------------------------------------- */

#define E(n) { #n, k_##n }
static const wr_export k_exports[] = {
    E(WideCharToMultiByte), E(MultiByteToWideChar), E(GetFileAttributesA), E(GetPrivateProfileStringA),
    E(FindClose), E(FindNextFileA), E(FindFirstFileA), E(GetTickCount), E(Sleep), E(CreateEventA),
    E(CloseHandle), E(lstrlenA), E(LocalFree), E(FormatMessageA), E(GetLastError), E(OpenMutexA),
    E(ReleaseMutex), E(CreateMutexA), E(GetCurrentProcessId), E(GetCurrentThreadId),
    E(QueryPerformanceCounter), E(GetCPInfo), E(GetOEMCP), E(GetACP), E(IsBadCodePtr), E(IsBadReadPtr),
    E(SetUnhandledExceptionFilter), E(GetStringTypeA), E(SetStdHandle), E(GetVersionExA),
    E(GetEnvironmentStringsW), E(OutputDebugStringA), E(GetEnvironmentStrings), E(FreeEnvironmentStringsA),
    E(UnhandledExceptionFilter), E(GetModuleFileNameA), E(LCMapStringW), E(LCMapStringA), E(VirtualQuery),
    E(GetSystemInfo), E(VirtualProtect), E(HeapSize), E(IsBadWritePtr), E(HeapReAlloc), E(VirtualAlloc),
    E(VirtualFree), E(HeapCreate), E(HeapDestroy), E(CreateFileA), E(ReadFile), E(WriteFile), E(GetFileType),
    E(GetStdHandle), E(SetHandleCount), E(SetFilePointer), E(GetCommandLineA), E(GetStartupInfoA),
    E(GetFullPathNameA), E(GetDriveTypeA), E(GetSystemTimeAsFileTime), E(FileTimeToLocalFileTime),
    E(FlushFileBuffers), E(LoadLibraryA), E(GetStringTypeW), E(GetTimeZoneInformation), E(GetLocaleInfoA),
    E(GetProcAddress), E(FreeEnvironmentStringsW), E(FreeLibrary), E(SetConsoleCtrlHandler),
    E(CompareStringA), E(SetEndOfFile), E(FileTimeToSystemTime), E(GetCurrentProcess), E(TerminateProcess),
    E(GetModuleHandleA), E(ExitProcess), E(SetCurrentDirectoryA), E(GetCurrentDirectoryA),
    E(SetEnvironmentVariableA), E(HeapFree), E(RtlUnwind), E(HeapAlloc), E(CompareStringW),
    E(SetErrorMode), E(GetVersion), E(DisableThreadLibraryCalls), E(CreateThread), E(RaiseException),
    { NULL, NULL }
};
#undef E

static wr_module k_module = { "KERNEL32.dll", k_exports, 0 };

void w32_register_kernel32(void) { wr_module_register(&k_module); }
