/*
 * winrecomp runtime - portable layout and guest path mapping.
 *
 *   <root>/game/   the game's own files (read only)
 *   <root>/saves/  everything the player would miss; written files go here
 *   <root>/data/   logs and caches, safe to delete
 *   <root>/media/  the player's media, read only
 *
 * A guest path is made relative to the virtual game folder (g_wr_vroot, e.g.
 * "C:\GTA2"): absolute paths below it, drive-relative and relative paths via
 * the virtual current directory. Reads look in saves/ first, then game/;
 * writes go to saves/. Lookup is case-insensitive on every host.
 */
#include "wr.h"
#if defined(_WIN32)
#include <direct.h>
#endif
#include <stdlib.h>
#include <time.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#if defined(_WIN32)
#  include <direct.h>
#  define mkdir_(p) _mkdir(p)
#else
#  include <dirent.h>
#  define mkdir_(p) mkdir(p, 0755)
#endif

wr_paths g_wr_paths;
char g_wr_vroot[128] = "C:\\GAME";
char g_wr_exe_name[64] = "game.exe";
char g_wr_cwd[512];

static void mkdirs(const char *path)
{
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir_(tmp);
            *p = '/';
        }
    mkdir_(tmp);
}

void wr_paths_init(const char *root)
{
    snprintf(g_wr_paths.root, sizeof g_wr_paths.root, "%s", root);
    snprintf(g_wr_paths.game, sizeof g_wr_paths.game, "%s/game", root);
    snprintf(g_wr_paths.saves, sizeof g_wr_paths.saves, "%s/saves", root);
    snprintf(g_wr_paths.data, sizeof g_wr_paths.data, "%s/data", root);
    snprintf(g_wr_paths.media, sizeof g_wr_paths.media, "%s/media", root);
    /* the whole layout exists from the first start, so the player sees where his files go */
    mkdirs(g_wr_paths.game);
    mkdirs(g_wr_paths.saves);
    mkdirs(g_wr_paths.data);
    mkdirs(g_wr_paths.media);
#if !defined(_WIN32) && !defined(__ANDROID__)
    /* Nothing outside the folder (as option-c): the GPU drivers' shader
     * caches go to data/shadercache (Mesa incl. RADV's built-in shaders,
     * NVIDIA). A value the player set himself wins. */
    {
        char cache[1100];
        snprintf(cache, sizeof cache, "%s/shadercache", g_wr_paths.data);
        mkdirs(cache);
        setenv("MESA_SHADER_CACHE_DIR", cache, 0);
        setenv("__GL_SHADER_DISK_CACHE_PATH", cache, 0);
        setenv("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", "1", 0);
    }
#endif
    snprintf(g_wr_cwd, sizeof g_wr_cwd, "%s", g_wr_vroot);
}

/* Join and normalise to forward slashes, resolving "." and "..". Works on a
 * Windows-style absolute path "X:\a\b". */
static void normalise(const char *in, char *out, size_t cap)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", in);
    for (char *p = buf; *p; p++)
        if (*p == '\\')
            *p = '/';
    /* split into components */
    const char *comp[128];
    int n = 0;
    char *save = NULL;
    char *head = buf;
    char drive[3] = { 0 };
    if (head[0] && head[1] == ':') {
        drive[0] = head[0];
        drive[1] = ':';
        head += 2;
    }
    for (char *t = strtok_r(head, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, "."))
            continue;
        if (!strcmp(t, "..")) {
            if (n)
                n--;
            continue;
        }
        if (n < 128)
            comp[n++] = t;
    }
    size_t o = (size_t)snprintf(out, cap, "%s", drive);
    for (int i = 0; i < n && o < cap; i++)
        o += (size_t)snprintf(out + o, cap - o, "/%s", comp[i]);
    if (!n && o < cap)
        snprintf(out + o, cap - o, "/");
}

int wr_path_relative(const char *guest, char *rel, size_t cap)
{
    char full[1100], vroot[256];
    if (!guest)
        return 0;
    /* absolute (X:\..), rooted (\..) or relative to the cwd */
    if (guest[0] && guest[1] == ':' && (guest[2] == '\\' || guest[2] == '/'))
        snprintf(full, sizeof full, "%s", guest);
    else if (guest[0] == '\\' || guest[0] == '/')
        snprintf(full, sizeof full, "%.2s%s", g_wr_cwd, guest);
    else if (guest[0] && guest[1] == ':')
        snprintf(full, sizeof full, "%s\\%s", g_wr_cwd, guest + 2);   /* "C:file" */
    else
        snprintf(full, sizeof full, "%s\\%s", g_wr_cwd, guest);
    char nfull[1100];
    normalise(full, nfull, sizeof nfull);
    normalise(g_wr_vroot, vroot, sizeof vroot);
    size_t lr = strlen(vroot);
    if (strncasecmp(nfull, vroot, lr) || (nfull[lr] && nfull[lr] != '/'))
        return 0;
    const char *r = nfull + lr;
    while (*r == '/')
        r++;
    snprintf(rel, cap, "%s", r);
    return 1;
}

/* Find rel below dir, matching each component case-insensitively. */
int wr_path_ci(const char *dir, const char *rel, char *host, size_t cap)
{
    char cur[1200];
    snprintf(cur, sizeof cur, "%s", dir);
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", rel);
    char *save = NULL;
    for (char *t = strtok_r(buf, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        char probe[1400];
        struct stat st;
        snprintf(probe, sizeof probe, "%s/%s", cur, t);
        if (stat(probe, &st) == 0) {
            snprintf(cur, sizeof cur, "%s", probe);
            continue;
        }
#if defined(_WIN32)
        return 0;   /* the host is case-insensitive already */
#else
        DIR *d = opendir(cur);
        if (!d)
            return 0;
        struct dirent *e;
        int found = 0;
        while ((e = readdir(d))) {
            if (!strcasecmp(e->d_name, t)) {
                snprintf(probe, sizeof probe, "%s/%s", cur, e->d_name);
                found = 1;
                break;
            }
        }
        closedir(d);
        if (!found)
            return 0;
        snprintf(cur, sizeof cur, "%s", probe);
#endif
    }
    snprintf(host, cap, "%s", cur);
    return 1;
}

/* Folders read between saves/ and game/ (the port's own files that stand in
 * for or add to the game's, e.g. translations unpacked into data/, and the
 * player's mods). Searched in the order added. */
#define MAX_READ_DIRS 36
#define PORT_READ_DIRS 4   /* kept free of mods for the port's own folders (e.g. the translations) */
static char g_read_dirs[MAX_READ_DIRS][1024];
static int g_nread_dirs;

void wr_path_add_read_dir(const char *dir)
{
    if (!dir || !*dir)
        return;
    if (g_nread_dirs >= MAX_READ_DIRS) {
        wr_log("paths: read folder %s left out (at most %d)", dir, MAX_READ_DIRS);
        return;
    }
    snprintf(g_read_dirs[g_nread_dirs++], sizeof g_read_dirs[0], "%s", dir);
}

static int cmp_ci(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

/* Mods: every folder below dir is one mod, a tree like the game's own folder
 * (e.g. mods/MyMod/data/wil.sty stands in for game/data/wil.sty). Added in
 * name order, so the first by name wins where two mods hold the same file
 * ("01 ...", "02 ..." set the order). A folder whose name starts with "_" or
 * "." is skipped (switched off), "textures" is the texture packs' folder.
 * Returns the number of mods added. */
#include <dirent.h>
int wr_path_add_mod_dirs(const char *dir)
{
    if (!dir || !*dir)
        return 0;
    mkdirs(dir);   /* there from the first start, so the player sees where mods go */
    {   /* ... and the texture packs' folder in it (maintainer 2026-11-27) */
        char t[1200];
        snprintf(t, sizeof t, "%s/textures", dir);
        mkdirs(t);
    }
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    static char names[256][256];
    int n = 0, all = 0;   /* names kept / mod folders seen */
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || e->d_name[0] == '_' || !strcasecmp(e->d_name, "textures"))
            continue;   /* switched off, or the texture packs' folder */
        char p[1400];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        all++;
        if (n < (int)(sizeof names / sizeof names[0]))
            snprintf(names[n++], sizeof names[0], "%s", e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], cmp_ci);
    int added = 0;
    /* mods never take the last PORT_READ_DIRS slots: the port adds its own folders after them */
    for (int i = 0; i < n && g_nread_dirs < MAX_READ_DIRS - PORT_READ_DIRS; i++) {
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        wr_path_add_read_dir(p);
        wr_log("mods: %s", names[i]);
        added++;
    }
    if (all > added)
        wr_log("mods: %d more left out (at most %d mod folders are read)", all - added,
               MAX_READ_DIRS - PORT_READ_DIRS);
    return added;
}

/* the port may read another file in place of one (tests, e.g. GTA 2's radio station check) */
const char *(*g_wr_read_map)(const char *rel);

int wr_path_resolve_read(const char *guest, char *host, size_t cap)
{
    char rel[1024];
    if (!wr_path_relative(guest, rel, sizeof rel))
        return 0;
    if (g_wr_read_map) {
        const char *m = g_wr_read_map(rel);
        if (m)
            snprintf(rel, sizeof rel, "%s", m);
    }
    if (wr_path_ci(g_wr_paths.saves, rel, host, cap))
        return 1;
    for (int i = 0; i < g_nread_dirs; i++)
        if (wr_path_ci(g_read_dirs[i], rel, host, cap))
            return 1;
    return wr_path_ci(g_wr_paths.game, rel, host, cap);
}

int wr_path_resolve_write(const char *guest, char *host, size_t cap)
{
    char rel[1024];
    if (!wr_path_relative(guest, rel, sizeof rel))
        return 0;
    /* Reuse the existing spelling of directories in saves/ where there is
     * one; create the rest as the guest spells them. */
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", rel);
    char *slash = strrchr(dir, '/');
    const char *file = rel;
    char hostdir[1200];
    snprintf(hostdir, sizeof hostdir, "%s", g_wr_paths.saves);
    if (slash) {
        *slash = 0;
        file = rel + (slash - dir) + 1;
        if (!wr_path_ci(g_wr_paths.saves, dir, hostdir, sizeof hostdir)) {
            snprintf(hostdir, sizeof hostdir, "%s/%s", g_wr_paths.saves, dir);
            mkdirs(hostdir);
        }
    }
    /* An existing file of another case is overwritten, not duplicated. */
    char existing[1400];
    if (wr_path_ci(hostdir, file, existing, sizeof existing)) {
        snprintf(host, cap, "%s", existing);
        return 1;
    }
    snprintf(host, cap, "%s/%s", hostdir, file);
    return 1;
}

/* F12 capture folder, as option-p: data/captures/<YYYYMMDD-HHMMSS>-frame<N>/
 * (created; WR_CAPTURE_DIR overrides data/captures). 1 on success. */
int wr_capture_dir(uint64_t frame, char *out, size_t cap)
{
    char base[1100];
    const char *e = getenv("WR_CAPTURE_DIR");
    if (e && *e)
        snprintf(base, sizeof base, "%s", e);
    else
        snprintf(base, sizeof base, "%s/captures", g_wr_paths.data);
    time_t t = time(NULL);
    struct tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &t);
    _mkdir(base);
#else
    localtime_r(&t, &tm);
    mkdir(base, 0755);
#endif
    snprintf(out, cap, "%s/%04d%02d%02d-%02d%02d%02d-frame%llu", base, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, (unsigned long long)frame);
#if defined(_WIN32)
    return _mkdir(out) == 0 || errno == EEXIST;
#else
    return mkdir(out, 0755) == 0 || errno == EEXIST;
#endif
}
