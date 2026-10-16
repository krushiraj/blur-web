// Offline shader translator: D3D9 bytecode files -> "BSC1" cache (see shcache.h).  Uses the same shader_translate() as the live path,
// so cache and live output are identical by construction.   usage: shadertool -o out.bin [-q] <dir-or-file.bin>...
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include "emu.h"
#include "shcache.h"

typedef struct { uint64_t h; uint32_t len; uint8_t *code; } In;
static In *ins; static int nins, cins;
static int quiet;

static void add_file(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 8 || n > (1 << 20)) { fclose(f); return; }
    uint8_t *b = malloc(n); if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return; } fclose(f);
    uint32_t v = *(uint32_t *)b; if ((v >> 16) != 0xfffe && (v >> 16) != 0xffff) { free(b); return; }
    uint64_t h = shc_fnv64(b, n);
    for (int i = 0; i < nins; i++) if (ins[i].h == h && ins[i].len == (uint32_t)n) { free(b); return; }
    if (nins == cins) { cins = cins ? cins * 2 : 1024; ins = realloc(ins, cins * sizeof *ins); }
    ins[nins++] = (In){h, (uint32_t)n, b};
}
static void add_path(const char *p) {
    struct stat st; if (stat(p, &st)) return;
    if (!S_ISDIR(st.st_mode)) { add_file(p); return; }
    DIR *d = opendir(p); struct dirent *e; if (!d) return;
    while ((e = readdir(d))) { size_t l = strlen(e->d_name); if (l > 4 && !strcmp(e->d_name + l - 4, ".bin")) { char fn[1024]; snprintf(fn, sizeof fn, "%s/%s", p, e->d_name); add_file(fn); } }
    closedir(d);
}
static int cmp(const void *a, const void *b) { const In *x = a, *y = b; return x->h < y->h ? -1 : x->h > y->h ? 1 : x->len < y->len ? -1 : x->len > y->len; }

int main(int argc, char **argv) {
    const char *out = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else add_path(argv[i]);
    }
    if (!out || !nins) { fprintf(stderr, "usage: shadertool -o out.bin [-q] <dir-or-file.bin>...\n"); return 1; }
    qsort(ins, nins, sizeof *ins, cmp);
    FILE *f = fopen(out, "wb"); if (!f) { perror(out); return 1; }
    uint32_t cnt = 0; fwrite("BSC1\0\0\0\0", 1, 8, f);
    int fail = 0, nvs = 0, nps = 0; size_t glsl_total = 0; clock_t t0 = clock();
    for (int i = 0; i < nins; i++) {
        ShaderXlat x;
        if (shader_translate(ins[i].code, ins[i].len, &x)) { fail++; if (!quiet) fprintf(stderr, "FAIL %016llx_%u\n", (unsigned long long)ins[i].h, ins[i].len); continue; }
        uint32_t hd[5] = {(uint32_t)ins[i].h, (uint32_t)(ins[i].h >> 32), ins[i].len, 0, x.glsl_len};
        hd[3] = (uint32_t)x.is_ps | (uint32_t)x.nin << 8 | (uint32_t)x.nout << 16 | (uint32_t)x.nsamp << 24;
        fwrite(hd, 4, 5, f); fwrite(x.in, 4, x.nin, f); fwrite(x.out, 4, x.nout, f); fwrite(x.samp, 4, x.nsamp, f); fwrite(x.glsl, 1, x.glsl_len, f);
        size_t sz = 20 + 4u * (x.nin + x.nout + x.nsamp) + x.glsl_len; for (; sz & 3; sz++) fputc(0, f);
        glsl_total += x.glsl_len; cnt++; if (x.is_ps) nps++; else nvs++; free(x.glsl);
    }
    fseek(f, 4, SEEK_SET); fwrite(&cnt, 4, 1, f); fclose(f);
    fprintf(stderr, "shadertool: %d inputs, %u translated (vs %d, ps %d), %d failed, glsl %zu bytes, %.1f s -> %s\n", nins, cnt, nvs, nps, fail, glsl_total, (double)(clock() - t0) / CLOCKS_PER_SEC, out);
    return fail ? 2 : 0;
}
