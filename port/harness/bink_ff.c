// FFmpeg backed Bink reader (see bink_ff.h).
#include "bink_ff.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct { uint8_t *p; size_t n, cap, rd; } Fifo;
typedef struct { AVCodecContext *cc; SwrContext *swr; int sidx; Fifo q; int ch, rate; } ATrack;

struct BkFF {
    AVFormatContext *fc; AVIOContext *io; const uint8_t *mem; size_t memsize, mempos;
    int vidx; AVCodecContext *vc; AVPacket *pkt; AVFrame *vf, *af;
    int ntr; ATrack tr[BKFF_MAXTRACKS]; int sidx2track[64];
    int next;        // index of the next video frame to be produced by the decoder
    int have;        // index of the frame currently held in vf (-1 none)
    int w, h, alpha, eof;
};

static int mem_read(void *o, uint8_t *buf, int n) {
    BkFF *f = o; size_t left = f->memsize - f->mempos; if (!left) return AVERROR_EOF; if ((size_t)n > left) n = (int)left;
    memcpy(buf, f->mem + f->mempos, (size_t)n); f->mempos += (size_t)n; return n;
}
static int64_t mem_seek(void *o, int64_t off, int whence) {
    BkFF *f = o; if (whence == AVSEEK_SIZE) return (int64_t)f->memsize;
    whence &= ~AVSEEK_FORCE; int64_t np = whence == SEEK_SET ? off : whence == SEEK_CUR ? (int64_t)f->mempos + off : (int64_t)f->memsize + off;
    if (np < 0 || np > (int64_t)f->memsize) return -1; f->mempos = (size_t)np; return np;
}

static void fifo_push(Fifo *q, const void *d, size_t n) {
    if (q->rd && q->rd == q->n) q->rd = q->n = 0;
    if (q->n + n > q->cap) { if (q->rd) { memmove(q->p, q->p + q->rd, q->n - q->rd); q->n -= q->rd; q->rd = 0; } if (q->n + n > q->cap) { q->cap = (q->n + n) * 2; q->p = realloc(q->p, q->cap); } }
    memcpy(q->p + q->n, d, n); q->n += n;
}

static int finish_open(BkFF *f, BkInfo *info) {
    if (avformat_find_stream_info(f->fc, NULL) < 0) return -1;
    f->vidx = -1; f->ntr = 0;
    for (unsigned i = 0; i < f->fc->nb_streams; i++) {
        AVStream *s = f->fc->streams[i]; AVCodecParameters *cp = s->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO && f->vidx < 0) {
            const AVCodec *c = avcodec_find_decoder(cp->codec_id); if (!c) return -1;
            f->vc = avcodec_alloc_context3(c); avcodec_parameters_to_context(f->vc, cp); f->vc->thread_count = 1;
            if (avcodec_open2(f->vc, c, NULL) < 0) return -1;
            f->vidx = (int)i; f->w = cp->width; f->h = cp->height;
            f->alpha = f->vc->pix_fmt == AV_PIX_FMT_YUVA420P;
            int fn = s->avg_frame_rate.num ? s->avg_frame_rate.num : s->r_frame_rate.num, fd = s->avg_frame_rate.num ? s->avg_frame_rate.den : s->r_frame_rate.den;
            info->fps_num = fn ? fn : 30; info->fps_den = fd ? fd : 1; info->frames = (int)s->nb_frames;
        } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO && f->ntr < BKFF_MAXTRACKS) {
            const AVCodec *c = avcodec_find_decoder(cp->codec_id); if (!c) continue;
            ATrack *t = &f->tr[f->ntr]; t->cc = avcodec_alloc_context3(c); avcodec_parameters_to_context(t->cc, cp);
            if (avcodec_open2(t->cc, c, NULL) < 0) { avcodec_free_context(&t->cc); continue; }
            t->sidx = (int)i; t->rate = cp->sample_rate; t->ch = cp->ch_layout.nb_channels;
            f->sidx2track[i < 64 ? i : 63] = f->ntr;
            info->tracks[f->ntr] = (BkTrackInfo){ .id = s->id, .rate = t->rate, .channels = t->ch }; f->ntr++;
        }
    }
    if (f->vidx < 0) return -1;
    if (info->frames <= 0) { AVStream *s = f->fc->streams[f->vidx]; info->frames = (int)(s->duration * s->avg_frame_rate.num / (s->avg_frame_rate.den ? s->avg_frame_rate.den : 1) * s->time_base.num / (s->time_base.den ? s->time_base.den : 1)); }
    info->width = f->w; info->height = f->h; info->has_alpha = f->alpha; info->ntracks = f->ntr;
    f->pkt = av_packet_alloc(); f->vf = av_frame_alloc(); f->af = av_frame_alloc(); f->have = -1; f->next = 0;
    return 0;
}

BkFF *bkff_open_file(const char *path, BkInfo *info) {
    BkFF *f = calloc(1, sizeof *f); memset(info, 0, sizeof *info); av_log_set_level(AV_LOG_ERROR);
    if (avformat_open_input(&f->fc, path, NULL, NULL) < 0 || finish_open(f, info)) { bkff_close(f); return NULL; }
    return f;
}
BkFF *bkff_open_mem(const uint8_t *data, size_t size, BkInfo *info) {
    BkFF *f = calloc(1, sizeof *f); memset(info, 0, sizeof *info); av_log_set_level(AV_LOG_ERROR);
    f->mem = data; f->memsize = size; uint8_t *buf = av_malloc(32768);
    f->io = avio_alloc_context(buf, 32768, 0, f, mem_read, NULL, mem_seek);
    f->fc = avformat_alloc_context(); f->fc->pb = f->io;
    if (avformat_open_input(&f->fc, "mem.bik", NULL, NULL) < 0 || finish_open(f, info)) { bkff_close(f); return NULL; }
    return f;
}
void bkff_close(BkFF *f) {
    if (!f) return;
    for (int i = 0; i < f->ntr; i++) { avcodec_free_context(&f->tr[i].cc); swr_free(&f->tr[i].swr); free(f->tr[i].q.p); }
    if (f->vc) avcodec_free_context(&f->vc);
    av_packet_free(&f->pkt); av_frame_free(&f->vf); av_frame_free(&f->af);
    if (f->fc) avformat_close_input(&f->fc);
    if (f->io) { av_freep(&f->io->buffer); avio_context_free(&f->io); }
    free(f);
}

static void decode_audio(BkFF *f, ATrack *t) {
    if (avcodec_send_packet(t->cc, f->pkt) < 0) return;
    while (avcodec_receive_frame(t->cc, f->af) >= 0) {
        if (!t->swr) {
            AVChannelLayout out; av_channel_layout_default(&out, f->af->ch_layout.nb_channels);
            if (swr_alloc_set_opts2(&t->swr, &out, AV_SAMPLE_FMT_S16, f->af->sample_rate, &f->af->ch_layout, f->af->format, f->af->sample_rate, 0, NULL) < 0 || swr_init(t->swr) < 0) { swr_free(&t->swr); av_frame_unref(f->af); return; }
            t->ch = f->af->ch_layout.nb_channels; t->rate = f->af->sample_rate;
        }
        int n = f->af->nb_samples; int16_t *tmp = malloc((size_t)n * (size_t)t->ch * 2); uint8_t *o[1] = { (uint8_t *)tmp };
        int got = swr_convert(t->swr, o, n, (const uint8_t **)f->af->extended_data, n);
        if (got > 0) fifo_push(&t->q, tmp, (size_t)got * (size_t)t->ch * 2);
        free(tmp); av_frame_unref(f->af);
    }
}

static int rewind_(BkFF *f) {
    if (av_seek_frame(f->fc, f->vidx, 0, AVSEEK_FLAG_BACKWARD) < 0) return -1;
    avcodec_flush_buffers(f->vc);
    for (int i = 0; i < f->ntr; i++) { avcodec_flush_buffers(f->tr[i].cc); f->tr[i].q.n = f->tr[i].q.rd = 0; }
    f->next = 0; f->have = -1; f->eof = 0; return 0;
}

int bkff_frame(BkFF *f, int n) {
    if (n == f->have) return 0;
    if (n < f->next && !(n == f->next - 1)) { if (rewind_(f)) return -1; }
    else if (n == f->next - 1) return f->have == n ? 0 : -1;
    while (f->next <= n) {
        int r = av_read_frame(f->fc, f->pkt);
        if (r < 0) { f->eof = 1; return -1; }
        if (f->pkt->stream_index == f->vidx) {
            if (avcodec_send_packet(f->vc, f->pkt) >= 0) {
                if (avcodec_receive_frame(f->vc, f->vf) >= 0) { f->have = f->next; f->next++; }
            }
        } else if (f->pkt->stream_index < 64 && f->fc->streams[f->pkt->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            for (int i = 0; i < f->ntr; i++) if (f->tr[i].sidx == f->pkt->stream_index) { decode_audio(f, &f->tr[i]); break; }
        }
        av_packet_unref(f->pkt);
    }
    return f->have == n ? 0 : -1;
}

const uint8_t *bkff_plane(BkFF *f, int plane, int *linesize) {
    if (f->have < 0 || plane < 0 || plane > 3) return NULL;
    if (plane == 3 && !f->alpha) return NULL;
    if (linesize) *linesize = f->vf->linesize[plane];
    return f->vf->data[plane];
}
size_t bkff_track_pending(BkFF *f, int t) { if (t < 0 || t >= f->ntr) return 0; return f->tr[t].q.n - f->tr[t].q.rd; }
size_t bkff_track_read(BkFF *f, int t, void *dst, size_t max) {
    if (t < 0 || t >= f->ntr) return 0; Fifo *q = &f->tr[t].q; size_t fb = (size_t)f->tr[t].ch * 2; if (!fb) fb = 4;
    size_t n = q->n - q->rd; if (n > max) n = max; n -= n % fb; if (!n) return 0;
    memcpy(dst, q->p + q->rd, n); q->rd += n; return n;
}
