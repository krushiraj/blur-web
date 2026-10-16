// Offline shader translation cache ("BSC1"): D3D9 bytecode -> GLSL ES 3.00 + IO tables, keyed by FNV-1a 64 of the bytecode (+ length).
// File layout (little endian):  "BSC1", u32 count, then per entry
//   u32 hash_lo, u32 hash_hi, u32 code_len, u8 stage(0 vs/1 ps), u8 nin, u8 nout, u8 nsamp, u32 glsl_len,
//   IOEnt in[nin], out[nout], samp[nsamp] (4 bytes each: reg, usage, idx, tex), GLSL text (glsl_len bytes, no NUL), zero padding to 4 bytes.
// The same payload (after the 20-byte fixed head) is what the live G_SHADER message carries, so the browser decodes both identically.
#pragma once
#include <stdint.h>
#include <stddef.h>

static inline uint64_t shc_fnv64(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
int shc_load(const char *path);                       // loads the key set (returns number of entries, <0 on error)
int shc_has(uint64_t hash, uint32_t len);             // 1 if present in the loaded cache
