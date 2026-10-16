// d3dx9_42.dll replacements: matrix/plane math is implemented for real (row-vector convention, as D3DX does).
// Texture/shader helpers are added as the game reaches them.
#include "emu.h"
#include <math.h>
#ifndef __EMSCRIPTEN__
#include <vkd3d_shader.h>
#endif

typedef struct { float m[4][4]; } Mat;
static float f_of(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static void get_mat(Ctx *c, uint32_t p, Mat *m) { rdbuf(c, p, m, 64); }
static void put_mat(Ctx *c, uint32_t p, const Mat *m) { wrbuf(c, p, m, 64); }

static uint32_t x_multiply(Ctx *c, uint32_t *a) {
    Mat A, B, R; get_mat(c, a[1], &A); get_mat(c, a[2], &B);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += A.m[i][k] * B.m[k][j]; R.m[i][j] = s; }
    put_mat(c, a[0], &R); return a[0];
}
static uint32_t x_transpose(Ctx *c, uint32_t *a) { Mat A, R; get_mat(c, a[1], &A); for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) R.m[i][j] = A.m[j][i]; put_mat(c, a[0], &R); return a[0]; }
static uint32_t x_inverse(Ctx *c, uint32_t *a) {                  // Gauss-Jordan with partial pivoting; returns 0 when singular
    Mat A; get_mat(c, a[2], &A); double m[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { m[i][j] = A.m[i][j]; m[i][4 + j] = i == j; }
    double det = 1;
    for (int col = 0; col < 4; col++) {
        int p = col; for (int r = col + 1; r < 4; r++) if (fabs(m[r][col]) > fabs(m[p][col])) p = r;
        if (fabs(m[p][col]) < 1e-30) { if (a[1]) wr32(c, a[1], 0); return 0; }
        if (p != col) { for (int j = 0; j < 8; j++) { double t = m[p][j]; m[p][j] = m[col][j]; m[col][j] = t; } det = -det; }
        det *= m[col][col]; double inv = 1.0 / m[col][col]; for (int j = 0; j < 8; j++) m[col][j] *= inv;
        for (int r = 0; r < 4; r++) if (r != col) { double f = m[r][col]; for (int j = 0; j < 8; j++) m[r][j] -= f * m[col][j]; }
    }
    Mat R; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) R.m[i][j] = (float)m[i][4 + j];
    put_mat(c, a[0], &R); if (a[1]) { float d = (float)det; uint32_t u; memcpy(&u, &d, 4); wr32(c, a[1], u); } return a[0];
}
static void vnorm(float *v) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static uint32_t x_lookat(Ctx *c, uint32_t *a) {
    float eye[3], at[3], up[3]; rdbuf(c, a[1], eye, 12); rdbuf(c, a[2], at, 12); rdbuf(c, a[3], up, 12);
    float z[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]}; vnorm(z);
    float x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]}; vnorm(x);
    float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    Mat R = {{{x[0], y[0], z[0], 0}, {x[1], y[1], z[1], 0}, {x[2], y[2], z[2], 0},
              {-(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]), -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]), -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]), 1}}};
    put_mat(c, a[0], &R); return a[0];
}
static uint32_t x_ortho(Ctx *c, uint32_t *a) {
    float w = f_of(a[1]), h = f_of(a[2]), zn = f_of(a[3]), zf = f_of(a[4]);
    Mat R = {{{2 / w, 0, 0, 0}, {0, 2 / h, 0, 0}, {0, 0, 1 / (zf - zn), 0}, {0, 0, zn / (zn - zf), 1}}}; put_mat(c, a[0], &R); return a[0];
}
static uint32_t x_plane_normalize(Ctx *c, uint32_t *a) {
    float p[4]; rdbuf(c, a[1], p, 16); float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (l > 0) { for (int i = 0; i < 4; i++) p[i] /= l; } else p[0] = p[1] = p[2] = p[3] = 0;
    wrbuf(c, a[0], p, 16); return a[0];
}
static uint32_t x_plane_transform(Ctx *c, uint32_t *a) {
    float p[4], r[4]; Mat M; rdbuf(c, a[1], p, 16); get_mat(c, a[2], &M);
    for (int j = 0; j < 4; j++) r[j] = p[0] * M.m[0][j] + p[1] * M.m[1][j] + p[2] * M.m[2][j] + p[3] * M.m[3][j];
    wrbuf(c, a[0], r, 16); return a[0];
}
static uint32_t s_vs, s_ps;
static uint32_t x_vsprofile(Ctx *c, uint32_t *a) { (void)a; if (!s_vs) { s_vs = guest_alloc(c, 8); wrbuf(c, s_vs, "vs_3_0", 7); } return s_vs; }
static uint32_t x_psprofile(Ctx *c, uint32_t *a) { (void)a; if (!s_ps) { s_ps = guest_alloc(c, 8); wrbuf(c, s_ps, "ps_3_0", 7); } return s_ps; }
static uint32_t x_ok(Ctx *c, uint32_t *a) { (void)c; (void)a; return 0; }

// ---- ID3DXConstantTable: parsed from the CTAB comment embedded in the shader bytecode ------------------------------------------------
typedef struct { char name[64]; uint16_t set, reg, count, cls, type, rows, cols, elems; uint32_t bytes, name_g, def_g; } Const;
typedef struct { int used, ps; uint32_t version, creator_g, buf_g, buf_len; int n; Const k[64]; } CTable;
#define MAXTABS 4096
static CTable tabs[MAXTABS]; static int ntabs = 1;
static CTable *tab_of(Ctx *c, uint32_t obj) { uint32_t i = rd32(c, obj + 8); return i && i < (uint32_t)ntabs ? &tabs[i] : &tabs[0]; }

static int parse_ctab(Ctx *c, uint32_t code, CTable *t) {
    uint32_t ver = rd32(c, code); t->version = ver; t->ps = (ver >> 16) == 0xffff;
    for (uint32_t p = code + 4, n = 0; n < 8192; n++) {
        uint32_t tok = rd32(c, p); if (tok == 0x0000ffff) break;
        if ((tok & 0xffff) == 0xfffe) {
            uint32_t len = (tok >> 16) & 0x7fff;
            if (rd32(c, p + 4) == 0x42415443) {                                  // 'CTAB'
                uint32_t base = p + 8, cnt = rd32(c, base + 12), info = base + rd32(c, base + 16);
                char creator[64]; rd_cstr(c, base + rd32(c, base + 4), creator, sizeof creator);
                t->creator_g = guest_alloc(c, 64); wrbuf(c, t->creator_g, creator, strlen(creator) + 1);
                t->buf_len = len * 4 - 4; t->buf_g = base;
                for (uint32_t i = 0; i < cnt && i < 64; i++) {
                    uint32_t e = info + i * 20; Const *k = &t->k[t->n++];
                    rd_cstr(c, base + rd32(c, e), k->name, sizeof k->name);
                    k->name_g = guest_alloc(c, 64); wrbuf(c, k->name_g, k->name, strlen(k->name) + 1);
                    k->set = rd16(c, e + 4); k->reg = rd16(c, e + 6); k->count = rd16(c, e + 8);
                    uint32_t ti = base + rd32(c, e + 12); k->cls = rd16(c, ti); k->type = rd16(c, ti + 2); k->rows = rd16(c, ti + 4); k->cols = rd16(c, ti + 6); k->elems = rd16(c, ti + 8);
                    k->bytes = (uint32_t)k->rows * k->cols * 4 * (k->elems ? k->elems : 1); { uint32_t dv = rd32(c, e + 16); k->def_g = dv ? base + dv : 0; }
                }
                return 1;
            }
            p += 4 + len * 4; continue;
        }
        if ((tok & 0x0ffff) == 0xfffd) { p += 4; continue; }                      // PHASE: no operands
        p += 4;                                                                    // instruction tokens: skip until END/comment (length in bits 24..27)
        uint32_t ops = (tok >> 24) & 0xf; p += ops * 4;
    }
    return 0;
}
static Const *find_const(Ctx *c, CTable *t, uint32_t h, uint32_t *elem) {
    *elem = 0;
    if (h >= 0x10000) { char n[64]; rd_cstr(c, h, n, sizeof n); for (int i = 0; i < t->n; i++) if (!strcmp(t->k[i].name, n)) return &t->k[i]; return NULL; }
    uint32_t i = (h & 0xffff); if (!i || i > (uint32_t)t->n) return NULL;
    if (h >> 16) *elem = (h >> 16) - 1; return &t->k[i - 1];
}
static uint32_t ct_getdesc(Ctx *c, uint32_t *a) { CTable *t = tab_of(c, a[0]); wr32(c, a[1], t->creator_g); wr32(c, a[1] + 4, t->version); wr32(c, a[1] + 8, (uint32_t)t->n); return 0; }
static uint32_t ct_getconstant(Ctx *c, uint32_t *a) { CTable *t = tab_of(c, a[0]); return a[2] < (uint32_t)t->n ? a[2] + 1 : 0; }
static uint32_t ct_byname(Ctx *c, uint32_t *a) { CTable *t = tab_of(c, a[0]); char n[64]; rd_cstr(c, a[2], n, sizeof n); for (int i = 0; i < t->n; i++) if (!strcmp(t->k[i].name, n)) return (uint32_t)i + 1; return 0; }
static uint32_t ct_element(Ctx *c, uint32_t *a) { CTable *t = tab_of(c, a[0]); uint32_t e; Const *k = find_const(c, t, a[1], &e); if (!k || a[2] >= (k->elems ? k->elems : 1)) return 0; return (a[1] & 0xffff) | ((a[2] + 1) << 16); }
static uint32_t ct_constdesc(Ctx *c, uint32_t *a) {
    CTable *t = tab_of(c, a[0]); uint32_t e; Const *k = find_const(c, t, a[1], &e); if (!k) return 0x8876086c;
    uint32_t d[12] = {k->name_g, k->set, k->reg, k->count, k->cls, k->type, k->rows, k->cols, k->elems ? k->elems : 1, 0, k->bytes, k->def_g};
    wrbuf(c, a[2], d, sizeof d); if (a[3]) wr32(c, a[3], 1); return 0;
}
static uint32_t ct_sampler(Ctx *c, uint32_t *a) { CTable *t = tab_of(c, a[0]); uint32_t e; Const *k = find_const(c, t, a[1], &e); return k ? k->reg : 0xffffffffu; }
static uint32_t ct_bufptr(Ctx *c, uint32_t *a) { return tab_of(c, a[0])->buf_g; }
static uint32_t ct_bufsize(Ctx *c, uint32_t *a) { return tab_of(c, a[0])->buf_len; }
static uint32_t ct_ok(Ctx *c, uint32_t *a) { (void)c; (void)a; return 0; }
static uint32_t ct_write(Ctx *c, CTable *t, uint32_t h, const float *src, uint32_t nfloats, int is_matrix, int transpose, int is_int);
// SetDefaults(device): copy the default values stored in the CTAB into the registers (vectors/scalars; matrix defaults are left alone)
static uint32_t ct_setdefaults(Ctx *c, uint32_t *a) {
    CTable *t = tab_of(c, a[0]);
    for (int i = 0; i < t->n; i++) { Const *k = &t->k[i]; if (!k->def_g || k->cls > 1 || k->set > 2) continue;
        uint32_t comps = k->cls == 0 ? 1 : k->cols ? k->cols : 4, n = comps * (k->count ? k->count : 1); if (n > 256) continue; float buf[256]; rdbuf(c, k->def_g, buf, 4 * n);
        ct_write(c, t, (uint32_t)i + 1, buf, n, 0, 0, k->set != 2); }
    return 0;
}

// write `nelem` values of the constant (with element offset) into registers. kind: 'f' floats (4 per reg for vectors), 'm' row-major matrices
static uint32_t ct_write(Ctx *c, CTable *t, uint32_t h, const float *src, uint32_t nfloats, int is_matrix, int transpose, int is_int) {
    uint32_t el; Const *k = find_const(c, t, h, &el); if (!k) return 0x8876086c;
    uint32_t per = k->elems ? k->count / k->elems : k->count, reg = k->reg + el * per;
    int set = k->set == 2 ? 2 : k->set == 1 ? 1 : 0;
    if (is_matrix) {
        uint32_t cols_major = (k->cls == 3) ^ (transpose != 0);                    // register i holds a column of the supplied row-major matrix
        uint32_t nregs = k->cls == 3 ? k->cols : k->rows, mats = nfloats / 16 ? nfloats / 16 : 1;
        for (uint32_t m = 0; m < mats; m++) {
            float v[16][4]; memset(v, 0, sizeof v); const float (*M)[4] = (const float (*)[4])(src + 16 * m);
            for (uint32_t i = 0; i < nregs && i < 4; i++) for (uint32_t j = 0; j < 4; j++) v[i][j] = cols_major ? M[j][i] : M[i][j];
            d3d9_set_const(c, t->ps, 2, reg + m * per, v, nregs);
        }
        return 0;
    }
    if (set == 2) {                                                               // float scalars/vectors: pack 4 per register when the constant spans several, else component-wise
        uint32_t comps = k->cls == 0 ? 1 : k->cols ? k->cols : 4;
        uint32_t n = nfloats; float v[64][4]; memset(v, 0, sizeof v);
        for (uint32_t r = 0; r < 64 && r * comps < n; r++) { float cur[4]; d3d9_get_const_f(t->ps, reg + r, cur); memcpy(v[r], cur, 16); for (uint32_t j = 0; j < comps && r * comps + j < n; j++) v[r][j] = src[r * comps + j]; }
        d3d9_set_const(c, t->ps, 2, reg, v, (n + comps - 1) / comps); return 0;
    }
    int32_t v[16][4]; memset(v, 0, sizeof v);
    for (uint32_t r = 0; r < 16 && r < nfloats; r++) v[r][0] = is_int ? ((const int32_t *)src)[r] : ((const int32_t *)src)[r];
    d3d9_set_const(c, t->ps, set, reg, v, nfloats); return 0;
}
static uint32_t ct_setvalue(Ctx *c, uint32_t *a) { uint8_t *b = malloc(a[4] + 16); rdbuf(c, a[3], b, a[4]); uint32_t r = ct_write(c, tab_of(c, a[0]), a[2], (float *)b, a[4] / 4, 0, 0, 0); free(b); return r; }
static uint32_t ct_setscalar(Ctx *c, uint32_t *a) { float f[1]; memcpy(f, &a[3], 4); return ct_write(c, tab_of(c, a[0]), a[2], f, 1, 0, 0, 0); }
static uint32_t ct_setarray(Ctx *c, uint32_t *a) { float *b = malloc(4 * (a[4] + 4)); rdbuf(c, a[3], b, 4 * a[4]); uint32_t r = ct_write(c, tab_of(c, a[0]), a[2], b, a[4], 0, 0, 0); free(b); return r; }
static uint32_t ct_setvector(Ctx *c, uint32_t *a) { float b[4]; rdbuf(c, a[3], b, 16); return ct_write(c, tab_of(c, a[0]), a[2], b, 4, 0, 0, 0); }
static uint32_t ct_setvectorarray(Ctx *c, uint32_t *a) { float *b = malloc(16 * (a[4] + 1)); rdbuf(c, a[3], b, 16 * a[4]); uint32_t r = ct_write(c, tab_of(c, a[0]), a[2], b, 4 * a[4], 0, 0, 0); free(b); return r; }
static uint32_t ct_setmatrix(Ctx *c, uint32_t *a) { float b[16]; rdbuf(c, a[3], b, 64); return ct_write(c, tab_of(c, a[0]), a[2], b, 16, 1, 0, 0); }
static uint32_t ct_setmatrixarray(Ctx *c, uint32_t *a) { float *b = malloc(64 * (a[4] + 1)); rdbuf(c, a[3], b, 64 * a[4]); uint32_t r = ct_write(c, tab_of(c, a[0]), a[2], b, 16 * a[4], 1, 0, 0); free(b); return r; }
static uint32_t ct_setmatrixT(Ctx *c, uint32_t *a) { float b[16]; rdbuf(c, a[3], b, 64); return ct_write(c, tab_of(c, a[0]), a[2], b, 16, 1, 1, 0); }
static uint32_t ct_setmatrixTarray(Ctx *c, uint32_t *a) { float *b = malloc(64 * (a[4] + 1)); rdbuf(c, a[3], b, 64 * a[4]); uint32_t r = ct_write(c, tab_of(c, a[0]), a[2], b, 16 * a[4], 1, 1, 0); free(b); return r; }

#define ZZ(n, na) { n, na, com_ok }
static const ComMethod ct_m[] = {
    ZZ("QueryInterface", 3), ZZ("AddRef", 1), ZZ("Release", 1), { "GetBufferPointer", 1, ct_bufptr }, { "GetBufferSize", 1, ct_bufsize }, { "GetDesc", 2, ct_getdesc },
    { "GetConstantDesc", 4, ct_constdesc }, { "GetSamplerIndex", 2, ct_sampler }, { "GetConstant", 3, ct_getconstant }, { "GetConstantByName", 3, ct_byname },
    { "GetConstantElement", 3, ct_element }, { "SetDefaults", 2, ct_setdefaults }, { "SetValue", 5, ct_setvalue }, { "SetBool", 4, ct_setscalar }, { "SetBoolArray", 5, ct_setarray },
    { "SetInt", 4, ct_setscalar }, { "SetIntArray", 5, ct_setarray }, { "SetFloat", 4, ct_setscalar }, { "SetFloatArray", 5, ct_setarray }, { "SetVector", 4, ct_setvector },
    { "SetVectorArray", 5, ct_setvectorarray }, { "SetMatrix", 4, ct_setmatrix }, { "SetMatrixArray", 5, ct_setmatrixarray }, { "SetMatrixPointerArray", 5, ct_ok },
    { "SetMatrixTranspose", 4, ct_setmatrixT }, { "SetMatrixTransposeArray", 5, ct_setmatrixTarray }, { "SetMatrixTransposePointerArray", 5, ct_ok },
};
static uint32_t x_getctable(Ctx *c, uint32_t *a) {                                  // D3DXGetShaderConstantTable(pFunction, ppConstantTable)
    if (ntabs >= MAXTABS) { fprintf(stderr, "[d3dx] constant table limit reached\n"); wr32(c, a[1], 0); return 0; }
    CTable *t = &tabs[ntabs]; memset(t, 0, sizeof *t);
    if (c->verbose) { fprintf(stderr, "[d3dx] parse_ctab code=%08x:", a[0]); for (int i = 0; i < 12; i++) fprintf(stderr, " %08x", rd32(c, a[0] + 4 * i)); fprintf(stderr, "\n"); }
    if (!parse_ctab(c, a[0], t)) { wr32(c, a[1], 0); return 0; }                    // shaders without a table: no object (D3D_OK like D3DX)
    uint32_t g = com_make(c, "ID3DXConstantTable", ct_m, sizeof ct_m / sizeof ct_m[0], 4);
    wr32(c, g + 8, (uint32_t)ntabs++); wr32(c, a[1], g);
    if (c->verbose) { fprintf(stderr, "[d3dx] constant table %s%u: %d constants:", t->ps ? "ps" : "vs", t->version & 0xffff, t->n); for (int i = 0; i < t->n; i++) fprintf(stderr, " %s(r%u/%u)", t->k[i].name, t->k[i].reg, t->k[i].count); fprintf(stderr, "\n"); }
    return 0;
}

// ---- ID3DXBuffer + D3DXCompileShader via vkd3d-shader (HLSL -> Direct3D shader-model-3 bytecode) --------------------------------
static uint32_t buf_ptr(Ctx *c, uint32_t *a) { return rd32(c, a[0] + 8); }
static uint32_t buf_size(Ctx *c, uint32_t *a) { return rd32(c, a[0] + 12); }
static const ComMethod xbuf_m[] = { ZZ("QueryInterface", 3), ZZ("AddRef", 1), ZZ("Release", 1), { "GetBufferPointer", 1, buf_ptr }, { "GetBufferSize", 1, buf_size } };
static uint32_t make_xbuffer(Ctx *c, const void *data, uint32_t n) {
    uint32_t g = com_make(c, "ID3DXBuffer", xbuf_m, 5, 8), p = guest_alloc(c, n + 4); wrbuf(c, p, data, n); wr32(c, g + 8, p); wr32(c, g + 12, n); return g;
}
static int shader_counter;
// D3DXCompileShader: the game embeds a few HLSL sources and compiles them at run time. Results are cached by a hash of (source, entry, profile)
// in $BLUR_HLSL_CACHE/hlsl_<hash>.bin, so the wasm build (which has no vkd3d-shader) can serve them from the cache that the native build writes.
static uint64_t fnv64(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }
static uint32_t x_compile(Ctx *c, uint32_t *a) {      // (src, len, defines, include, entry, profile, flags, ppShader, ppErrors, ppConstTable)
    char *src = malloc(a[1] + 1); rdbuf(c, a[0], src, a[1]); src[a[1]] = 0;
    char entry[64] = "main", profile[16] = "vs_3_0"; if (a[4]) rd_cstr(c, a[4], entry, sizeof entry); if (a[5]) rd_cstr(c, a[5], profile, sizeof profile);
    uint64_t key = fnv64(fnv64(fnv64(0xcbf29ce484222325ull, src, a[1]), entry, strlen(entry)), profile, strlen(profile));
    const char *cdir = getenv("BLUR_HLSL_CACHE"); char cpath[600] = ""; uint8_t *code = NULL; size_t csize = 0; char *msgs = NULL; int rc = -1, id = ++shader_counter;
    if (cdir) {
        snprintf(cpath, sizeof cpath, "%s/hlsl_%016llx.bin", cdir, (unsigned long long)key); FILE *f = fopen(cpath, "rb");
        if (f) { fseek(f, 0, SEEK_END); csize = (size_t)ftell(f); fseek(f, 0, SEEK_SET); code = malloc(csize + 1); if (fread(code, 1, csize, f) != csize) { free(code); code = NULL; } fclose(f); if (code) rc = 0; }
    }
#ifndef __EMSCRIPTEN__
    struct vkd3d_shader_code out = {0};
    if (!code) {
        struct vkd3d_shader_hlsl_source_info hlsl = { VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO, NULL, entry, {0}, profile };
        struct vkd3d_shader_compile_info info = { VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO, &hlsl, {src, a[1]}, VKD3D_SHADER_SOURCE_HLSL, VKD3D_SHADER_TARGET_D3D_BYTECODE };
        info.log_level = VKD3D_SHADER_LOG_INFO;
        rc = vkd3d_shader_compile(&info, &out, &msgs);
        if (rc >= 0) {
            code = malloc(out.size + 1); memcpy(code, out.code, out.size); csize = out.size; vkd3d_shader_free_shader_code(&out);
            if (cdir) { FILE *f = fopen(cpath, "wb"); if (f) { fwrite(code, 1, csize, f); fclose(f); } }
        }
    }
    if (getenv("BLUR_DUMP_SHADERS")) {
        char fn[256]; snprintf(fn, sizeof fn, "%s/shader_%03d_%s.hlsl", getenv("BLUR_DUMP_SHADERS"), id, profile); FILE *f = fopen(fn, "w"); if (f) { fwrite(src, 1, a[1], f); fclose(f); }
        if (rc >= 0) { snprintf(fn, sizeof fn, "%s/shader_%03d_%s.bin", getenv("BLUR_DUMP_SHADERS"), id, profile); f = fopen(fn, "wb"); if (f) { fwrite(code, 1, csize, f); fclose(f); } }
    }
#endif
    if (c->verbose || rc < 0) fprintf(stderr, "[d3dx] CompileShader #%d %s/%s (%u bytes, key %016llx) -> %s%s%s\n", id, entry, profile, a[1], (unsigned long long)key, rc >= 0 ? "ok " : "FAILED ", msgs ? "\n" : "", msgs ? msgs : "");
    free(src);
    if (rc < 0 || !code) {
        if (a[8]) wr32(c, a[8], msgs ? make_xbuffer(c, msgs, (uint32_t)strlen(msgs) + 1) : 0);
#ifndef __EMSCRIPTEN__
        if (msgs) vkd3d_shader_free_messages(msgs);
#endif
        return 0x88760b59;
    }
#ifndef __EMSCRIPTEN__
    if (msgs) vkd3d_shader_free_messages(msgs);
#endif
    uint32_t g = make_xbuffer(c, code, (uint32_t)csize); free(code); if (a[7]) wr32(c, a[7], g);
    if (a[9]) { uint32_t args[2] = { rd32(c, g + 8), a[9] }; x_getctable(c, args); }
    if (a[8]) wr32(c, a[8], 0);
    return 0;
}

// D3DXGetShaderInputSemantics(pFunction, D3DXSEMANTIC* out, UINT* count): usage/index of every `dcl` on an input register
static uint32_t x_inputsem(Ctx *c, uint32_t *a) {
    uint32_t n = 0, outs[64][2]; uint32_t p = a[0] + 4;
    for (int guard = 0; guard < 100000; guard++) {
        uint32_t tok = rd32(c, p);
        if (tok == 0x0000ffff) break;
        if ((tok & 0xffff) == 0xfffe) { p += 4 + ((tok >> 16) & 0x7fff) * 4; continue; }
        uint32_t len = (tok >> 24) & 0xf;
        if ((tok & 0xffff) == 0x1f) {                                              // D3DSIO_DCL: usage token, destination token
            uint32_t u = rd32(c, p + 4), d = rd32(c, p + 8), type = ((d >> 28) & 7) | ((d >> 8) & 0x18);
            if (type == 1 && n < 64) { outs[n][0] = u & 0x1f; outs[n][1] = (u >> 16) & 0xf; n++; }       // D3DSPR_INPUT
        }
        p += 4 + len * 4;
    }
    if (a[1]) for (uint32_t i = 0; i < n; i++) { wr32(c, a[1] + 8 * i, outs[i][0]); wr32(c, a[1] + 8 * i + 4, outs[i][1]); }
    if (a[2]) wr32(c, a[2], n);
    return 0;
}

static const HleDef tbl[] = {
    { "D3DXGetShaderInputSemantics", 3, 0, x_inputsem },
    { "D3DXCompileShader", 10, 0, x_compile },
    { "D3DXGetShaderConstantTable", 2, 0, x_getctable },
    { "D3DXMatrixMultiply", 3, 0, x_multiply }, { "D3DXMatrixTranspose", 2, 0, x_transpose }, { "D3DXMatrixInverse", 3, 0, x_inverse },
    { "D3DXMatrixLookAtLH", 4, 0, x_lookat }, { "D3DXMatrixOrthoLH", 5, 0, x_ortho }, { "D3DXPlaneNormalize", 2, 0, x_plane_normalize },
    { "D3DXPlaneTransform", 3, 0, x_plane_transform }, { "D3DXGetVertexShaderProfile", 1, 0, x_vsprofile }, { "D3DXGetPixelShaderProfile", 1, 0, x_psprofile },
    { "D3DXSaveSurfaceToFileA", 4, 0, x_ok },
};
const HleDef *d3dx_lookup(const char *name) {
    for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++) if (!strcmp(tbl[i].name, name)) return &tbl[i];
    return NULL;
}

int d3dx_snap_busy(void) { return ntabs - 1 + shader_counter; }
