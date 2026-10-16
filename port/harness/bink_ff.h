// Thin FFmpeg (libavformat/libavcodec) wrapper that gives the Bink HLE what binkw32 gives a game: sequential video frames as
// planar YUV (Y, cR/cB at half size, optional A), and per-track PCM (signed 16-bit interleaved, native rate/channels).
// No emulator dependency.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define BKFF_MAXTRACKS 8

typedef struct BkTrackInfo { int id; int rate, channels; } BkTrackInfo;
typedef struct BkInfo {
    int width, height, frames;
    int fps_num, fps_den;               // frame rate = fps_num / fps_den
    int has_alpha;
    int ntracks; BkTrackInfo tracks[BKFF_MAXTRACKS];
} BkInfo;

typedef struct BkFF BkFF;
BkFF *bkff_open_file(const char *path, BkInfo *info);
BkFF *bkff_open_mem(const uint8_t *data, size_t size, BkInfo *info);       // data must outlive the handle
void  bkff_close(BkFF *f);
// Decode video frame `n` (0-based). Sequential access is cheap; n==last returns the cached frame; a smaller n rewinds to the start.
// Audio packets met on the way are decoded into the per-track FIFOs. Returns 0 on success, <0 at end of stream/error.
int   bkff_frame(BkFF *f, int n);
// Planes of the frame returned by the last successful bkff_frame (valid until the next call). plane 3 is NULL without alpha.
const uint8_t *bkff_plane(BkFF *f, int plane, int *linesize);
// Pop up to max_bytes of decoded PCM of track `t` (whole sample frames only). Returns bytes written.
size_t bkff_track_read(BkFF *f, int t, void *dst, size_t max_bytes);
size_t bkff_track_pending(BkFF *f, int t);
