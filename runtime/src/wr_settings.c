/*
 * winrecomp runtime - the port's settings file, saves/settings.ini.
 *
 * One "key=value" per line (UTF-8). Comments (# or ;), blank lines and keys
 * this build does not know are kept as they are. Every write replaces the
 * whole file atomically (temp file, flush to disk, rename), so a crash
 * leaves the old file. <PREFIX>_<KEY> in the environment (key upper-cased)
 * overrides a value for one run and is never written back.
 */
#include "wr.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#define MAX_LINES 4096   /* per-preset shader parameters add lines */
#define LINE_MAX_LEN 512

static char g_path[1100];
static char g_prefix[32];
static char *g_line[MAX_LINES];   /* raw lines, without newline */
static int g_n;

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
}

/* index of the line holding key, or -1 */
static int find(const char *key)
{
    size_t kl = strlen(key);
    for (int i = 0; i < g_n; i++) {
        const char *l = g_line[i];
        while (*l == ' ' || *l == '\t')
            l++;
        if (!strncmp(l, key, kl)) {
            const char *p = l + kl;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == '=')
                return i;
        }
    }
    return -1;
}

void wr_settings_init(const char *env_prefix)
{
    snprintf(g_prefix, sizeof g_prefix, "%s", env_prefix ? env_prefix : "WR");
    snprintf(g_path, sizeof g_path, "%s/settings.ini", g_wr_paths.saves);
    FILE *f = fopen(g_path, "rb");
    if (!f)
        return;
    char buf[LINE_MAX_LEN];
    while (g_n < MAX_LINES && fgets(buf, sizeof buf, f)) {
        trim(buf);
        g_line[g_n++] = strdup(buf);
    }
    fclose(f);
    wr_log("settings: %s (%d lines)", g_path, g_n);
}

const char *wr_settings_get(const char *key, const char *def)
{
    char env[96];
    int n = snprintf(env, sizeof env, "%s_", g_prefix);
    for (const char *k = key; *k && n < (int)sizeof env - 1; k++)
        env[n++] = (char)toupper((unsigned char)*k);
    env[n] = 0;
    const char *e = getenv(env);
    if (e && *e)
        return e;
    int i = find(key);
    if (i < 0)
        return def;
    const char *v = strchr(g_line[i], '=') + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    return *v ? v : def;
}

int wr_settings_get_int(const char *key, int def)
{
    const char *v = wr_settings_get(key, NULL);
    if (!v)
        return def;
    char *end;
    long x = strtol(v, &end, 10);
    return end != v ? (int)x : def;
}

static void save(void)
{
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        wr_log("settings: cannot write %s", tmp);
        return;
    }
    for (int i = 0; i < g_n; i++) {
        fputs(g_line[i], f);
        fputc('\n', f);
    }
    fflush(f);
#if defined(_WIN32)
    _commit(_fileno(f));
    fclose(f);
    if (!MoveFileExA(tmp, g_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        wr_log("settings: cannot replace %s", g_path);
#else
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, g_path) != 0)
        wr_log("settings: cannot replace %s", g_path);
#endif
}

void wr_settings_set(const char *key, const char *value)
{
    if (!g_path[0] || !key || !*key)
        return;
    char line[LINE_MAX_LEN];
    snprintf(line, sizeof line, "%s=%s", key, value ? value : "");
    int i = find(key);
    if (i >= 0) {
        if (!strcmp(g_line[i], line))
            return;
        free(g_line[i]);
        g_line[i] = strdup(line);
    } else {
        if (g_n >= MAX_LINES)
            return;
        g_line[g_n++] = strdup(line);
    }
    save();
}

void wr_settings_set_int(const char *key, int value)
{
    char v[16];
    snprintf(v, sizeof v, "%d", value);
    wr_settings_set(key, v);
}

/* The key of line i (lines without "key=" are skipped), NULL past the end:
 * to walk the file (per-preset values and the like). */
const char *wr_settings_key_at(int i)
{
    static char key[LINE_MAX_LEN];
    int n = 0;
    for (int l = 0; l < g_n; l++) {
        const char *t = g_line[l];
        while (*t == ' ' || *t == '\t')
            t++;
        const char *eq = strchr(t, '=');
        if (!eq || *t == '#' || *t == ';' || eq == t)
            continue;
        if (n++ == i) {
            size_t k = (size_t)(eq - t);
            while (k && (t[k - 1] == ' ' || t[k - 1] == '\t'))
                k--;
            if (k >= sizeof key)
                k = sizeof key - 1;
            memcpy(key, t, k);
            key[k] = 0;
            return key;
        }
    }
    return NULL;
}
