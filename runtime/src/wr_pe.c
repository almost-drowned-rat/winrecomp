/*
 * winrecomp runtime - PE image mapping, native modules, synthetic addresses.
 */
#include "wr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

wr_image g_wr_image;

/* ---- synthetic addresses ------------------------------------------------------ */

typedef struct thunk { wr_handler_t fn; const char *label; } thunk;
static thunk    g_thunks[WR_THUNK_MAX];
static uint32_t g_nthunks;

uint32_t wr_thunk_for(wr_handler_t fn, const char *label)
{
    for (uint32_t i = 0; i < g_nthunks; i++)
        if (g_thunks[i].fn == fn)
            return WR_THUNK_BASE + 4u * i;
    if (g_nthunks >= WR_THUNK_MAX)
        wr_fatal("out of synthetic addresses");
    g_thunks[g_nthunks].fn = fn;
    g_thunks[g_nthunks].label = label;
    return WR_THUNK_BASE + 4u * g_nthunks++;
}

const char *wr_thunk_label(uint32_t va)
{
    uint32_t i = (va - WR_THUNK_BASE) / 4u;
    if (va < WR_THUNK_BASE || i >= g_nthunks)
        return NULL;
    return g_thunks[i].label;
}

recomp_func_t recomp_lookup_kernel(uint32_t va)
{
    uint32_t i = (va - WR_THUNK_BASE) / 4u;
    if (va < WR_THUNK_BASE || (va & 3u) || i >= g_nthunks)
        return NULL;
    return g_thunks[i].fn;
}

/* ---- modules ------------------------------------------------------------------ */

#define MAX_MODULES 64
static wr_module *g_modules[MAX_MODULES];
static int g_nmodules;

static int name_eq(const char *a, const char *b)
{
    /* "kernel32" matches "KERNEL32.dll"; a path prefix is ignored. */
    const char *sa = strrchr(a, '\\'), *sb = strrchr(b, '\\');
    a = sa ? sa + 1 : a;
    b = sb ? sb + 1 : b;
    size_t la = strlen(a), lb = strlen(b);
    if (la > 4 && !strcasecmp(a + la - 4, ".dll")) la -= 4;
    if (lb > 4 && !strcasecmp(b + lb - 4, ".dll")) lb -= 4;
    return la == lb && !strncasecmp(a, b, la);
}

void wr_module_register(wr_module *m)
{
    if (wr_module_find(m->name))
        return;
    if (g_nmodules >= MAX_MODULES)
        wr_fatal("too many native modules");
    /* Fake HMODULEs: page-aligned values in the never-mapped top range. */
    m->handle = 0xFD000000u + 0x10000u * (uint32_t)g_nmodules;
    g_modules[g_nmodules++] = m;
}

wr_module *wr_module_find(const char *name)
{
    for (int i = 0; i < g_nmodules; i++)
        if (name_eq(g_modules[i]->name, name))
            return g_modules[i];
    return NULL;
}

wr_module *wr_module_by_handle(uint32_t h)
{
    for (int i = 0; i < g_nmodules; i++)
        if (g_modules[i]->handle == h)
            return g_modules[i];
    return NULL;
}

uint32_t wr_module_proc(wr_module *m, const char *name)
{
    for (const wr_export *e = m->exports; e->name; e++)
        if (!strcmp(e->name, name)) {
            static char labels[WR_THUNK_MAX][80];
            static int nl;
            const char *label = e->name;
            if (nl < (int)WR_THUNK_MAX) {
                snprintf(labels[nl], sizeof labels[nl], "%s!%s", m->name, e->name);
                label = labels[nl++];
            }
            return wr_thunk_for(e->fn, label);
        }
    return 0;
}

/* ---- unknown imports ---------------------------------------------------------- */

/* Each unbound import gets its own slot so the stop names it. */
#define MAX_UNBOUND 256
static char g_unbound_names[MAX_UNBOUND][96];
static int  g_nunbound;

static void unbound_hit(int i)
{
    wr_fatal("unimplemented import %s called", g_unbound_names[i]);
}

/* n is three octal digits; 0##n makes it an octal literal equal to the index. */
#define U(n) static void unbound_##n(RECOMP_PARAMS) { (void)ctx; (void)base; unbound_hit(0##n); }
#define U8(n) U(n##0) U(n##1) U(n##2) U(n##3) U(n##4) U(n##5) U(n##6) U(n##7)
#define U64(n) U8(n##0) U8(n##1) U8(n##2) U8(n##3) U8(n##4) U8(n##5) U8(n##6) U8(n##7)
U64(0) U64(1) U64(2) U64(3)
#undef U
#define U(n) unbound_##n,
static const wr_handler_t g_unbound_fns[MAX_UNBOUND] = { U64(0) U64(1) U64(2) U64(3) };
#undef U
#undef U8
#undef U64

/* Entry i is unbound_<i in three octal digits>, laid out in index order. */
static wr_handler_t unbound_fn(int i) { return g_unbound_fns[i]; }

/* ---- PE loading --------------------------------------------------------------- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* SHA-1 (for the variant check and the log) */
static void sha1(const uint8_t *data, size_t len, uint8_t out[20]);

int wr_pe_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        wr_log("pe: cannot open %s", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = (uint8_t *)malloc((size_t)len);
    if (!file || fread(file, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(file);
        wr_log("pe: cannot read %s", path);
        return 0;
    }
    fclose(f);

    if (len < 0x200 || file[0] != 'M' || file[1] != 'Z') {
        wr_log("pe: %s is not an MZ executable", path);
        free(file);
        return 0;
    }
    uint32_t pe = rd32(file + 0x3C);
    if (pe + 0xF8 > (uint32_t)len || memcmp(file + pe, "PE\0\0", 4) || rd16(file + pe + 4) != 0x14C) {
        wr_log("pe: %s is not an i386 PE", path);
        free(file);
        return 0;
    }
    const uint8_t *fh = file + pe + 4;
    const uint8_t *oh = fh + 20;
    uint16_t nsec = rd16(fh + 2);
    uint16_t ohsize = rd16(fh + 16);
    uint32_t ibase = rd32(oh + 28);
    uint32_t isize = rd32(oh + 56);
    uint32_t hsize = rd32(oh + 60);
    uint32_t entry = rd32(oh + 16);
    uint32_t imp_rva = rd32(oh + 96 + 8);
    uint32_t iat_rva = rd32(oh + 96 + 12 * 8);
    uint32_t iat_size = rd32(oh + 96 + 12 * 8 + 4);

    if (!wr_mem_commit(ibase, isize)) {
        wr_log("pe: cannot commit image range 0x%08X+0x%X", ibase, isize);
        free(file);
        return 0;
    }
    memset(wr_ptr(ibase), 0, isize);
    memcpy(wr_ptr(ibase), file, hsize < (uint32_t)len ? hsize : (uint32_t)len);

    g_wr_image.base = ibase;
    g_wr_image.size = isize;
    g_wr_image.entry = ibase + entry;
    g_wr_image.code_lo = 0xFFFFFFFFu;
    g_wr_image.code_hi = 0;
    g_wr_image.iat_lo = ibase + iat_rva;
    g_wr_image.iat_hi = ibase + iat_rva + iat_size;
    snprintf(g_wr_image.host_path, sizeof g_wr_image.host_path, "%s", path);
    sha1(file, (size_t)len, g_wr_image.sha1);

    const uint8_t *sh = oh + ohsize;
    for (int i = 0; i < nsec; i++, sh += 40) {
        uint32_t vsize = rd32(sh + 8), va = rd32(sh + 12);
        uint32_t rsize = rd32(sh + 16), roff = rd32(sh + 20);
        uint32_t ch = rd32(sh + 36);
        uint32_t n = rsize < vsize || !vsize ? rsize : vsize;
        if (roff + n > (uint32_t)len)
            n = roff < (uint32_t)len ? (uint32_t)len - roff : 0;
        memcpy(wr_ptr(ibase + va), file + roff, n);
        if (ch & 0x20000020u) {   /* CODE / EXECUTE */
            if (ibase + va < g_wr_image.code_lo) g_wr_image.code_lo = ibase + va;
            if (ibase + va + vsize > g_wr_image.code_hi) g_wr_image.code_hi = ibase + va + vsize;
        }
        char name[9] = { 0 };
        memcpy(name, sh, 8);
        wr_log("pe: section %-8s 0x%08X +0x%06X (file 0x%X)", name, ibase + va, vsize, rsize);
    }
    g_xbox_code_lo = g_wr_image.code_lo;
    g_xbox_code_hi = g_wr_image.code_hi;
    wr_vmem_note_fixed(ibase, ibase + isize, WR_MEM_IMAGE, 0x40 /* PAGE_EXECUTE_READWRITE */);

    /* Imports: write a synthetic address into every IAT slot. */
    int bound = 0, unbound = 0;
    for (uint32_t d = imp_rva; d && rd32(wr_ptr(ibase + d + 12)); d += 20) {
        const uint8_t *desc = (const uint8_t *)wr_ptr(ibase + d);
        uint32_t ilt = rd32(desc + 0), name_rva = rd32(desc + 12), iat = rd32(desc + 16);
        const char *dll = (const char *)wr_ptr(ibase + name_rva);
        wr_module *m = wr_module_find(dll);
        if (!ilt)
            ilt = iat;
        for (uint32_t k = 0;; k++) {
            uint32_t e = wr_rd32(ibase + ilt + 4 * k);
            if (!e)
                break;
            char fname[80];
            if (e & 0x80000000u)
                snprintf(fname, sizeof fname, "#%u", e & 0xFFFFu);
            else
                snprintf(fname, sizeof fname, "%s", (const char *)wr_ptr(ibase + e + 2));
            uint32_t addr = m ? wr_module_proc(m, fname) : 0;
            if (addr) {
                bound++;
            } else {
                if (g_nunbound >= MAX_UNBOUND)
                    wr_fatal("too many unbound imports");
                snprintf(g_unbound_names[g_nunbound], sizeof g_unbound_names[0], "%s!%s", dll, fname);
                addr = wr_thunk_for(unbound_fn(g_nunbound), g_unbound_names[g_nunbound]);
                g_nunbound++;
                unbound++;
                wr_log("pe: import %s!%s not implemented", dll, fname);
            }
            wr_wr32(ibase + iat + 4 * k, addr);
        }
    }
    wr_log("pe: %s: base 0x%08X size 0x%X entry 0x%08X, %d imports bound, %d unbound",
           path, ibase, isize, g_wr_image.entry, bound, unbound);
    free(file);
    return 1;
}

/* ---- SHA-1 (public domain style, compact) ------------------------------------- */

#define ROL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    size_t i = 0;
    for (; i + 64 <= len; i += 64)
        sha1_block(h, data + i);
    uint8_t tail[128] = { 0 };
    size_t r = len - i;
    memcpy(tail, data + i, r);
    tail[r] = 0x80;
    size_t tl = r + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++)
        tail[tl - 1 - k] = (uint8_t)(bits >> (8 * k));
    sha1_block(h, tail);
    if (tl == 128)
        sha1_block(h, tail + 64);
    for (int k = 0; k < 5; k++) {
        out[4 * k] = (uint8_t)(h[k] >> 24);
        out[4 * k + 1] = (uint8_t)(h[k] >> 16);
        out[4 * k + 2] = (uint8_t)(h[k] >> 8);
        out[4 * k + 3] = (uint8_t)h[k];
    }
}
