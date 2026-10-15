// SSE2 / MMX integer vector helpers for the recompiled code. d and s point at n bytes (16 for xmm, 8 for mm registers).
#include "rt.h"

#define LANES(T, body) do { T *D = (T *)d; const T *S = (const T *)s; for (int i = 0; i < n / (int)sizeof(T); i++) { body; } } while (0)
#define LANES1(T, body) do { T *D = (T *)d; for (int i = 0; i < n / (int)sizeof(T); i++) { body; } } while (0)
static inline int8_t  sat_s8 (int v) { return v > 127 ? 127 : v < -128 ? -128 : (int8_t)v; }
static inline int16_t sat_s16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline uint8_t sat_u8 (int v) { return v > 255 ? 255 : v < 0 ? 0 : (uint8_t)v; }
static inline uint16_t sat_u16(int v) { return v > 65535 ? 65535 : v < 0 ? 0 : (uint16_t)v; }

void v_binop(int op, uint8_t *d, const uint8_t *s, int n) {
    uint8_t tmp[16]; memcpy(tmp, s, n); s = tmp;                       // d and s may alias
    switch (op) {
    case V_PCMPEQB: LANES(uint8_t,  D[i] = D[i] == S[i] ? 0xff : 0); break;
    case V_PCMPEQW: LANES(uint16_t, D[i] = D[i] == S[i] ? 0xffff : 0); break;
    case V_PCMPEQD: LANES(uint32_t, D[i] = D[i] == S[i] ? 0xffffffffu : 0); break;
    case V_PCMPGTB: LANES(int8_t,   D[i] = D[i] > S[i] ? -1 : 0); break;
    case V_PCMPGTW: LANES(int16_t,  D[i] = D[i] > S[i] ? -1 : 0); break;
    case V_PCMPGTD: LANES(int32_t,  D[i] = D[i] > S[i] ? -1 : 0); break;
    case V_PADDB:   LANES(uint8_t,  D[i] = D[i] + S[i]); break;
    case V_PADDW:   LANES(uint16_t, D[i] = D[i] + S[i]); break;
    case V_PADDD:   LANES(uint32_t, D[i] = D[i] + S[i]); break;
    case V_PADDQ:   LANES(uint64_t, D[i] = D[i] + S[i]); break;
    case V_PSUBB:   LANES(uint8_t,  D[i] = D[i] - S[i]); break;
    case V_PSUBW:   LANES(uint16_t, D[i] = D[i] - S[i]); break;
    case V_PSUBD:   LANES(uint32_t, D[i] = D[i] - S[i]); break;
    case V_PSUBQ:   LANES(uint64_t, D[i] = D[i] - S[i]); break;
    case V_PADDUSB: LANES(uint8_t,  D[i] = sat_u8(D[i] + S[i])); break;
    case V_PADDUSW: LANES(uint16_t, D[i] = sat_u16(D[i] + S[i])); break;
    case V_PSUBUSB: LANES(uint8_t,  D[i] = sat_u8((int)D[i] - S[i])); break;
    case V_PSUBUSW: LANES(uint16_t, D[i] = sat_u16((int)D[i] - S[i])); break;
    case V_PADDSB:  LANES(int8_t,   D[i] = sat_s8(D[i] + S[i])); break;
    case V_PADDSW:  LANES(int16_t,  D[i] = sat_s16(D[i] + S[i])); break;
    case V_PSUBSB:  LANES(int8_t,   D[i] = sat_s8(D[i] - S[i])); break;
    case V_PSUBSW:  LANES(int16_t,  D[i] = sat_s16(D[i] - S[i])); break;
    case V_PMAXUB:  LANES(uint8_t,  D[i] = D[i] > S[i] ? D[i] : S[i]); break;
    case V_PMINUB:  LANES(uint8_t,  D[i] = D[i] < S[i] ? D[i] : S[i]); break;
    case V_PMAXSW:  LANES(int16_t,  D[i] = D[i] > S[i] ? D[i] : S[i]); break;
    case V_PMINSW:  LANES(int16_t,  D[i] = D[i] < S[i] ? D[i] : S[i]); break;
    case V_PAVGB:   LANES(uint8_t,  D[i] = (D[i] + S[i] + 1) >> 1); break;
    case V_PAVGW:   LANES(uint16_t, D[i] = (D[i] + S[i] + 1) >> 1); break;
    case V_PMULLW:  LANES(int16_t,  D[i] = (int16_t)(D[i] * S[i])); break;
    case V_PMULHW:  LANES(int16_t,  D[i] = (int16_t)(((int)D[i] * S[i]) >> 16)); break;
    case V_PMULHUW: LANES(uint16_t, D[i] = (uint16_t)(((uint32_t)D[i] * S[i]) >> 16)); break;
    case V_PMADDWD: { int16_t *D = (int16_t *)d; const int16_t *S = (const int16_t *)s; int32_t r[4]; for (int i = 0; i < n / 4; i++) r[i] = D[2*i] * S[2*i] + D[2*i+1] * S[2*i+1]; memcpy(d, r, n); } break;
    case V_PSADBW: { for (int h = 0; h < n / 8; h++) { unsigned sum = 0; for (int i = 0; i < 8; i++) { int x = d[8*h+i] - s[8*h+i]; sum += x < 0 ? -x : x; } memset(d + 8*h, 0, 8); *(uint16_t *)(d + 8*h) = (uint16_t)sum; } } break;
    case V_PAND:    LANES(uint64_t, D[i] &= S[i]); break;
    case V_PANDN:   LANES(uint64_t, D[i] = ~D[i] & S[i]); break;
    case V_POR:     LANES(uint64_t, D[i] |= S[i]); break;
    case V_PXOR:    LANES(uint64_t, D[i] ^= S[i]); break;
    case V_PUNPCKLBW: case V_PUNPCKLWD: case V_PUNPCKLDQ: case V_PUNPCKHBW: case V_PUNPCKHWD: case V_PUNPCKHDQ: {
        int w = (op == V_PUNPCKLBW || op == V_PUNPCKHBW) ? 1 : (op == V_PUNPCKLWD || op == V_PUNPCKHWD) ? 2 : 4;
        int hi = op >= V_PUNPCKHBW; uint8_t r[16]; int half = n / 2, k = 0;
        for (int i = 0; i < half / w; i++) { memcpy(r + k, d + (hi ? half : 0) + i * w, w); k += w; memcpy(r + k, s + (hi ? half : 0) + i * w, w); k += w; }
        memcpy(d, r, n); } break;
    case V_PACKSSWB: { int16_t *D = (int16_t *)d; const int16_t *S = (const int16_t *)s; int8_t r[16]; int h = n / 2; for (int i = 0; i < h; i++) { r[i] = sat_s8(D[i]); r[h + i] = sat_s8(S[i]); } memcpy(d, r, n); } break;
    case V_PACKUSWB: { int16_t *D = (int16_t *)d; const int16_t *S = (const int16_t *)s; uint8_t r[16]; int h = n / 2; for (int i = 0; i < h; i++) { r[i] = sat_u8(D[i]); r[h + i] = sat_u8(S[i]); } memcpy(d, r, n); } break;
    case V_PACKSSDW: { int32_t *D = (int32_t *)d; const int32_t *S = (const int32_t *)s; int16_t r[8]; int h = n / 4; for (int i = 0; i < h; i++) { r[i] = sat_s16(D[i]); r[h + i] = sat_s16(S[i]); } memcpy(d, r, n); } break;
    }
}

// shifts: count is a plain number (immediate or the low qword of the count operand)
void v_shift(int op, uint8_t *d, uint64_t cnt, int n) {
    switch (op) {
    case V_PSLLW: LANES1(uint16_t, D[i] = cnt > 15 ? 0 : (uint16_t)(D[i] << cnt)); break;
    case V_PSLLD: LANES1(uint32_t, D[i] = cnt > 31 ? 0 : D[i] << cnt); break;
    case V_PSLLQ: LANES1(uint64_t, D[i] = cnt > 63 ? 0 : D[i] << cnt); break;
    case V_PSRLW: LANES1(uint16_t, D[i] = cnt > 15 ? 0 : (uint16_t)(D[i] >> cnt)); break;
    case V_PSRLD: LANES1(uint32_t, D[i] = cnt > 31 ? 0 : D[i] >> cnt); break;
    case V_PSRLQ: LANES1(uint64_t, D[i] = cnt > 63 ? 0 : D[i] >> cnt); break;
    case V_PSRAW: { int sh = cnt > 15 ? 15 : (int)cnt; int16_t *D = (int16_t *)d; for (int i = 0; i < n / 2; i++) D[i] >>= sh; } break;
    case V_PSRAD: { int sh = cnt > 31 ? 31 : (int)cnt; int32_t *D = (int32_t *)d; for (int i = 0; i < n / 4; i++) D[i] >>= sh; } break;
    }
}

uint32_t v_movmskb(const uint8_t *s, int n) { uint32_t r = 0; for (int i = 0; i < n; i++) r |= (uint32_t)(s[i] >> 7) << i; return r; }
