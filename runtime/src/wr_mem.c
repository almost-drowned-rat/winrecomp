/*
 * winrecomp runtime - guest address space, VirtualAlloc regions.
 *
 * One 4 GiB host reservation holds the whole 32-bit guest space, so a guest
 * address is a plain offset (g_xbox_mem_offset) and every 32-bit pointer the
 * game forms stays inside it. On POSIX the user half (above the null guard,
 * below 2 GiB) is mapped read/write up front with MAP_NORESERVE: pages cost
 * nothing until touched. Windows reserves and commits on demand.
 */
#include "wr.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

ptrdiff_t g_xbox_mem_offset;
uint32_t  g_xbox_code_lo;
uint32_t  g_xbox_code_hi;

static uint8_t *g_span;

#define PAGE 4096u
#define USER_HI 0x80000000u

int wr_mem_init(void)
{
#if defined(_WIN32)
    g_span = (uint8_t *)VirtualAlloc(NULL, (SIZE_T)WR_GUEST_SPAN, MEM_RESERVE, PAGE_NOACCESS);
    if (!g_span) {
        wr_log("mem: could not reserve 4 GiB of address space (error %lu)", GetLastError());
        return 0;
    }
#else
    void *p = mmap(NULL, (size_t)WR_GUEST_SPAN, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        wr_log("mem: could not reserve 4 GiB of address space");
        return 0;
    }
    g_span = (uint8_t *)p;
    /* User half read/write, lazily backed. */
    if (mprotect(g_span + WR_NULL_GUARD, USER_HI - WR_NULL_GUARD, PROT_READ | PROT_WRITE) != 0) {
        wr_log("mem: mprotect of the user half failed");
        return 0;
    }
#endif
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_span;
    wr_log("mem: guest space at host %p", (void *)g_span);
    return 1;
}

int wr_mem_commit(uint32_t va, uint32_t size)
{
    if (!size)
        return 1;
    if (va < WR_NULL_GUARD || (uint64_t)va + size > USER_HI)
        return 0;
#if defined(_WIN32)
    uint32_t lo = va & ~(PAGE - 1), hi = (uint32_t)(((uint64_t)va + size + PAGE - 1) & ~(uint64_t)(PAGE - 1));
    return VirtualAlloc(g_span + lo, hi - lo, MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
    return 1;
#endif
}

void wr_mem_decommit(uint32_t va, uint32_t size)
{
    uint32_t lo = (va + PAGE - 1) & ~(PAGE - 1);
    uint32_t hi = (uint32_t)(((uint64_t)va + size) & ~(uint64_t)(PAGE - 1));
    if (hi <= lo)
        return;
#if defined(_WIN32)
    VirtualFree(g_span + lo, hi - lo, MEM_DECOMMIT);
#elif defined(MADV_DONTNEED)
    madvise(g_span + lo, hi - lo, MADV_DONTNEED);
#else
    memset(g_span + lo, 0, hi - lo);
#endif
}

/* ---- VirtualAlloc regions ----------------------------------------------------
 *
 * Page state for [WR_VMEM_LO, WR_VMEM_HI): 0 free, 1 reserved, 2 committed.
 * Allocation bases are 64 KiB aligned like Windows; a region remembers its
 * base and size so MEM_RELEASE frees exactly what was reserved.
 */
#define VPAGES ((WR_VMEM_HI - WR_VMEM_LO) / PAGE)
static uint8_t  *g_vstate;           /* VPAGES bytes */
static uint32_t *g_vbase;            /* allocation base per page (0 = free) */
static uint32_t  g_vhint = WR_VMEM_LO;

typedef struct fixed_range { uint32_t lo, hi, type, protect; } fixed_range;
static fixed_range g_fixed[16];
static int g_nfixed;

void wr_vmem_note_fixed(uint32_t lo, uint32_t hi, uint32_t type, uint32_t protect)
{
    if (g_nfixed < 16)
        g_fixed[g_nfixed++] = (fixed_range){ lo, hi, type, protect };
}

static int vmem_ready(void)
{
    if (g_vstate)
        return 1;
    g_vstate = (uint8_t *)calloc(VPAGES, 1);
    g_vbase = (uint32_t *)calloc(VPAGES, sizeof(uint32_t));
    return g_vstate && g_vbase;
}

static uint32_t vidx(uint32_t va) { return (va - WR_VMEM_LO) / PAGE; }

static uint32_t vmem_find_free(uint32_t size)
{
    const uint32_t gran = 0x10000u;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t start = pass ? WR_VMEM_LO : g_vhint;
        for (uint32_t a = (start + gran - 1) & ~(gran - 1);
             (uint64_t)a + size <= WR_VMEM_HI; a += gran) {
            uint32_t i = vidx(a), n = size / PAGE, k;
            for (k = 0; k < n; k++)
                if (g_vstate[i + k])
                    break;
            if (k == n) {
                g_vhint = a + size;
                return a;
            }
            a += (k / (gran / PAGE)) * gran;   /* skip what is known used */
        }
    }
    return 0;
}

uint32_t wr_vmem_alloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t protect)
{
    (void)protect;
    if (!vmem_ready() || !size)
        return 0;
    size = (size + PAGE - 1) & ~(PAGE - 1);

    if (addr == 0) {
        if (!(type & WR_MEM_RESERVE) && !(type & WR_MEM_COMMIT))
            return 0;
        uint32_t a = vmem_find_free(size);
        if (!a) {
            wr_log("VirtualAlloc: out of guest address space for 0x%X bytes", size);
            return 0;
        }
        for (uint32_t i = vidx(a), n = size / PAGE; n--; i++) {
            g_vstate[i] = (type & WR_MEM_COMMIT) ? 2 : 1;
            g_vbase[i] = a;
        }
        if (type & WR_MEM_COMMIT) {
            wr_mem_commit(a, size);
            memset(wr_ptr(a), 0, size);
        }
        return a;
    }

    /* A given address: inside our range, reserve there or commit. */
    uint32_t lo = addr & ~(PAGE - 1);
    uint32_t hi = (uint32_t)(((uint64_t)addr + size + PAGE - 1) & ~(uint64_t)(PAGE - 1));
    if (lo < WR_VMEM_LO || hi > WR_VMEM_HI) {
        /* Fixed areas (image, stack, heap) are always committed. */
        for (int f = 0; f < g_nfixed; f++)
            if (lo >= g_fixed[f].lo && hi <= g_fixed[f].hi && (type & WR_MEM_COMMIT))
                return addr;
        WR_SETERR(487);   /* ERROR_INVALID_ADDRESS */
        return 0;
    }
    if (type & WR_MEM_RESERVE) {
        uint32_t base64 = addr & ~0xFFFFu;
        lo = base64;
        for (uint32_t i = vidx(lo); i < vidx(hi); i++)
            if (g_vstate[i]) {
                WR_SETERR(487);
                return 0;
            }
        for (uint32_t i = vidx(lo); i < vidx(hi); i++) {
            g_vstate[i] = 1;
            g_vbase[i] = base64;
        }
    }
    if (type & WR_MEM_COMMIT) {
        for (uint32_t i = vidx(lo); i < vidx(hi); i++)
            if (!g_vstate[i]) {
                WR_SETERR(487);
                return 0;
            }
        wr_mem_commit(lo, hi - lo);
        for (uint32_t i = vidx(lo); i < vidx(hi); i++) {
            if (g_vstate[i] == 1)
                memset(wr_ptr(WR_VMEM_LO + i * PAGE), 0, PAGE);
            g_vstate[i] = 2;
        }
        return (type & WR_MEM_RESERVE) ? lo : addr;
    }
    return lo;
}

int wr_vmem_free(uint32_t addr, uint32_t size, uint32_t type)
{
    if (!vmem_ready() || addr < WR_VMEM_LO || addr >= WR_VMEM_HI)
        return 0;
    if (type & WR_MEM_RELEASE) {
        uint32_t i = vidx(addr);
        uint32_t base64 = g_vbase[i];
        if (base64 != addr || size != 0) {
            WR_SETERR(87);
            return 0;
        }
        uint32_t a = addr;
        while (a < WR_VMEM_HI && g_vbase[vidx(a)] == base64 && g_vstate[vidx(a)]) {
            g_vstate[vidx(a)] = 0;
            g_vbase[vidx(a)] = 0;
            a += PAGE;
        }
        wr_mem_decommit(addr, a - addr);
        if (addr < g_vhint)
            g_vhint = addr;
        return 1;
    }
    if (type & WR_MEM_DECOMMIT) {
        uint32_t lo = addr & ~(PAGE - 1);
        uint32_t hi = (uint32_t)(((uint64_t)addr + size + PAGE - 1) & ~(uint64_t)(PAGE - 1));
        if (size == 0) {   /* whole region from addr */
            uint32_t b = g_vbase[vidx(lo)];
            hi = lo;
            while (hi < WR_VMEM_HI && g_vbase[vidx(hi)] == b && g_vstate[vidx(hi)])
                hi += PAGE;
        }
        for (uint32_t i = vidx(lo); i < vidx(hi) && i < VPAGES; i++)
            if (g_vstate[i] == 2)
                g_vstate[i] = 1;
        wr_mem_decommit(lo, hi - lo);
        return 1;
    }
    return 0;
}

uint32_t wr_vmem_query(uint32_t addr, uint32_t mbi)
{
    uint32_t page = addr & ~(PAGE - 1);
    uint32_t base_ = page, alloc = 0, rsize = PAGE, state = WR_MEM_FREE, prot = 1 /*NOACCESS*/, type = 0, aprot = 0;

    for (int f = 0; f < g_nfixed; f++) {
        if (page >= g_fixed[f].lo && page < g_fixed[f].hi) {
            base_ = page;
            alloc = g_fixed[f].lo;
            rsize = g_fixed[f].hi - page;
            state = WR_MEM_COMMIT;
            prot = aprot = g_fixed[f].protect;
            type = g_fixed[f].type;
            goto out;
        }
    }
    if (vmem_ready() && page >= WR_VMEM_LO && page < WR_VMEM_HI) {
        uint32_t i = vidx(page);
        uint8_t st = g_vstate[i];
        uint32_t b = g_vbase[i];
        uint32_t end = page;
        while (end < WR_VMEM_HI && g_vstate[vidx(end)] == st && g_vbase[vidx(end)] == b)
            end += PAGE;
        rsize = end - page;
        if (st) {
            alloc = b;
            aprot = 4;   /* PAGE_READWRITE */
            type = WR_MEM_PRIVATE;
            state = st == 2 ? WR_MEM_COMMIT : WR_MEM_RESERVE;
            prot = st == 2 ? 4 : 0;
        }
    }
out:
    if (!wr_mem_valid(mbi, 28))
        return 0;
    wr_wr32(mbi + 0, base_);
    wr_wr32(mbi + 4, alloc);
    wr_wr32(mbi + 8, aprot);
    wr_wr32(mbi + 12, rsize);
    wr_wr32(mbi + 16, state);
    wr_wr32(mbi + 20, prot);
    wr_wr32(mbi + 24, type);
    return 28;
}

int wr_mem_valid(uint32_t va, uint32_t size)
{
    uint64_t end = (uint64_t)va + size;
    if (va < WR_NULL_GUARD || end > USER_HI)
        return 0;
    for (int f = 0; f < g_nfixed; f++)
        if (va >= g_fixed[f].lo && end <= g_fixed[f].hi)
            return 1;
    if (va >= WR_VMEM_LO && end <= WR_VMEM_HI && g_vstate) {
        for (uint32_t p = va & ~(PAGE - 1); p < end; p += PAGE)
            if (g_vstate[vidx(p)] != 2)
                return 0;
        return 1;
    }
    return 0;
}

/* ---- guest strings ------------------------------------------------------------ */

const char *wr_str(uint32_t va)
{
    return va ? (const char *)wr_ptr(va) : NULL;
}

size_t wr_wstr_to_host(uint32_t va, char *out, size_t cap)
{
    size_t n = 0;
    if (!cap)
        return 0;
    if (va)
        for (;; n++) {
            uint16_t c = wr_rd16(va + 2u * (uint32_t)n);
            if (!c || n + 1 >= cap)
                break;
            out[n] = c < 256 ? (char)c : '?';
        }
    out[n] = 0;
    return n;
}

size_t wr_put_str(uint32_t va, uint32_t cap, const char *s)
{
    size_t n = 0;
    if (!cap || !va)
        return 0;
    char *d = (char *)wr_ptr(va);
    while (s[n] && n + 1 < cap) {
        d[n] = s[n];
        n++;
    }
    d[n] = 0;
    return n;
}
