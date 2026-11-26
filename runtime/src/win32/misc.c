/*
 * winrecomp runtime - GDI32, ADVAPI32 (registry), SHELL32, ole32, VERSION,
 * WINMM, DPLAYX, and the module registration of the whole Win32 layer.
 *
 * The registry lives in saves/registry.txt (portable layout: nothing outside
 * the release folder). Format, one value per line under its key:
 *   [HKEY_LOCAL_MACHINE\SOFTWARE\Vendor\Game\Screen]
 *   "name"=dword:00000001
 *   "name"="text"
 *   "name"=hex:01,02,03
 */
#include "w32_internal.h"
#include "../platform/wr_platform.h"

#include <stdlib.h>

/* ---- GDI32 ----------------------------------------------------------------------- */

WR_HANDLER(g_GetDeviceCaps)
{
    (void)base;
    int w, h;
    wr_display_size(&w, &h);
    uint32_t i = WR_ARG(1), v = 0;
    switch (i) {
    case 8: v = (uint32_t)w; break;    /* HORZRES */
    case 10: v = (uint32_t)h; break;   /* VERTRES */
    case 12: v = 32; break;            /* BITSPIXEL */
    case 14: v = 1; break;             /* PLANES */
    case 24: v = 0xFFFFFFFFu; break;   /* NUMCOLORS: more than 256 */
    case 88: case 90: v = 96; break;   /* LOGPIXELSX/Y */
    case 116: v = 60; break;           /* VREFRESH */
    default: WR_ONCE("GetDeviceCaps(%u) answered 0", i); break;
    }
    WR_TRACE("GetDeviceCaps(%u) -> %u", i, v);
    WR_RET(v, 2);
}

/* ---- registry -------------------------------------------------------------------- */

typedef struct rval { char name[128]; uint32_t type, len; uint8_t *data; } rval;
typedef struct rkey { char path[512]; rval *vals; int nvals, cap; } rkey;

static rkey *g_keys;
static int g_nkeys, g_keycap, g_reg_dirty, g_reg_loaded;

static const char *root_name(uint32_t h)
{
    switch (h) {
    case 0x80000000u: return "HKEY_CLASSES_ROOT";
    case 0x80000001u: return "HKEY_CURRENT_USER";
    case 0x80000002u: return "HKEY_LOCAL_MACHINE";
    case 0x80000003u: return "HKEY_USERS";
    case 0x80000005u: return "HKEY_CURRENT_CONFIG";
    default: return NULL;
    }
}

static rkey *key_find(const char *path, int create)
{
    for (int i = 0; i < g_nkeys; i++)
        if (!strcasecmp(g_keys[i].path, path))
            return &g_keys[i];
    if (!create)
        return NULL;
    if (g_nkeys == g_keycap) {
        g_keycap = g_keycap ? 2 * g_keycap : 32;
        g_keys = (rkey *)realloc(g_keys, (size_t)g_keycap * sizeof *g_keys);
    }
    rkey *k = &g_keys[g_nkeys++];
    memset(k, 0, sizeof *k);
    snprintf(k->path, sizeof k->path, "%s", path);
    return k;
}

static rval *val_find(rkey *k, const char *name, int create)
{
    for (int i = 0; i < k->nvals; i++)
        if (!strcasecmp(k->vals[i].name, name))
            return &k->vals[i];
    if (!create)
        return NULL;
    if (k->nvals == k->cap) {
        k->cap = k->cap ? 2 * k->cap : 8;
        k->vals = (rval *)realloc(k->vals, (size_t)k->cap * sizeof *k->vals);
    }
    rval *v = &k->vals[k->nvals++];
    memset(v, 0, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", name);
    return v;
}

static void val_set(rval *v, uint32_t type, const void *data, uint32_t len)
{
    free(v->data);
    v->data = (uint8_t *)malloc(len ? len : 1);
    if (len)
        memcpy(v->data, data, len);
    v->len = len;
    v->type = type;
}

/* Port hook: a default value written only if the value does not exist. */
void wr_reg_default(const char *key, const char *name, uint32_t type, const void *data, uint32_t len)
{
    w32_registry_load();
    rval *v = val_find(key_find(key, 1), name, 1);
    if (!v->data)
        val_set(v, type, data, len);
}

/* Port hook: read a DWORD value; 1 if present. */
int wr_reg_get_dword(const char *key, const char *name, uint32_t *out)
{
    w32_registry_load();
    rkey *k = key_find(key, 0);
    rval *v = k ? val_find(k, name, 0) : NULL;
    if (!v || v->type != 4 || v->len != 4)
        return 0;
    memcpy(out, v->data, 4);
    return 1;
}

void wr_reg_default_dword(const char *key, const char *name, uint32_t value)
{
    wr_reg_default(key, name, 4, &value, 4);
}

void wr_reg_default_str(const char *key, const char *name, const char *s)
{
    wr_reg_default(key, name, 1, s, (uint32_t)strlen(s) + 1);
}

static void reg_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/registry.txt", g_wr_paths.saves);
}

void w32_registry_load(void)
{
    if (g_reg_loaded)
        return;
    g_reg_loaded = 1;
    char path[1200];
    reg_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    char line[4096];
    rkey *k = NULL;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        if (line[0] == '[') {
            char *e = strrchr(line, ']');
            if (e) *e = 0;
            k = key_find(line + 1, 1);
            continue;
        }
        if (!k || line[0] != '"')
            continue;
        char *q = strchr(line + 1, '"');
        if (!q || q[1] != '=')
            continue;
        *q = 0;
        rval *v = val_find(k, line + 1, 1);
        char *d = q + 2;
        if (!strncmp(d, "dword:", 6)) {
            uint32_t x = (uint32_t)strtoul(d + 6, NULL, 16);
            val_set(v, 4, &x, 4);
        } else if (d[0] == '"') {
            char s[4096];
            size_t n = 0;
            for (char *p = d + 1; *p && *p != '"' && n < sizeof s - 1; p++) {
                if (*p == '\\' && p[1]) p++;
                s[n++] = *p;
            }
            s[n] = 0;
            val_set(v, 1, s, (uint32_t)n + 1);
        } else if (!strncmp(d, "hex", 3)) {
            uint32_t type = 3;
            char *c = strchr(d, ':');
            if (d[3] == '(') type = (uint32_t)strtoul(d + 4, NULL, 16);
            uint8_t b[2048];
            uint32_t n = 0;
            for (char *p = c ? c + 1 : d + 4; *p && n < sizeof b;) {
                b[n++] = (uint8_t)strtoul(p, &p, 16);
                while (*p == ',' || *p == ' ') p++;
            }
            val_set(v, type, b, n);
        }
    }
    fclose(f);
}

void w32_registry_save(void)
{
    if (!g_reg_dirty)
        return;
    char path[1200], tmp[1210];
    reg_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        wr_log("registry: cannot write %s", tmp);
        return;
    }
    for (int i = 0; i < g_nkeys; i++) {
        rkey *k = &g_keys[i];
        if (!k->nvals)
            continue;
        fprintf(f, "[%s]\n", k->path);
        for (int j = 0; j < k->nvals; j++) {
            rval *v = &k->vals[j];
            fprintf(f, "\"%s\"=", v->name);
            if (v->type == 4 && v->len == 4) {
                uint32_t x;
                memcpy(&x, v->data, 4);
                fprintf(f, "dword:%08x\n", x);
            } else if (v->type == 1 && v->len && !memchr(v->data, '\n', v->len)) {
                fputc('"', f);
                for (uint32_t c = 0; c + 1 < v->len || (c < v->len && v->data[c]); c++) {
                    if (!v->data[c]) break;
                    if (v->data[c] == '"' || v->data[c] == '\\') fputc('\\', f);
                    fputc(v->data[c], f);
                }
                fputs("\"\n", f);
            } else {
                if (v->type == 3) fputs("hex:", f); else fprintf(f, "hex(%x):", v->type);
                for (uint32_t c = 0; c < v->len; c++)
                    fprintf(f, c ? ",%02x" : "%02x", v->data[c]);
                fputc('\n', f);
            }
        }
        fputc('\n', f);
    }
    fclose(f);
    /* replace atomically (POSIX rename; Windows needs the target gone) */
#if defined(_WIN32)
    remove(path);
#endif
    if (rename(tmp, path) != 0)
        wr_log("registry: cannot replace %s", path);
    g_reg_dirty = 0;
}

typedef struct hkey { char path[512]; } hkey;
static void hkey_close(void *d) { free(d); }

static const char *key_path_of(uint32_t h)
{
    const char *r = root_name(h);
    if (r)
        return r;
    wr_hobj *o = wr_handle_get(h, WR_H_REGKEY);
    return o ? ((hkey *)o->data)->path : NULL;
}

static uint32_t open_key(uint32_t parent, const char *sub, int create, uint32_t out, uint32_t disp)
{
    w32_registry_load();
    const char *pp = key_path_of(parent);
    if (!pp)
        return ERROR_INVALID_HANDLE;
    char path[512];
    if (sub && *sub)
        snprintf(path, sizeof path, "%s\\%s", pp, sub);
    else
        snprintf(path, sizeof path, "%s", pp);
    size_t l = strlen(path);
    while (l && path[l - 1] == '\\')
        path[--l] = 0;
    rkey *k = key_find(path, 0);
    int existed = k != NULL;
    /* A key with sub keys exists too. */
    if (!k) {
        for (int i = 0; i < g_nkeys && !existed; i++)
            if (!strncasecmp(g_keys[i].path, path, l) && g_keys[i].path[l] == '\\')
                existed = 1;
    }
    if (!existed && !create) {
        WR_TRACE("RegOpenKeyExA(%s) -> not found", path);
        return ERROR_FILE_NOT_FOUND;
    }
    if (!k)
        k = key_find(path, 1);
    hkey *hk = (hkey *)calloc(1, sizeof *hk);
    snprintf(hk->path, sizeof hk->path, "%s", path);
    wr_wr32(out, wr_handle_new(WR_H_REGKEY, hk, hkey_close));
    if (disp)
        wr_wr32(disp, existed ? 2u : 1u);   /* REG_OPENED_EXISTING_KEY / REG_CREATED_NEW_KEY */
    WR_TRACE("Reg%sKey(%s) -> %s", create ? "Create" : "Open", path, existed ? "existing" : "created");
    return ERROR_SUCCESS;
}

WR_HANDLER(a_RegOpenKeyExA)
{
    (void)base;
    WR_RET(open_key(WR_ARG(0), wr_str(WR_ARG(1)), 0, WR_ARG(4), 0), 5);
}

WR_HANDLER(a_RegCreateKeyExA)
{
    (void)base;
    WR_RET(open_key(WR_ARG(0), wr_str(WR_ARG(1)), 1, WR_ARG(7), WR_ARG(8)), 9);
}

WR_HANDLER(a_RegCloseKey)
{
    (void)base;
    if (root_name(WR_ARG(0)))
        WR_RET(ERROR_SUCCESS, 1);
    WR_RET(wr_handle_close(WR_ARG(0)) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE, 1);
}

WR_HANDLER(a_RegQueryValueExA)
{
    (void)base;
    const char *kp = key_path_of(WR_ARG(0));
    const char *name = wr_str(WR_ARG(1));
    uint32_t ptype = WR_ARG(3), data = WR_ARG(4), plen = WR_ARG(5);
    if (!kp)
        WR_RET(ERROR_INVALID_HANDLE, 6);
    w32_registry_load();
    rkey *k = key_find(kp, 0);
    rval *v = k ? val_find(k, name ? name : "", 0) : NULL;
    if (!v) {
        WR_TRACE("RegQueryValueExA(%s, %s) -> missing", kp, name ? name : "");
        WR_RET(ERROR_FILE_NOT_FOUND, 6);
    }
    if (ptype)
        wr_wr32(ptype, v->type);
    uint32_t cap = plen ? wr_rd32(plen) : 0;
    if (plen)
        wr_wr32(plen, v->len);
    if (!data)
        WR_RET(ERROR_SUCCESS, 6);
    if (cap < v->len)
        WR_RET(ERROR_MORE_DATA, 6);
    memcpy(wr_ptr(data), v->data, v->len);
    WR_TRACE("RegQueryValueExA(%s, %s) -> type %u, %u bytes", kp, name ? name : "", v->type, v->len);
    WR_RET(ERROR_SUCCESS, 6);
}

WR_HANDLER(a_RegSetValueExA)
{
    (void)base;
    const char *kp = key_path_of(WR_ARG(0));
    const char *name = wr_str(WR_ARG(1));
    uint32_t type = WR_ARG(3), data = WR_ARG(4), len = WR_ARG(5);
    if (!kp)
        WR_RET(ERROR_INVALID_HANDLE, 6);
    if (len > 65536 || (len && !wr_mem_valid(data, len)))
        WR_RET(ERROR_INVALID_PARAMETER, 6);
    w32_registry_load();
    rval *v = val_find(key_find(kp, 1), name ? name : "", 1);
    val_set(v, type, len ? wr_ptr(data) : "", len);
    g_reg_dirty = 1;
    w32_registry_save();   /* at once: survives crashes and app kills (atomic replace) */
    WR_TRACE("RegSetValueExA(%s, %s, type %u, %u bytes)", kp, name ? name : "", type, len);
    WR_RET(ERROR_SUCCESS, 6);
}

WR_HANDLER(a_RegDeleteValueA)
{
    (void)base;
    const char *kp = key_path_of(WR_ARG(0));
    const char *name = wr_str(WR_ARG(1));
    rkey *k = kp ? key_find(kp, 0) : NULL;
    if (!k)
        WR_RET(ERROR_FILE_NOT_FOUND, 2);
    for (int i = 0; i < k->nvals; i++)
        if (!strcasecmp(k->vals[i].name, name ? name : "")) {
            free(k->vals[i].data);
            k->vals[i] = k->vals[--k->nvals];
            g_reg_dirty = 1;
            w32_registry_save();
            WR_RET(ERROR_SUCCESS, 2);
        }
    WR_RET(ERROR_FILE_NOT_FOUND, 2);
}

WR_HANDLER(a_GetUserNameA)
{
    (void)base;
    uint32_t buf = WR_ARG(0), plen = WR_ARG(1);
    const char *name = "Player";
    uint32_t need = (uint32_t)strlen(name) + 1;
    if (wr_rd32(plen) < need) {
        wr_wr32(plen, need);
        WR_SETERR(ERROR_INSUFFICIENT_BUFFER);
        WR_RET(0, 2);
    }
    wr_put_str(buf, need, name);
    wr_wr32(plen, need);
    WR_RET(1, 2);
}

WR_HANDLER(a_IsTextUnicode)
{
    (void)base;
    if (WR_ARG(2))
        wr_wr32(WR_ARG(2), 0);
    WR_RET(0, 3);
}

/* ---- SHELL32 / ole32 ----------------------------------------------------------- */

WR_HANDLER(s_ShellExecuteA)
{
    (void)base;
    const char *op = wr_str(WR_ARG(1)), *file = wr_str(WR_ARG(2));
    wr_log("ShellExecuteA(%s, %s): not run", op ? op : "open", file ? file : "");
    WR_RET(2, 6);   /* SE_ERR_FNF: nothing was started */
}

WR_HANDLER(o_CoInitialize) { (void)base; WR_RET(0, 1); }
WR_HANDLER(o_CoUninitialize) { (void)base; WR_RET(0, 0); }

WR_HANDLER(o_CoCreateInstance)
{
    (void)base;
    uint32_t clsid = WR_ARG(0), ppv = WR_ARG(4);
    if (ppv)
        wr_wr32(ppv, 0);
    const uint8_t *g = (const uint8_t *)wr_ptr(clsid);
    wr_log("CoCreateInstance({%02X%02X%02X%02X-...}): class not registered", g[3], g[2], g[1], g[0]);
    WR_RET(0x80040154u, 5);   /* REGDB_E_CLASSNOTREG */
}

/* ---- VERSION: system DLLs answer as DirectX 8.1 files (4.08.01.0881) ----------- */

static int is_system_dll(const char *path)
{
    static const char *dx[] = { "ddraw", "dinput", "dinput8", "dsound", "d3d8", "d3dim", "d3dim700",
                                "dplayx", "dmusic", "dsetup", "dxdiagn", "dpnet", "d3d8thk", NULL };
    const char *s = strrchr(path, '\\');
    s = s ? s + 1 : path;
    for (int i = 0; dx[i]; i++) {
        size_t l = strlen(dx[i]);
        if (!strncasecmp(s, dx[i], l) && (!s[l] || s[l] == '.'))
            return 1;
    }
    return 0;
}

#define VER_BLOCK 0x80

/* The game's own exe: its RT_VERSION resource from the mapped image. */
static int is_game_exe(const char *path)
{
    const char *s = strrchr(path, '\\');
    s = s ? s + 1 : path;
    return !strcasecmp(s, g_wr_exe_name);
}

static uint32_t res_find(uint32_t rsrc, uint32_t dir, uint32_t id, int any)
{
    uint16_t named = wr_rd16(dir + 12), ids = wr_rd16(dir + 14);
    for (uint32_t i = 0; i < (uint32_t)named + ids; i++) {
        uint32_t e = dir + 16 + 8 * i;
        uint32_t name = wr_rd32(e), off = wr_rd32(e + 4);
        if (any || (!(name & 0x80000000u) && name == id))
            return off;
    }
    (void)rsrc;
    return 0xFFFFFFFFu;
}

/* VS_VERSIONINFO of the image: guest address + size, 0 if none. */
static uint32_t image_version(uint32_t *size)
{
    uint32_t b = g_wr_image.base;
    uint32_t pe = b + wr_rd32(b + 0x3C);
    uint32_t rva = wr_rd32(pe + 24 + 96 + 2 * 8);
    if (!rva)
        return 0;
    uint32_t root = b + rva;
    uint32_t o = res_find(root, root, 16, 0);                  /* RT_VERSION */
    if (o == 0xFFFFFFFFu || !(o & 0x80000000u)) return 0;
    o = res_find(root, root + (o & 0x7FFFFFFFu), 0, 1);       /* first name */
    if (o == 0xFFFFFFFFu || !(o & 0x80000000u)) return 0;
    o = res_find(root, root + (o & 0x7FFFFFFFu), 0, 1);       /* first language */
    if (o == 0xFFFFFFFFu || (o & 0x80000000u)) return 0;
    uint32_t data = root + o;
    *size = wr_rd32(data + 4);
    return b + wr_rd32(data);
}

WR_HANDLER(v_GetFileVersionInfoSizeA)
{
    (void)base;
    const char *p = wr_str(WR_ARG(0));
    if (WR_ARG(1))
        wr_wr32(WR_ARG(1), 0);
    uint32_t vsize = 0;
    if (p && is_game_exe(p) && image_version(&vsize)) {
        wr_log("GetFileVersionInfoSizeA(%s) -> the image's version resource", p);
        WR_RET(vsize + 16, 2);
    }
    int ok = p && is_system_dll(p);
    wr_log("GetFileVersionInfoSizeA(%s) -> %s", p ? p : "", ok ? "DirectX 8.1 file" : "no version info");
    if (!ok)
        WR_SETERR(1813);   /* ERROR_RESOURCE_TYPE_NOT_FOUND */
    WR_RET(ok ? VER_BLOCK : 0, 2);
}

WR_HANDLER(v_GetFileVersionInfoA)
{
    (void)base;
    const char *p = wr_str(WR_ARG(0));
    uint32_t len = WR_ARG(2), out = WR_ARG(3);
    uint32_t vsize = 0, vva;
    if (p && is_game_exe(p) && (vva = image_version(&vsize)) != 0) {
        if (len < vsize)
            WR_RET(0, 4);
        memcpy(wr_ptr(out), wr_ptr(vva), vsize);
        WR_RET(1, 4);
    }
    if (!p || !is_system_dll(p) || len < VER_BLOCK)
        WR_RET(0, 4);
    memset(wr_ptr(out), 0, VER_BLOCK);
    wr_wr32(out + 0, 0x52575652u);          /* our own marker: "RVWR" */
    uint32_t fi = out + 0x10;               /* VS_FIXEDFILEINFO */
    wr_wr32(fi + 0, 0xFEEF04BDu);
    wr_wr32(fi + 4, 0x00010000u);
    wr_wr32(fi + 8, 0x00040008u);           /* 4.08 */
    wr_wr32(fi + 12, 0x00010371u);          /* .01.0881 */
    wr_wr32(fi + 16, 0x00040008u);
    wr_wr32(fi + 20, 0x00010371u);
    wr_wr32(fi + 24, 0x3Fu);
    wr_wr32(fi + 32, 0x00040004u);          /* VOS_NT_WINDOWS32 */
    wr_wr32(fi + 36, 2);                    /* VFT_DLL */
    WR_RET(1, 4);
}

WR_HANDLER(v_VerQueryValueA)
{
    (void)base;
    uint32_t blk = WR_ARG(0), out = WR_ARG(2), plen = WR_ARG(3);
    const char *sub = wr_str(WR_ARG(1));
    if (sub && !strcmp(sub, "\\") && wr_rd32(blk) != 0x52575652u) {
        /* a real VS_VERSIONINFO: the fixed info follows "VS_VERSION_INFO" */
        uint16_t total = wr_rd16(blk);
        for (uint32_t o = 0; o + 52 <= total; o += 4)
            if (wr_rd32(blk + o) == 0xFEEF04BDu) {
                wr_wr32(out, blk + o);
                if (plen)
                    wr_wr32(plen, 52);
                WR_RET(1, 4);
            }
        WR_RET(0, 4);
    }
    if (wr_rd32(blk) != 0x52575652u || !sub || strcmp(sub, "\\")) {
        wr_log("VerQueryValueA(%s) -> not available", sub ? sub : "");
        WR_RET(0, 4);
    }
    wr_wr32(out, blk + 0x10);
    if (plen)
        wr_wr32(plen, 52);
    WR_RET(1, 4);
}

/* ---- WINMM ---------------------------------------------------------------------- */

WR_HANDLER(m_timeGetTime) { (void)base; WR_RET(wr_time_ms(), 0); }

/* ---- DPLAYX: no DirectPlay (network play comes with M9) ------------------------- */

WR_HANDLER(dp_DirectPlayCreate)
{
    (void)base;
    if (WR_ARG(1))
        wr_wr32(WR_ARG(1), 0);
    wr_log("DirectPlayCreate: not available");
    WR_RET(0x88770078u, 3);   /* DPERR_UNAVAILABLE */
}

WR_HANDLER(dp_DirectPlayLobbyCreateA)
{
    (void)base;
    if (WR_ARG(1))
        wr_wr32(WR_ARG(1), 0);
    wr_log("DirectPlayLobbyCreate: not available");
    WR_RET(0x88770078u, 5);
}

/* ---- tables -------------------------------------------------------------------------- */

WR_HANDLER(g_SetPixel) { (void)base; WR_RET(0, 4); }
WR_HANDLER(g_GetPixel) { (void)base; WR_RET(0, 3); }

static const wr_export g32_exports[] = { { "GetDeviceCaps", g_GetDeviceCaps }, { "SetPixel", g_SetPixel },
                                         { "GetPixel", g_GetPixel }, { NULL, NULL } };
static const wr_export a32_exports[] = {
    { "IsTextUnicode", a_IsTextUnicode }, { "RegDeleteValueA", a_RegDeleteValueA },
    { "RegOpenKeyExA", a_RegOpenKeyExA }, { "GetUserNameA", a_GetUserNameA },
    { "RegSetValueExA", a_RegSetValueExA }, { "RegCreateKeyExA", a_RegCreateKeyExA },
    { "RegQueryValueExA", a_RegQueryValueExA }, { "RegCloseKey", a_RegCloseKey }, { NULL, NULL } };
static const wr_export s32_exports[] = { { "ShellExecuteA", s_ShellExecuteA }, { NULL, NULL } };
static const wr_export o32_exports[] = {
    { "CoInitialize", o_CoInitialize }, { "CoUninitialize", o_CoUninitialize },
    { "CoCreateInstance", o_CoCreateInstance }, { NULL, NULL } };
static const wr_export v_exports[] = {
    { "GetFileVersionInfoA", v_GetFileVersionInfoA }, { "VerQueryValueA", v_VerQueryValueA },
    { "GetFileVersionInfoSizeA", v_GetFileVersionInfoSizeA }, { NULL, NULL } };
static const wr_export mm_exports[] = { { "timeGetTime", m_timeGetTime }, { NULL, NULL } };
/* dplayx ordinals: 1 DirectPlayCreate (3 args), 4/5 DirectPlayLobbyCreateA/W (5 args) */
static const wr_export dp_exports[] = {
    { "#1", dp_DirectPlayCreate }, { "DirectPlayCreate", dp_DirectPlayCreate },
    { "#4", dp_DirectPlayLobbyCreateA }, { "#5", dp_DirectPlayLobbyCreateA },
    { "DirectPlayLobbyCreateA", dp_DirectPlayLobbyCreateA }, { NULL, NULL } };

/* DirectX system DLLs the game only loads to check they exist (version check):
 * present as modules without exports; GetProcAddress logs what is asked for. */
static const wr_export no_exports[] = { { NULL, NULL } };
static wr_module ddraw_mod = { "DDRAW.DLL", no_exports, 0 };
static wr_module dinput_mod = { "DINPUT.DLL", no_exports, 0 };
static wr_module dmusic_mod = { "DMUSIC.DLL", no_exports, 0 };
static wr_module dsound_mod = { "DSOUND.DLL", no_exports, 0 };

static wr_module g32_mod = { "GDI32.dll", g32_exports, 0 };
static wr_module a32_mod = { "ADVAPI32.dll", a32_exports, 0 };
static wr_module s32_mod = { "SHELL32.dll", s32_exports, 0 };
static wr_module o32_mod = { "ole32.dll", o32_exports, 0 };
static wr_module v_mod = { "VERSION.dll", v_exports, 0 };
static wr_module mm_mod = { "WINMM.dll", mm_exports, 0 };
static wr_module dp_mod = { "DPLAYX.dll", dp_exports, 0 };

void w32_register_kernel32(void);
void w32_register_user32(void);
void w32_register_dinput8(void);

void wr_win32_register(void)
{
    w32_register_kernel32();
    w32_register_user32();
    w32_register_dinput8();
    wr_module_register(&g32_mod);
    wr_module_register(&a32_mod);
    wr_module_register(&s32_mod);
    wr_module_register(&o32_mod);
    wr_module_register(&v_mod);
    wr_module_register(&mm_mod);
    wr_module_register(&dp_mod);
    wr_module_register(&ddraw_mod);
    wr_module_register(&dinput_mod);
    wr_module_register(&dmusic_mod);
    wr_module_register(&dsound_mod);
}

void w32_exit(uint32_t code)
{
    w32_registry_save();
    wr_platform_quit();
    wr_exit(code);
}
