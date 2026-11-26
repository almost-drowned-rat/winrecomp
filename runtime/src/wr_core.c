/*
 * winrecomp runtime - logging, time, handles, guest calls, run loop and the
 * symbols the generated code expects from its runtime.
 */
#include "wr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#endif

/* ---- logging ------------------------------------------------------------------ */

static FILE *g_logf;
int g_wr_trace_api;

void wr_log_init(void)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", g_wr_paths.data, "log.txt");
    g_logf = fopen(path, "w");
    const char *t = getenv("WR_TRACE_API");
    g_wr_trace_api = t && *t && *t != '0';
}

void wr_logv(const char *fmt, va_list ap)
{
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    double t = (double)wr_time_ns() / 1e9;
    fprintf(stderr, "[%9.3f] %s\n", t, buf);
    if (g_logf) {
        fprintf(g_logf, "[%9.3f] %s\n", t, buf);
        fflush(g_logf);
    }
}

void wr_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    wr_logv(fmt, ap);
    va_end(ap);
}

void wr_icall_trace_dump(void);

void wr_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    wr_log("FATAL: %s", buf);
    wr_icall_trace_dump();
    if (g_logf)
        fflush(g_logf);
    fflush(stderr);
    abort();
}

/* ---- time --------------------------------------------------------------------- */

static uint64_t host_ns(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t g_t0;

uint64_t wr_time_ns(void)
{
    if (!g_t0)
        g_t0 = host_ns();
    return host_ns() - g_t0;
}

/* GetTickCount: start the guest clock at a non-trivial value, as a machine
 * that has been on for a minute would. */
uint32_t wr_time_ms(void)
{
    return (uint32_t)(60000u + wr_time_ns() / 1000000u);
}

/* rdtsc: a 1 GHz time base. The guest calibrates against its own clock if it
 * cares about the rate. */
uint64_t xbox_ReadTimeStampCounter(void)
{
    return 1000000000ull + wr_time_ns();
}

/* ---- handles ------------------------------------------------------------------ */

#define MAX_HANDLES 4096
#define HBASE 0x00001000u
static wr_hobj g_handles[MAX_HANDLES];

uint32_t wr_handle_new(int type, void *data, void (*close)(void *))
{
    for (uint32_t i = 1; i < MAX_HANDLES; i++)
        if (!g_handles[i].type) {
            g_handles[i].type = type;
            g_handles[i].data = data;
            g_handles[i].close = close;
            return HBASE + 4u * i;
        }
    wr_log("handles: table full");
    return 0;
}

wr_hobj *wr_handle_get(uint32_t h, int type)
{
    uint32_t i = (h - HBASE) / 4u;
    if (h < HBASE || (h & 3u) || i >= MAX_HANDLES || !g_handles[i].type)
        return NULL;
    if (type && g_handles[i].type != type)
        return NULL;
    return &g_handles[i];
}

int wr_handle_close(uint32_t h)
{
    wr_hobj *o = wr_handle_get(h, 0);
    if (!o)
        return 0;
    if (o->close)
        o->close(o->data);
    memset(o, 0, sizeof *o);
    return 1;
}

uint32_t g_wr_last_error;

/* ---- generated-code support ------------------------------------------------------ */

int g_force_return;
uint32_t g_recomp_cpuid_edx1 = 0x0383FBFFu;   /* Pentium III-class answer */
RECOMP_TLS uint32_t g_icall_tls_trace[ICALL_TRACE_SIZE];
RECOMP_TLS uint32_t g_icall_tls_idx;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;

void wr_icall_trace_dump(void)
{
    char line[ICALL_TRACE_SIZE * 12 + 16];
    size_t n = 0;
    for (uint32_t i = 0; i < ICALL_TRACE_SIZE; i++) {
        uint32_t v = g_icall_tls_trace[(g_icall_tls_idx + i) & (ICALL_TRACE_SIZE - 1)];
        n += (size_t)snprintf(line + n, sizeof line - n, " %08X", v);
    }
    wr_log("last indirect calls (oldest first):%s", line);
}

typedef struct manual { uint32_t va; recomp_func_t fn; } manual;
static manual g_manual[256];
static int g_nmanual;

void wr_manual_register(uint32_t va, recomp_func_t fn)
{
    if (g_nmanual < 256)
        g_manual[g_nmanual++] = (manual){ va, fn };
}

recomp_func_t recomp_lookup_manual(uint32_t va)
{
    for (int i = 0; i < g_nmanual; i++)
        if (g_manual[i].va == va)
            return g_manual[i].fn;
    return NULL;
}

static uint32_t ret_addr(X86Ctx *ctx)
{
    return wr_mem_valid(g_esp, 4) ? wr_rd32(g_esp) : 0;
}

void recomp_icall_fail_log(X86Ctx *ctx, uint32_t va)
{
    const char *l = wr_thunk_label(va);
    wr_log("icall: no function at 0x%08X%s%s (return 0x%08X)", va, l ? " " : "", l ? l : "", ret_addr(ctx));
    if (getenv("WR_ICALL_TRAP"))
        wr_fatal("unresolved indirect call 0x%08X", va);
}

void recomp_icall_not_code_log(X86Ctx *ctx, uint32_t va)
{
    static int n;
    if (n++ < 50)
        wr_log("icall: 0x%08X is not code (return 0x%08X)", va, ret_addr(ctx));
    if (getenv("WR_ICALL_TRAP"))
        wr_fatal("indirect call to non-code 0x%08X", va);
}

void recomp_unimpl(X86Ctx *ctx, const char *text, uint32_t va)
{
    (void)ctx;
    static int n;
    if (n++ < 50)
        wr_log("unimplemented instruction reached: %s at 0x%08X", text, va);
    if (getenv("RECOMP_UNIMPL_TRAP"))
        wr_fatal("unimplemented instruction %s at 0x%08X", text, va);
}

void recomp_stub_hit(uint32_t va, uint32_t ret)
{
    static int n;
    if (n++ < 50)
        wr_log("unresolved function 0x%08X called (return 0x%08X)", va, ret);
}

/* ---- calling guest code ------------------------------------------------------------- */

uint32_t wr_call_guest(X86Ctx *ctx, uint32_t va, int n, const uint32_t *args)
{
    uint8_t *base = (uint8_t *)g_xbox_mem_offset;
    recomp_func_t fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup(va);
    if (!fn) fn = recomp_lookup_kernel(va);
    if (!fn) {
        wr_log("call: no guest function at 0x%08X", va);
        return 0;
    }
    uint32_t saved = g_esp;
    for (int i = n - 1; i >= 0; i--) {
        g_esp -= 4;
        wr_wr32(g_esp, args[i]);
    }
    g_esp -= 4;
    wr_wr32(g_esp, WR_CALL_RET_VA);
    fn(ctx, base);
    g_esp = saved;
    return g_eax;
}

/* ---- process ------------------------------------------------------------------------ */

X86Ctx *g_wr_main_ctx;
uint32_t g_wr_cmdline_va;

uint32_t wr_heap_top(void);

void wr_exit(uint32_t code)
{
    wr_log("heap: arena used up to 0x%08X (%u MB)", wr_heap_top(), (wr_heap_top() - WR_HEAP_LO) >> 20);
    wr_log("process exit, code %u", code);
    fflush(stderr);
    exit((int)code);
}

static void setup_teb(X86Ctx *ctx)
{
    wr_mem_commit(WR_PEB_VA, 0x10000u);
    memset(wr_ptr(WR_PEB_VA), 0, 0x10000u);
    wr_vmem_note_fixed(WR_PEB_VA, WR_PEB_VA + 0x10000u, WR_MEM_PRIVATE, 4);
    uint32_t teb = WR_TEB_VA;
    wr_wr32(teb + 0x00, 0xFFFFFFFFu);                   /* ExceptionList: end */
    wr_wr32(teb + 0x04, WR_STACK_TOP);                  /* StackBase */
    wr_wr32(teb + 0x08, WR_STACK_TOP - WR_STACK_SIZE);  /* StackLimit */
    wr_wr32(teb + 0x18, teb);                           /* Self */
    wr_wr32(teb + 0x20, 1);                             /* process id */
    wr_wr32(teb + 0x24, 2);                             /* thread id */
    wr_wr32(teb + 0x30, WR_PEB_VA);                     /* PEB */
    wr_wr32(WR_PEB_VA + 0x08, g_wr_image.base);         /* ImageBaseAddress */
    ctx->fs_base = teb;
}

int wr_run(int argc, char **argv)
{
    static X86Ctx main_ctx;
    X86Ctx *ctx = &main_ctx;
    uint8_t *base = (uint8_t *)g_xbox_mem_offset;
    g_wr_main_ctx = ctx;

    /* Command line: "C:\GAME\game.exe" args... */
    char cmd[2048];
    size_t n = (size_t)snprintf(cmd, sizeof cmd, "\"%s\\%s\"", g_wr_vroot, g_wr_exe_name);
    for (int i = 1; i < argc && n < sizeof cmd; i++)
        n += (size_t)snprintf(cmd + n, sizeof cmd - n, " %s", argv[i]);
    g_wr_cmdline_va = wr_gstrdup(cmd);

    /* Main thread stack. */
    wr_mem_commit(WR_STACK_TOP - WR_STACK_SIZE, WR_STACK_SIZE);
    wr_vmem_note_fixed(WR_STACK_TOP - WR_STACK_SIZE, WR_STACK_TOP, WR_MEM_PRIVATE, 4);
    memset(ctx, 0, sizeof *ctx);
    ctx->r_esp = WR_STACK_TOP - 0x100u;
    ctx->fp_control_word = 0x027F;   /* MSVC default: 53-bit precision, all masked */
    ctx->mxcsr = 0x1F80u;
    setup_teb(ctx);

    recomp_dispatch_init();
    wr_pe_attach_dlls(ctx);
    recomp_func_t entry = recomp_lookup(g_wr_image.entry);
    if (!entry) {
        wr_log("no translated function at the entry point 0x%08X", g_wr_image.entry);
        return 1;
    }
    wr_log("run: entry 0x%08X, cmdline %s", g_wr_image.entry, cmd);
    g_esp -= 4;
    wr_wr32(g_esp, WR_CALL_RET_VA);
    entry(ctx, base);
    wr_log("run: entry point returned, eax %u", g_eax);
    return (int)g_eax;
}
