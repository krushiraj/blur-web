// Portable software mixer for DirectSound-style buffers. No dependency on the emulator: voices point at host memory
// (in the native harness G + guest address; in the wasm build the same, guest address == linear-memory address).
// Output is interleaved stereo float at AM_RATE.
#pragma once
#include <stdint.h>

#define AM_RATE 48000
#define AM_MAXV 256

typedef struct {
    const uint8_t *data;          // ring storage (host pointer)
    uint32_t size;                // bytes
    uint16_t channels, bits;      // source format (bits 8/16/24/32; is_float for 32-bit float)
    int is_float;
    uint32_t rate;                // source sample rate
    int playing, looping;
    uint32_t pos;                 // play cursor, byte offset (frame aligned)
    double frac;                  // sub-frame position (source frames, 0..1)
    int vol_mb, pan_mb;           // DirectSound units: hundredths of dB (0 .. -10000), pan -10000..10000
    uint32_t freq;                // DSBFREQUENCY override (0 = use rate)
    int is3d; float x, y, z, min_dist, max_dist;   // listener fixed at the origin looking down +z
    uint64_t frames_done;         // total output frames consumed from this voice (diagnostics)
    int ended;                    // set when a non-looping voice ran off its end during the last render
    int stream;                   // host-fed ring (movie audio): plays only while wr > rd, never loops; see audio_stream_*
    uint64_t wr, rd;              // stream: frames written / consumed (monotonic)
    uint32_t underruns;
} AmVoice;

typedef struct {
    AmVoice *v[AM_MAXV]; int nv;
    float master;                 // linear master gain
} AmMixer;

void am_init(AmMixer *m);
int  am_add(AmMixer *m, AmVoice *v);      // returns slot id or -1
void am_remove(AmMixer *m, AmVoice *v);
// Render `frames` stereo frames (2*frames floats) by mixing every playing voice; advances voice cursors.
void am_render(AmMixer *m, float *out, int frames);
// Convert float stereo to clipped int16.
void am_to_s16(const float *in, int16_t *out, int n);
uint32_t am_frame_bytes(const AmVoice *v);
