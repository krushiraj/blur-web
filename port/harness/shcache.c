// Harness side of the offline shader cache: only the key set is needed (the browser fetches the same file for the GLSL).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shcache.h"

typedef struct { uint64_t h; uint32_t len; } Key;
static Key *keys; static int nkeys;

static int kcmp(const void *a, const void *b) {
    const Key *x = a, *y = b;
    return x->h < y->h ? -1 : x->h > y->h ? 1 : x->len < y->len ? -1 : x->len > y->len;
}

int shc_load(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n); if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return -2; } fclose(f);
    if (n < 8 || memcmp(b, "BSC1", 4)) { free(b); return -3; }
    uint32_t cnt; memcpy(&cnt, b + 4, 4); keys = calloc(cnt ? cnt : 1, sizeof *keys); size_t p = 8;
    for (uint32_t i = 0; i < cnt; i++) {
        if (p + 20 > (size_t)n) { free(b); return -4; }
        uint32_t lo, hi, len, gl; memcpy(&lo, b + p, 4); memcpy(&hi, b + p + 4, 4); memcpy(&len, b + p + 8, 4); memcpy(&gl, b + p + 16, 4);
        keys[nkeys].h = ((uint64_t)hi << 32) | lo; keys[nkeys++].len = len;
        p += 20 + 4u * (b[p + 12 + 1] + b[p + 12 + 2] + b[p + 12 + 3]) + gl; p = (p + 3) & ~(size_t)3;
    }
    free(b); qsort(keys, nkeys, sizeof *keys, kcmp); return nkeys;
}

int shc_has(uint64_t hash, uint32_t len) {
    Key k = {hash, len};
    return nkeys && bsearch(&k, keys, nkeys, sizeof *keys, kcmp) != NULL;
}
