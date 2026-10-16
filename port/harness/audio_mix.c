// Portable software mixer (see audio_mix.h). Linear-interpolating resampler, volume/pan in DirectSound millibels,
// multichannel -> stereo downmix, crude 3D (distance attenuation + azimuth pan).
#include "audio_mix.h"
#include <math.h>
#include <string.h>

void am_init(AmMixer *m) { memset(m, 0, sizeof *m); m->master = 1.0f; }
int am_add(AmMixer *m, AmVoice *v) {
    for (int i = 0; i < m->nv; i++) if (!m->v[i]) { m->v[i] = v; return i; }
    if (m->nv >= AM_MAXV) return -1;
    m->v[m->nv] = v; return m->nv++;
}
void am_remove(AmMixer *m, AmVoice *v) { for (int i = 0; i < m->nv; i++) if (m->v[i] == v) m->v[i] = 0; }
uint32_t am_frame_bytes(const AmVoice *v) { return (uint32_t)v->channels * (v->bits / 8u); }

static inline float sample_at(const AmVoice *v, uint32_t byte_off, int ch) {
    const uint8_t *p = v->data + byte_off + (uint32_t)ch * (v->bits / 8u);
    if (v->is_float && v->bits == 32) { float f; memcpy(&f, p, 4); return f; }
    switch (v->bits) {
    case 8: return ((int)p[0] - 128) * (1.0f / 128.0f);
    case 16: return (int16_t)(p[0] | p[1] << 8) * (1.0f / 32768.0f);
    case 24: return (int32_t)(p[0] << 8 | p[1] << 16 | (uint32_t)p[2] << 24) * (1.0f / 2147483648.0f);
    case 32: return (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) * (1.0f / 2147483648.0f);
    }
    return 0;
}
// stereo downmix of one source frame (channel order: FL FR [C LFE BL BR SL SR])
static inline void frame_lr(const AmVoice *v, uint32_t off, float *l, float *r) {
    int n = v->channels;
    if (n == 1) { *l = *r = sample_at(v, off, 0); return; }
    float fl = sample_at(v, off, 0), fr = sample_at(v, off, 1);
    if (n == 2) { *l = fl; *r = fr; return; }
    if (n == 4) { *l = fl + 0.7f * sample_at(v, off, 2); *r = fr + 0.7f * sample_at(v, off, 3); return; }
    float c = n > 2 ? sample_at(v, off, 2) : 0, bl = n > 4 ? sample_at(v, off, 4) : 0, br = n > 5 ? sample_at(v, off, 5) : 0;
    float sl = n > 6 ? sample_at(v, off, 6) : 0, sr = n > 7 ? sample_at(v, off, 7) : 0, lfe = n > 3 ? sample_at(v, off, 3) : 0;
    *l = 0.6f * (fl + 0.7f * c + 0.7f * (bl + sl) + 0.5f * lfe); *r = 0.6f * (fr + 0.7f * c + 0.7f * (br + sr) + 0.5f * lfe);
}
static float mb_to_gain(int mb) { if (mb <= -10000) return 0; if (mb >= 0) return 1; return powf(10.0f, mb / 2000.0f); }

void am_render(AmMixer *m, float *out, int frames) {
    memset(out, 0, (size_t)frames * 2 * sizeof(float));
    for (int i = 0; i < m->nv; i++) {
        AmVoice *v = m->v[i]; if (!v) continue; v->ended = 0;
        if (!v->playing || !v->data || !v->size) continue;
        uint32_t fb = am_frame_bytes(v); if (!fb) continue;
        uint32_t nframes = v->size / fb; if (!nframes) continue;
        double step = (double)(v->freq ? v->freq : v->rate) / AM_RATE;
        float gv = mb_to_gain(v->vol_mb), gl = 1, gr = 1;
        if (v->pan_mb > 0) gl = mb_to_gain(-v->pan_mb); else if (v->pan_mb < 0) gr = mb_to_gain(v->pan_mb);
        if (v->is3d) {
            float d = sqrtf(v->x * v->x + v->y * v->y + v->z * v->z), md = v->min_dist > 0 ? v->min_dist : 1.0f;
            float att = d <= md ? 1.0f : md / d; if (v->max_dist > 0 && d > v->max_dist) att = 0;
            float az = d > 1e-4f ? v->x / d : 0;        // -1 (left) .. +1 (right)
            gl *= att * sqrtf(0.5f * (1 - az)) * 1.4142f; gr *= att * sqrtf(0.5f * (1 + az)) * 1.4142f;
        }
        gl *= gv * m->master; gr *= gv * m->master;
        uint32_t cur = v->stream ? (uint32_t)(v->rd % nframes) : v->pos / fb; double frac = v->frac;
        for (int k = 0; k < frames; k++) {
            if (v->stream && __atomic_load_n(&v->rd, __ATOMIC_RELAXED) >= __atomic_load_n(&v->wr, __ATOMIC_ACQUIRE)) { v->underruns++; break; }
            float l0, r0, l1, r1; uint32_t nx = cur + 1;
            if (nx >= nframes) nx = v->looping ? 0 : cur;
            frame_lr(v, cur * fb, &l0, &r0);
            if (frac > 0) { frame_lr(v, nx * fb, &l1, &r1); l0 += (l1 - l0) * (float)frac; r0 += (r1 - r0) * (float)frac; }
            out[2 * k] += l0 * gl; out[2 * k + 1] += r0 * gr;
            frac += step;
            if (frac >= 1.0) {
                uint32_t adv = (uint32_t)frac; frac -= adv; cur += adv;
                if (v->stream) { uint64_t r = v->rd + adv; __atomic_store_n(&v->rd, r, __ATOMIC_RELEASE); cur = (uint32_t)(r % nframes); continue; }
                if (cur >= nframes) {
                    if (v->looping) cur %= nframes;
                    else { v->playing = 0; v->ended = 1; cur = 0; frac = 0; v->frames_done += (uint64_t)k + 1; goto next; }
                }
            }
        }
        v->frames_done += (uint64_t)frames;
    next:
        v->pos = cur * fb; v->frac = frac;
    }
}

void am_to_s16(const float *in, int16_t *out, int n) {
    for (int i = 0; i < n; i++) { float f = in[i] * 32767.0f; if (f > 32767.0f) f = 32767.0f; if (f < -32768.0f) f = -32768.0f; out[i] = (int16_t)lrintf(f); }
}
