/* Randomised heap test against a model: alloc/free/realloc keep contents,
 * blocks never overlap, the walk stays consistent. */
#include "wr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void wr_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }
void wr_fatal(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); abort(); }
uint32_t g_wr_last_error;

#define N 4000
static uint32_t va[N], sz[N];
static uint8_t tag[N];

static void fill(int i) { memset(wr_ptr(va[i]), tag[i], sz[i]); }
static int check(int i) {
    const uint8_t *p = wr_ptr(va[i]);
    for (uint32_t k = 0; k < sz[i]; k++) if (p[k] != tag[i]) return 0;
    return 1;
}

int main(void)
{
    if (!wr_mem_init()) return 1;
    srand(12345);
    long ops = 0;
    for (int round = 0; round < 400000; round++) {
        int i = rand() % N;
        int r = rand() % 10;
        uint32_t s = (rand() % 4 == 0) ? (uint32_t)(rand() % 70000) : (uint32_t)(rand() % 300);
        if (!va[i]) {
            va[i] = wr_heap_alloc(s, r < 3);
            if (!va[i]) { printf("alloc failed\n"); return 1; }
            if (va[i] & 15) { printf("misaligned\n"); return 1; }
            if (r < 3) { const uint8_t *p = wr_ptr(va[i]); for (uint32_t k = 0; k < s; k++) if (p[k]) { printf("not zeroed\n"); return 1; } }
            sz[i] = s; tag[i] = (uint8_t)(rand() | 1); fill(i);
            if (wr_heap_size(va[i]) != s) { printf("size mismatch\n"); return 1; }
        } else if (r < 6) {
            if (!check(i)) { printf("corrupt before free %d\n", i); return 1; }
            wr_heap_free(va[i]); va[i] = 0;
        } else {
            if (!check(i)) { printf("corrupt before realloc\n"); return 1; }
            uint32_t n = wr_heap_realloc(va[i], s, 0, 0);
            if (!n) { printf("realloc failed\n"); return 1; }
            uint32_t keep = s < sz[i] ? s : sz[i];
            const uint8_t *p = wr_ptr(n);
            for (uint32_t k = 0; k < keep; k++) if (p[k] != tag[i]) { printf("realloc lost data\n"); return 1; }
            va[i] = n; sz[i] = s; fill(i);
        }
        ops++;
        if (round % 500 == 0 && !wr_heap_check()) { printf("heap check failed at %d\n", round); return 1; }
    }
    for (int i = 0; i < N; i++) if (va[i] && !check(i)) { printf("final corrupt\n"); return 1; }
    for (int i = 0; i < N; i++) if (va[i]) wr_heap_free(va[i]);
    if (!wr_heap_check()) { printf("final heap check failed\n"); return 1; }
    printf("heap: %ld ops ok\n", ops);
    return 0;
}
