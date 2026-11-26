/*
 * winrecomp runtime - crash report.
 *
 * On a fatal signal (POSIX) or unhandled exception (Windows) the log gets:
 * the faulting address (and its guest address when it lies in the guest
 * space), the guest registers of the main thread, the last indirect calls
 * and the host call stack with the generated sub_XXXXXXXX names resolved
 * (the exe is linked with -rdynamic / exported symbols for that).
 */
#include "wr.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__GLIBC__)
#  include <dlfcn.h>
#  include <execinfo.h>
#endif

void wr_icall_trace_dump(void);

static void report(const char *what, uintptr_t fault)
{
    uintptr_t gbase = (uintptr_t)g_xbox_mem_offset;
    if (!fault)
        ;
    else if (gbase && fault >= gbase && fault < gbase + (uintptr_t)WR_GUEST_SPAN)
        wr_log("CRASH: %s at host %p = guest address 0x%08X", what, (void *)fault, (uint32_t)(fault - gbase));
    else
        wr_log("CRASH: %s at host %p", what, (void *)fault);
    X86Ctx *c = g_wr_main_ctx;
    if (c)
        wr_log("guest regs: eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X ebp %08X esp %08X",
               c->r_eax, c->r_ebx, c->r_ecx, c->r_edx, c->r_esi, c->r_edi, c->r_ebp, c->r_esp);
    if (c && wr_mem_valid(c->r_esp, 64)) {
        char line[256];
        size_t n = 0;
        for (int i = 0; i < 12; i++)
            n += (size_t)snprintf(line + n, sizeof line - n, " %08X", wr_rd32(c->r_esp + 4u * (uint32_t)i));
        wr_log("guest stack:%s", line);
    }
    wr_icall_trace_dump();
#if defined(__GLIBC__)
    void *frames[48];
    int n = backtrace(frames, 48);
    for (int i = 0; i < n; i++) {
        Dl_info info;
        if (dladdr(frames[i], &info) && info.dli_sname)
            wr_log("  #%02d %s+0x%lx", i, info.dli_sname, (unsigned long)((uintptr_t)frames[i] - (uintptr_t)info.dli_saddr));
        else
            wr_log("  #%02d %p", i, frames[i]);
    }
#endif
}

/* ---- hang watchdog ------------------------------------------------------------------------
 * The guest's message loop / frame flip bumps wr_heartbeat. A host thread
 * checks it; when it stops for WR_WATCHDOG seconds (default 5, 0 = off) the
 * main thread gets SIGUSR1 and logs where it is: the backtrace names the
 * generated sub_XXXXXXXX it is stuck in. */
volatile uint32_t wr_heartbeat;

#if !defined(_WIN32)
#include <pthread.h>
#include <unistd.h>
static pthread_t g_main_thread;

static void on_usr1(int sig)
{
    (void)sig;
    wr_log("WATCHDOG: no message loop / flip for a while - the main thread is here:");
    report("hang", 0);
}

static void *watchdog(void *arg)
{
    int secs = (int)(intptr_t)arg;
    uint32_t last = wr_heartbeat;
    int still = 0, reported = 0;
    for (;;) {
        sleep(1);
        uint32_t now = wr_heartbeat;
        if (now != last) {
            last = now;
            still = 0;
            reported = 0;
            continue;
        }
        if (++still >= secs && !reported) {
            pthread_kill(g_main_thread, SIGUSR1);
            reported = 1;
        }
    }
    return NULL;
}
#endif

#if defined(_WIN32)
static LONG WINAPI on_exception(EXCEPTION_POINTERS *ep)
{
    static volatile LONG in;
    if (InterlockedIncrement(&in) > 1)
        return EXCEPTION_CONTINUE_SEARCH;
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    DWORD code = er->ExceptionCode;
    const char *name = code == EXCEPTION_ACCESS_VIOLATION ? "ACCESS_VIOLATION"
                     : code == EXCEPTION_ILLEGAL_INSTRUCTION ? "ILLEGAL_INSTRUCTION"
                     : code == EXCEPTION_INT_DIVIDE_BY_ZERO ? "INT_DIVIDE_BY_ZERO"
                     : code == EXCEPTION_STACK_OVERFLOW ? "STACK_OVERFLOW"
                     : code == EXCEPTION_IN_PAGE_ERROR ? "IN_PAGE_ERROR" : "exception";
    /* where: the host code address as module + offset (the exe's offset maps to a function
     * with llvm-nm / the linker map; the release exe carries no symbols) */
    uintptr_t pc = (uintptr_t)er->ExceptionAddress;
    HMODULE mod = NULL;
    char mname[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pc, &mod) && mod) {
        GetModuleFileNameA(mod, mname, sizeof mname);
        const char *slash = strrchr(mname, '\\');
        wr_log("CRASH: %s (0x%08lX) at %s+0x%llx", name, (unsigned long)code, slash ? slash + 1 : mname,
               (unsigned long long)(pc - (uintptr_t)mod));
    } else {
        wr_log("CRASH: %s (0x%08lX) at host pc %p", name, (unsigned long)code, (void *)pc);
    }
    uintptr_t fault = (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
                      && er->NumberParameters >= 2 ? (uintptr_t)er->ExceptionInformation[1] : 0;
    report(name, fault);
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void on_signal(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    static volatile sig_atomic_t in;
    if (in++)
        _exit(128 + sig);
    const char *name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL"
                     : sig == SIGFPE ? "SIGFPE" : sig == SIGABRT ? "SIGABRT" : "signal";
    report(name, (uintptr_t)si->si_addr);
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

void wr_crash_init(void)
{
#if defined(_WIN32)
    SetUnhandledExceptionFilter(on_exception);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    static char altstack[65536];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
    const char *w = getenv("WR_WATCHDOG");
    int secs = w && *w ? atoi(w) : 5;
    if (secs > 0) {
        g_main_thread = pthread_self();
        signal(SIGUSR1, on_usr1);
        pthread_t t;
        if (pthread_create(&t, NULL, watchdog, (void *)(intptr_t)secs) == 0)
            pthread_detach(t);
    }
#endif
}
