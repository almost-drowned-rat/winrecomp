/*
 * winrecomp runtime - guest heap (HeapAlloc, LocalAlloc, runtime data).
 *
 * Boundary-tag allocator in the arena [WR_HEAP_LO, WR_HEAP_HI). Every block
 * starts with a 16-byte header, so user pointers are 16-byte aligned (Windows
 * guarantees 8):
 *
 *   +0  size | flags   total block size incl. header, multiple of 16
 *                      bit0 INUSE, bit1 PREV_INUSE
 *   +4  prev_size      size of the physically previous block (0 for the
 *                      first); kept current at all times
 *   +8  req            requested size (HeapSize answers this)
 *   +12 magic          WR_HEAP_MAGIC ^ block address
 *
 * A free block keeps next/prev free-list links in its first user bytes. Free
 * blocks live in 64 exact bins (16..1024 bytes) and 32 log2 bins above; a
 * bitmap finds the next non-empty bin. The top of the arena is a wilderness
 * that grows by committing pages. All heaps (HeapCreate) share this one
 * allocator: the CRT only destroys its heap at exit.
 */
#include "wr.h"

#include <string.h>

#define HDR          16u
#define ALIGN        16u
#define MIN_BLOCK    32u
#define F_INUSE      1u
#define F_PREV_INUSE 2u
#define WR_HEAP_MAGIC 0x50414548u    /* "HEAP" */
#define NBINS        96

static uint32_t g_top;            /* first byte of the wilderness */
static uint32_t g_committed;      /* arena committed up to here */
static uint32_t g_bins[NBINS];    /* head block address per bin, 0 = empty */
static uint32_t g_binmap[(NBINS + 31) / 32];
static uint32_t g_last_block;     /* the block right below g_top, 0 if none */
static int      g_ready;

static inline uint32_t bsize(uint32_t b) { return wr_rd32(b) & ~15u; }
static inline uint32_t bflags(uint32_t b) { return wr_rd32(b) & 15u; }
static inline void set_hdr(uint32_t b, uint32_t size, uint32_t flags)
{
    wr_wr32(b, size | flags);
    wr_wr32(b + 12, WR_HEAP_MAGIC ^ b);
}
static inline uint32_t fnext(uint32_t b) { return wr_rd32(b + HDR); }
static inline uint32_t fprev(uint32_t b) { return wr_rd32(b + HDR + 4); }

static int bin_of(uint32_t size)
{
    if (size <= 1024)
        return (int)(size / 16) - 1;            /* 0..63 */
    int lg = 31 - __builtin_clz(size);          /* 10.. */
    int b = 64 + (lg - 10);
    return b < NBINS ? b : NBINS - 1;
}

static void bin_insert(uint32_t b)
{
    int i = bin_of(bsize(b));
    uint32_t head = g_bins[i];
    wr_wr32(b + HDR, head);
    wr_wr32(b + HDR + 4, 0);
    if (head)
        wr_wr32(head + HDR + 4, b);
    g_bins[i] = b;
    g_binmap[i / 32] |= 1u << (i % 32);
}

static void bin_remove(uint32_t b)
{
    int i = bin_of(bsize(b));
    uint32_t n = fnext(b), p = fprev(b);
    if (p)
        wr_wr32(p + HDR, n);
    else
        g_bins[i] = n;
    if (n)
        wr_wr32(n + HDR + 4, p);
    if (!g_bins[i])
        g_binmap[i / 32] &= ~(1u << (i % 32));
}

static void heap_init(void)
{
    if (g_ready)
        return;
    g_top = WR_HEAP_LO;
    g_committed = WR_HEAP_LO;
    g_ready = 1;
    wr_vmem_note_fixed(WR_HEAP_LO, WR_HEAP_HI, WR_MEM_PRIVATE, 4);
}

static int grow(uint32_t need_end)
{
    if (need_end > WR_HEAP_HI)
        return 0;
    if (need_end > g_committed) {
        uint32_t c = (need_end + 0xFFFFu) & ~0xFFFFu;
        if (c > WR_HEAP_HI)
            c = WR_HEAP_HI;
        if (!wr_mem_commit(g_committed, c - g_committed))
            return 0;
        g_committed = c;
    }
    return 1;
}

/* The block physically after b, or 0 when b is the last one. */
static inline uint32_t next_block(uint32_t b)
{
    uint32_t n = b + bsize(b);
    return n < g_top ? n : 0;
}

static uint32_t take_from_bins(uint32_t size)
{
    int i = bin_of(size);
    /* The exact-size bins hold one size each: any block there fits. The log
     * bins hold a range: walk the list for a fit first. */
    if (i >= 64) {
        for (uint32_t b = g_bins[i]; b; b = fnext(b))
            if (bsize(b) >= size) {
                bin_remove(b);
                return b;
            }
        i++;
    }
    for (int w = i / 32; w < (NBINS + 31) / 32; w++) {
        uint32_t m = g_binmap[w];
        if (w == i / 32)
            m &= ~0u << (i % 32);
        while (m) {
            int j = w * 32 + __builtin_ctz(m);
            uint32_t b = g_bins[j];
            if (b && bsize(b) >= size) {
                bin_remove(b);
                return b;
            }
            m &= m - 1;
        }
    }
    return 0;
}

static void split(uint32_t b, uint32_t size)
{
    uint32_t total = bsize(b);
    if (total - size < MIN_BLOCK)
        return;
    uint32_t rest = b + size;
    uint32_t rsize = total - size;
    set_hdr(b, size, bflags(b));
    set_hdr(rest, rsize, F_PREV_INUSE);   /* b is in use */
    wr_wr32(rest + 4, size);
    uint32_t n = next_block(rest);
    if (n) {
        wr_wr32(n + 4, rsize);
        set_hdr(n, bsize(n), bflags(n) & ~F_PREV_INUSE);
    } else {
        g_last_block = rest;
    }
    bin_insert(rest);
}

uint32_t wr_heap_alloc(uint32_t req, int zero)
{
    heap_init();
    if (req > 0x3F000000u)
        return 0;
    uint32_t size = (req + HDR + ALIGN - 1) & ~(ALIGN - 1);
    if (size < MIN_BLOCK)
        size = MIN_BLOCK;

    uint32_t b = take_from_bins(size);
    if (b) {
        set_hdr(b, bsize(b), bflags(b) | F_INUSE);
        uint32_t n = next_block(b);
        if (n)
            set_hdr(n, bsize(n), bflags(n) | F_PREV_INUSE);
        split(b, size);
    } else {
        b = g_top;
        if (!grow(b + size))
            return 0;
        uint32_t prev_inuse = (!g_last_block || (bflags(g_last_block) & F_INUSE)) ? F_PREV_INUSE : 0;
        set_hdr(b, size, F_INUSE | prev_inuse);
        wr_wr32(b + 4, g_last_block ? bsize(g_last_block) : 0);
        g_top = b + size;
        g_last_block = b;
    }
    wr_wr32(b + 8, req);
    uint32_t user = b + HDR;
    if (zero)
        memset(wr_ptr(user), 0, bsize(b) - HDR);
    return user;
}

static int is_block(uint32_t user)
{
    if (!g_ready || user < WR_HEAP_LO + HDR || user >= g_top || (user & (ALIGN - 1)))
        return 0;
    uint32_t b = user - HDR;
    return wr_rd32(b + 12) == (WR_HEAP_MAGIC ^ b) && (bflags(b) & F_INUSE);
}

int wr_heap_free(uint32_t user)
{
    if (!user)
        return 1;
    if (!is_block(user)) {
        wr_log("heap: free of 0x%08X, not a live heap block (ignored)", user);
        return 0;
    }
    uint32_t b = user - HDR;
    uint32_t size = bsize(b);
    uint32_t flags = bflags(b) & ~F_INUSE;

    /* Merge with the previous block if it is free. */
    if (!(flags & F_PREV_INUSE)) {
        uint32_t psize = wr_rd32(b + 4);
        uint32_t p = b - psize;
        bin_remove(p);
        size += psize;
        flags = bflags(p) & ~F_INUSE;
        b = p;
    }
    /* Merge with the next one. */
    uint32_t n = b + size;
    if (n < g_top && !(bflags(n) & F_INUSE)) {
        bin_remove(n);
        size += bsize(n);
    }
    n = b + size;
    if (n >= g_top) {
        /* Back into the wilderness. */
        g_top = b;
        g_last_block = (b == WR_HEAP_LO) ? 0 : b - wr_rd32(b + 4);
        return 1;
    }
    set_hdr(b, size, flags);
    wr_wr32(n + 4, size);
    set_hdr(n, bsize(n), bflags(n) & ~F_PREV_INUSE);
    bin_insert(b);
    return 1;
}

uint32_t wr_heap_size(uint32_t user)
{
    return is_block(user) ? wr_rd32(user - HDR + 8) : 0xFFFFFFFFu;
}

uint32_t wr_heap_realloc(uint32_t user, uint32_t req, int zero, int in_place_only)
{
    if (!user)
        return in_place_only ? 0 : wr_heap_alloc(req, zero);
    if (!is_block(user)) {
        wr_log("heap: realloc of 0x%08X, not a live heap block", user);
        return 0;
    }
    uint32_t b = user - HDR;
    uint32_t old_req = wr_rd32(b + 8);
    uint32_t need = (req + HDR + ALIGN - 1) & ~(ALIGN - 1);
    if (need < MIN_BLOCK)
        need = MIN_BLOCK;
    uint32_t have = bsize(b);

    if (need <= have) {
        split(b, need);
        wr_wr32(b + 8, req);
        return user;
    }
    /* Grow into a free neighbour or the wilderness. */
    uint32_t n = b + have;
    if (n >= g_top) {
        if (grow(b + need)) {
            set_hdr(b, need, bflags(b));
            g_top = b + need;
            g_last_block = b;
            wr_wr32(b + 8, req);
            if (zero && req > old_req)
                memset(wr_ptr(user + old_req), 0, req - old_req);
            return user;
        }
    } else if (!(bflags(n) & F_INUSE) && have + bsize(n) >= need) {
        bin_remove(n);
        uint32_t total = have + bsize(n);
        set_hdr(b, total, bflags(b));
        uint32_t nn = next_block(b);
        if (nn) {
            set_hdr(nn, bsize(nn), bflags(nn) | F_PREV_INUSE);
            wr_wr32(nn + 4, total);
        } else {
            g_last_block = b;
        }
        split(b, need);
        wr_wr32(b + 8, req);
        if (zero && req > old_req)
            memset(wr_ptr(user + old_req), 0, req - old_req);
        return user;
    }
    if (in_place_only)
        return 0;
    uint32_t nu = wr_heap_alloc(req, 0);
    if (!nu)
        return 0;
    memcpy(wr_ptr(nu), wr_ptr(user), old_req < req ? old_req : req);
    if (zero && req > old_req)
        memset(wr_ptr(nu + old_req), 0, req - old_req);
    wr_heap_free(user);
    return nu;
}

int wr_heap_check(void)
{
    uint32_t s = WR_HEAP_LO, prev_free = 0, last_size = 0;
    if (!g_ready)
        return 1;
    while (s < g_top) {
        uint32_t sz = bsize(s);
        if (wr_rd32(s + 12) != (WR_HEAP_MAGIC ^ s) || sz < MIN_BLOCK || s + sz > g_top) {
            wr_log("heap: corrupt block at 0x%08X (size 0x%X)", s, sz);
            return 0;
        }
        int inuse = bflags(s) & F_INUSE;
        if (s > WR_HEAP_LO && wr_rd32(s + 4) != last_size) {
            wr_log("heap: prev_size mismatch at 0x%08X", s);
            return 0;
        }
        last_size = sz;
        if (prev_free && !inuse) {
            wr_log("heap: two free blocks in a row at 0x%08X", s);
            return 0;
        }
        prev_free = !inuse;
        s += sz;
    }
    return 1;
}

uint32_t wr_galloc(uint32_t size)
{
    uint32_t va = wr_heap_alloc(size, 1);
    if (!va)
        wr_fatal("out of guest heap (0x%X bytes)", size);
    return va;
}

uint32_t wr_gstrdup(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t va = wr_galloc(n);
    memcpy(wr_ptr(va), s, n);
    return va;
}
