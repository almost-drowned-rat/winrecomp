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
    mkdirs(g_wr_paths.saves);
    mkdirs(g_wr_paths.data);
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

int wr_path_resolve_read(const char *guest, char *host, size_t cap)
{
    char rel[1024];
    if (!wr_path_relative(guest, rel, sizeof rel))
        return 0;
    if (wr_path_ci(g_wr_paths.saves, rel, host, cap))
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

/* <root>/screenshots/<game>_YYYY-MM-DD_HH-MM-SS[_n].png (created on demand);
 * WR_SCREENSHOT_DIR overrides the folder. 1 on success. */
int wr_screenshot_path(const char *game, char *out, size_t cap)
{
    char dir[1100];
    const char *e = getenv("WR_SCREENSHOT_DIR");
    if (e && *e)
        snprintf(dir, sizeof dir, "%s", e);
    else
        snprintf(dir, sizeof dir, "%s/screenshots", g_wr_paths.root);
#if defined(_WIN32)
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    time_t t = time(NULL);
    struct tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &tm);
    for (int n = 0; n < 100; n++) {
        if (n)
            snprintf(out, cap, "%s/%s_%s_%d.png", dir, game, stamp, n);
        else
            snprintf(out, cap, "%s/%s_%s.png", dir, game, stamp);
        FILE *f = fopen(out, "rb");
        if (!f)
            return 1;
        fclose(f);
    }
    return 0;
}
