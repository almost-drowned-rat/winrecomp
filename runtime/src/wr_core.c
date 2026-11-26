/*
 * winrecomp runtime - logging, time, handles, guest calls, run loop and the
 * symbols the generated code expects from its runtime.
 */
#include "wr.h"

#include <SDL3/SDL.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

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

/* ---- logging ------------------------------------------------------------------
 * data/<name>.log (the port names it, wr_log_set_name; "log" otherwise), the
 * run before kept as <name>.previous.log - after a crash and a restart the
 * report is still there. A run of lines that differ only in their numbers
 * keeps its first LOG_KEEP lines, then one summary line with the count and the
 * last one. Crash and watchdog reports are written verbatim (their lines all
 * look alike and every one matters). Whatever else prints to stdout / stderr
 * (libraries, drivers) is captured into the log as well (POSIX; the terminal
 * still shows everything). Every line is flushed: a crash loses nothing. */

#define LOG_KEEP 3
#define LOG_VERBATIM 400

static FILE *g_logf;
static char g_log_name[64] = "log";
static char g_prev_key[1024], g_last_line[1100];
static unsigned g_repeat, g_verbatim;
static int g_term_fd = 2;       /* the real terminal (stderr before the capture) */
int g_wr_trace_api;

#if !defined(_WIN32)
#include <pthread.h>
static pthread_mutex_t g_log_mx = PTHREAD_MUTEX_INITIALIZER;
/* try, never wait forever: a crash report may come while the lock is held */
static void log_lock(void)
{
    for (int i = 0; i < 200000; i++)
        if (pthread_mutex_trylock(&g_log_mx) == 0)
            return;
}
#define LOG_LOCK() log_lock()
#define LOG_UNLOCK() pthread_mutex_unlock(&g_log_mx)
#else
#define LOG_LOCK() ((void)0)
#define LOG_UNLOCK() ((void)0)
#endif

void wr_log_set_name(const char *name)
{
    if (name && *name)
        snprintf(g_log_name, sizeof g_log_name, "%s", name);
}

/* the line without its digits: lines alike but for numbers share it */
static void log_key(const char *l, char *k, size_t cap)
{
    size_t n = 0;
    for (; *l && n + 1 < cap; l++)
        if (*l < '0' || *l > '9')
            k[n++] = *l;
    k[n] = 0;
}

static void log_flush_repeat(void)
{
    if (g_repeat > LOG_KEEP && g_logf)
        fprintf(g_logf, "           ... %u more lines like this, the last: %s\n", g_repeat - LOG_KEEP, g_last_line);
    g_repeat = 0;
}

/* one finished line into the file (lock held) */
static void log_put(const char *line)
{
    if (!g_logf)
        return;
    if (strstr(line, "CRASH:") || strstr(line, "WATCHDOG:"))
        g_verbatim = LOG_VERBATIM;
    if (g_verbatim) {
        g_verbatim--;
        log_flush_repeat();
        g_prev_key[0] = 0;
        fprintf(g_logf, "%s\n", line);
        fflush(g_logf);
        return;
    }
    char key[1024];
    log_key(line + (line[0] == '[' && strchr(line, ']') ? strchr(line, ']') - line + 1 : 0), key, sizeof key);
    if (g_prev_key[0] && !strcmp(key, g_prev_key)) {
        if (++g_repeat >= LOG_KEEP) {
            snprintf(g_last_line, sizeof g_last_line, "%s", line);
            return;
        }
    } else {
        log_flush_repeat();
        snprintf(g_prev_key, sizeof g_prev_key, "%s", key);
    }
    fprintf(g_logf, "%s\n", line);
    fflush(g_logf);
}

#if !defined(_WIN32)
/* stdout / stderr of everything else: a pipe, read by a thread, written as log lines */
static int g_cap_r = -1;
static void *log_capture(void *arg)
{
    (void)arg;
    char buf[4096], line[1024];
    size_t n = 0;
    for (;;) {
        ssize_t got = read(g_cap_r, buf, sizeof buf);
        if (got <= 0)
            break;
        if (write(g_term_fd, buf, (size_t)got) < 0) { /* the terminal is gone: the log still gets it */ }
        for (ssize_t i = 0; i < got; i++) {
            if (buf[i] == '\n' || n + 1 >= sizeof line) {
                line[n] = 0;
                if (n && !(line[0] == '[' && strchr(line, ']'))) {   /* wr_log's own lines are in already */
                    char out[1100];
                    snprintf(out, sizeof out, "[%9.3f] (out) %s", (double)wr_time_ns() / 1e9, line);
                    LOG_LOCK();
                    log_put(out);
                    LOG_UNLOCK();
                }
                n = 0;
            } else
                line[n++] = buf[i];
        }
    }
    return NULL;
}
#endif

void wr_log_init(void)
{
    char path[1200], prev[1200], old[1200];
    snprintf(path, sizeof path, "%s/%s.log", g_wr_paths.data, g_log_name);
    snprintf(prev, sizeof prev, "%s/%s.previous.log", g_wr_paths.data, g_log_name);
    snprintf(old, sizeof old, "%s/log.txt", g_wr_paths.data);
    if (strcmp(g_log_name, "log"))
        remove(old);   /* the file name before 2026-11-26 */
    remove(prev);
    rename(path, prev);
    g_logf = fopen(path, "w");
    const char *t = getenv("WR_TRACE_API");
    g_wr_trace_api = t && *t && *t != '0';
#if !defined(_WIN32)
    int fds[2];
    if (g_logf && pipe(fds) == 0) {
        g_term_fd = dup(2);
        if (g_term_fd < 0)
            g_term_fd = 2;
        fflush(stdout);
        fflush(stderr);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        g_cap_r = fds[0];
        setvbuf(stdout, NULL, _IOLBF, 0);
        pthread_t th;
        if (pthread_create(&th, NULL, log_capture, NULL) == 0)
            pthread_detach(th);
    }
#endif
}

void wr_logv(const char *fmt, va_list ap)
{
    char buf[2048], line[2100];
    vsnprintf(buf, sizeof buf, fmt, ap);
    double t = (double)wr_time_ns() / 1e9;
    int n = snprintf(line, sizeof line, "[%9.3f] %s", t, buf);
    if (n > 0) {
#if !defined(_WIN32)
        if (write(g_term_fd, line, strlen(line)) < 0 || write(g_term_fd, "\n", 1) < 0) { /* no terminal */ }
#else
        fprintf(stderr, "%s\n", line);
#endif
    }
    LOG_LOCK();
    log_put(line);
    LOG_UNLOCK();
}

/* The log's head: who, what and where - the first thing to read in a report. */
void wr_log_header(const char *title, const char *version)
{
    wr_log("%s %s", title, version ? version : "");
    int sv = SDL_GetVersion();
    wr_log("system: %s, SDL %d.%d.%d", SDL_GetPlatform(), SDL_VERSIONNUM_MAJOR(sv), SDL_VERSIONNUM_MINOR(sv),
           SDL_VERSIONNUM_MICRO(sv));
#if !defined(_WIN32)
    FILE *f = fopen("/etc/os-release", "r");
    if (f) {
        char l[256];
        while (fgets(l, sizeof l, f))
            if (!strncmp(l, "PRETTY_NAME=", 12)) {
                l[strcspn(l, "\n")] = 0;
                wr_log("os: %s", l + 12);
                break;
            }
        fclose(f);
    }
    f = fopen("/proc/sys/kernel/osrelease", "r");
    if (f) {
        char l[128];
        if (fgets(l, sizeof l, f)) {
            l[strcspn(l, "\n")] = 0;
            wr_log("kernel: %s", l);
        }
        fclose(f);
    }
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char l[256];
        while (fgets(l, sizeof l, f))
            if (!strncmp(l, "model name", 10) && strchr(l, ':')) {
                l[strcspn(l, "\n")] = 0;
                wr_log("cpu: %s, %d threads", strchr(l, ':') + 2, SDL_GetNumLogicalCPUCores());
                break;
            }
        fclose(f);
    }
#else
    wr_log("cpu: %d threads", SDL_GetNumLogicalCPUCores());
#endif
    wr_log("ram: %d MB", SDL_GetSystemRAM());
    const char *de = getenv("XDG_CURRENT_DESKTOP"), *ses = getenv("XDG_SESSION_TYPE");
    if (de || ses)
        wr_log("desktop: %s (%s)", de ? de : "?", ses ? ses : "?");
    wr_log("folder: %s", g_wr_paths.root);
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
    /* split into seconds + rest: a double of c * 1e9 loses ns precision after a few hours */
    uint64_t f = (uint64_t)freq.QuadPart, n = (uint64_t)c.QuadPart;
    return (n / f) * 1000000000ull + (n % f) * 1000000000ull / f;
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

#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
void wr_sleep_ns(uint64_t ns)
{
    /* one timer per thread (a waitable timer is waited on by the thread that set it) */
    static __thread HANDLE timer;
    static __thread int tried;
    static int period_set;
    if (!ns) {
        Sleep(0);   /* as Sleep(0) / nanosleep(0): give up the rest of the slice */
        return;
    }
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((ns + 99) / 100);   /* relative, 100 ns units */
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    /* older Windows: Sleep at a 1 ms timer period (timeBeginPeriod loaded at run time, no
     * winmm import); a sub-ms wait becomes Sleep(0), which only yields the slice */
    if (!period_set) {
        period_set = 1;
        HMODULE w = LoadLibraryA("winmm.dll");
        typedef UINT (WINAPI *tbp_fn)(UINT);
        tbp_fn tbp = w ? (tbp_fn)(void *)GetProcAddress(w, "timeBeginPeriod") : NULL;
        if (tbp)
            tbp(1);
    }
    Sleep((DWORD)(ns / 1000000u));
}
#else
void wr_sleep_ns(uint64_t ns)
{
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    nanosleep(&ts, NULL);
}
#endif

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
#if defined(_WIN32)
    exit((int)code);
#else
    /* not exit(): its destructors run while the audio / timer threads still lock their mutexes (Android's
     * FORTIFY aborts on that: "pthread_mutex_lock called on a destroyed mutex" -> a crash report after every
     * QUIT, Odin 2026-11-26). Everything the player keeps is on disk already (saves and settings are written at
     * once, atomically); the logs are flushed here. */
    fflush(NULL);
    _exit((int)code);
#endif
}

/* RESTART NOW (a setting that only applies at start, e.g. the language):
 * everything the player chose is already on disk (settings.ini and the
 * registry file are written at once), so the process just starts itself
 * again with the same arguments. */
static char **g_argv;

void wr_restart(void)
{
    wr_log("restart requested");
    fflush(stderr);
    SDL_Quit();   /* give back the display mode, window and audio device */
#if defined(_WIN32)
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (CreateProcessA(NULL, GetCommandLineA(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    exit(0);
#elif defined(__ANDROID__)
    exit(0);   /* M8: the activity restarts the game */
#else
    if (g_argv)
        execv("/proc/self/exe", g_argv);
    wr_log("restart: exec failed, exiting");
    exit(0);
#endif
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
    g_argv = argv;
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
