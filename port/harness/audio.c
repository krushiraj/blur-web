// DirectSound8 HLE for the game's audio engine ("Mercury" XtAudio mixer: the game mixes its own sounds and streams the result
// through DirectSound buffers). IDirectSound8 / IDirectSoundBuffer8 / IDirectSoundNotify / IDirectSound3DBuffer / IDirectSound3DListener
// are emulated as COM objects; the buffers are real guest memory (Lock hands out guest pointers), the play cursor advances with
// the host clock, DSBPOSITIONNOTIFY events are signalled when the cursor crosses an offset, and everything that plays is mixed
// by audio_mix.c into 48 kHz stereo which is forwarded to the browser (WebSocket message G_AUDIO) and optionally dumped to a WAV.
//
// env: BLUR_AUDIOLOG=1 (per-second RMS/peak line) =2 (every DirectSound call), BLUR_AUDIO_WAV=<file> [BLUR_AUDIO_WAV_SECS=20],
//      BLUR_AUDIO_OFF=1 (fail DirectSoundCreate8 like before)
#include "emu.h"
#include "audio_mix.h"
#include <math.h>
#include <time.h>

#define G_AUDIO 90
#define DS_OK 0
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_NOINTERFACE 0x80004002u
#define DSERR_UNSUPPORTED 0x80004001u
#define GP(a) (G + (uint32_t)(a))

enum { DSBCAPS_PRIMARY = 1, DSBCAPS_CTRL3D = 0x10 };
#define MAXBUF 512
#define MAXNOT 64
typedef struct {
    int used; uint32_t guest, notify_obj, d3_obj, flags, bytes, data, refs;
    int primary; AmVoice v; int mixer_slot;
    uint8_t wfx[40]; uint32_t wfx_size;
    int nnot; struct { uint32_t off, ev; } not_[MAXNOT];
    uint32_t last_pos; int was_playing; uint32_t poll_pos; int poll_same;   // GetCurrentPosition poll throttling
    uint32_t play_calls, lock_bytes;
} Buf;
static Buf bufs[MAXBUF]; static int nbufs = 1;
static AmMixer mixer; static int mixer_init;
static uint32_t g_ds_obj, g_listener_obj;
static int alog = -1;
static int dlog_(void) { if (alog < 0) { const char *e = getenv("BLUR_AUDIOLOG"); alog = e ? atoi(e) : 0; } return alog; }
#define LOG(...) do { if (dlog_() >= 2) fprintf(stderr, "[ds] " __VA_ARGS__); } while (0)

// ---- output side: device clock, stats, WAV dump, WebSocket forwarding ------------------------------------------------
// device clock: native = host monotonic time; the wasm build points this at a clock derived from the AudioWorklet ring fill level
uint64_t (*audio_clock_us)(void) = real_us;
static uint64_t t_last; static int out_started;
static int16_t acc[2 * 4800]; static int nacc;
static double st_sum2; static float st_peak; static uint64_t st_frames, st_last_log, st_total_frames, st_sent_frames;
static FILE *wav; static uint32_t wav_frames, wav_max;
static uint64_t wr_total, wr_nonzero; static float wr_peak;

static void wav_hdr(void) {
    uint32_t dl = wav_frames * 4, v;
    fseek(wav, 0, SEEK_SET); fwrite("RIFF", 1, 4, wav); v = 36 + dl; fwrite(&v, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav); v = 16; fwrite(&v, 4, 1, wav);
    uint16_t s = 1; fwrite(&s, 2, 1, wav); s = 2; fwrite(&s, 2, 1, wav); v = AM_RATE; fwrite(&v, 4, 1, wav); v = AM_RATE * 4; fwrite(&v, 4, 1, wav);
    s = 4; fwrite(&s, 2, 1, wav); s = 16; fwrite(&s, 2, 1, wav); fwrite("data", 1, 4, wav); fwrite(&dl, 4, 1, wav); fseek(wav, 0, SEEK_END);
}
static void out_flush(void) {
    if (!nacc) return;
    if (wav && wav_frames < wav_max) { uint32_t n = (uint32_t)nacc; if (wav_frames + n > wav_max) n = wav_max - wav_frames; fwrite(acc, 4, n, wav); wav_frames += n; wav_hdr(); fflush(wav); }
    if (net_client_connected()) {
        uint32_t hdr[5] = {12 + (uint32_t)nacc * 4, G_AUDIO, AM_RATE, 2, (uint32_t)nacc};
        uint8_t *m = malloc(sizeof hdr + (size_t)nacc * 4); memcpy(m, hdr, sizeof hdr); memcpy(m + sizeof hdr, acc, (size_t)nacc * 4);
        net_send(m, sizeof hdr + (size_t)nacc * 4); free(m); st_sent_frames += (uint64_t)nacc;
    }
    nacc = 0;
}
static void out_push(const float *f, int frames) {
    int16_t tmp[2 * 240]; am_to_s16(f, tmp, 2 * frames);
    for (int i = 0; i < 2 * frames; i++) { float a = fabsf(f[i]); st_sum2 += (double)f[i] * f[i]; if (a > st_peak) st_peak = a; }
    st_frames += (uint64_t)frames; st_total_frames += (uint64_t)frames;
    if (nacc + frames > 4800) out_flush();
    memcpy(acc + 2 * nacc, tmp, (size_t)frames * 4); nacc += frames;
}

void audio_start_testmovie(void);
static void audio_start_testmovie_hook(void) { audio_start_testmovie(); }
static void ds_init_once(void) {
    if (mixer_init) return; mixer_init = 1; am_init(&mixer);
    const char *w = getenv("BLUR_AUDIO_WAV");
    audio_start_testmovie_hook();
    if (w) { wav = fopen(w, "wb+"); wav_max = (uint32_t)(AM_RATE * (getenv("BLUR_AUDIO_WAV_SECS") ? atof(getenv("BLUR_AUDIO_WAV_SECS")) : 20.0)); if (wav) wav_hdr(); }
}

void hle_signal_event(Ctx *c, uint32_t h); extern uint32_t g_api_ret;
static uint32_t buf_bpf(Buf *b) { return b->v.channels * (b->v.bits / 8u); }

// Advance the device clock to "now": mix the elapsed time in 5 ms blocks, fire notifications.
static void ds_advance(Ctx *c) {
    if (!out_started) return;
    uint64_t now = audio_clock_us(); if (now <= t_last) return;
    uint64_t due = (now - t_last) * AM_RATE / 1000000ull;
    if (due > AM_RATE * 3 / 25) { t_last = now - 120000; due = AM_RATE * 3 / 25; }       // the guest stalled (loading): never burst more than 120 ms at the browser       // the guest stalled (loading): do not replay seconds of device time
    while (due >= 240) {
        float out[2 * 240];
        uint32_t before[MAXBUF];
        for (int i = 1; i < nbufs; i++) before[i] = bufs[i].v.pos;
        am_render(&mixer, out, 240); out_push(out, 240);
        t_last += 240ull * 1000000ull / AM_RATE; due -= 240;
        for (int i = 1; i < nbufs; i++) {
            Buf *b = &bufs[i]; if (!b->used || b->primary || !b->nnot) { if (b->used) b->last_pos = b->v.pos; continue; }
            uint32_t p0 = before[i], p1 = b->v.pos; int ended = b->was_playing && !b->v.playing;
            for (int k = 0; k < b->nnot; k++) {
                uint32_t o = b->not_[k].off; int fire = 0;
                if (o == 0xffffffffu) fire = ended;
                else if (b->was_playing) { if (p1 >= p0) fire = o > p0 && o <= p1; else fire = o > p0 || o <= p1; }
                if (fire) { hle_signal_event(c, b->not_[k].ev); c->yield_req = 1; }
            }
            if (b->nnot >= 2 && b->v.looping && b->bytes) {      // streaming ring: consumed data must not replay if the game is late -> silence instead of a stale loop
                uint8_t *d = G + b->data; uint32_t sz = b->v.size, z = bufs[i].v.bits == 8 ? 0x80 : 0;
                if (p1 >= p0) memset(d + p0, (int)z, p1 - p0); else { memset(d + p0, (int)z, sz - p0); memset(d, (int)z, p1); }
            }
            b->was_playing = b->v.playing;
        }
    }
    if (nacc >= 480) out_flush();
    if (dlog_() >= 1 && now - st_last_log >= 1000000) {
        int np = 0, nv = 0; for (int i = 1; i < nbufs; i++) if (bufs[i].used && !bufs[i].primary) { nv++; if (bufs[i].v.playing) np++; }
        double rms = st_frames ? sqrt(st_sum2 / (2.0 * st_frames)) : 0;
        fprintf(stderr, "[audio] t=%.1fs rms=%.4f peak=%.4f frames=%llu buffers=%d playing=%d sent=%llu game_wrote=%lluKB nonzero=%lluKB wpeak=%.3f\n", st_total_frames / (double)AM_RATE, rms, st_peak, (unsigned long long)st_frames, nv, np, (unsigned long long)st_sent_frames, (unsigned long long)(wr_total >> 10), (unsigned long long)(wr_nonzero >> 10), wr_peak);
        st_sum2 = 0; st_peak = 0; st_frames = 0; st_last_log = now;
    }
}
// process exit: flush what is mixed and tell the page to fade out (message 91) so the sound does not end with a crack
void audio_shutdown(void) {
    if (!out_started) return; out_flush();
    if (net_client_connected()) { uint32_t m[2] = {0, 91}; net_send(m, sizeof m); usleep(120000); }      // give the writer thread time before _exit
    if (wav) { wav_hdr(); fclose(wav); wav = NULL; }
}
// called by the dispatcher on (almost) every API call; cheap when nothing is playing
void audio_tick(Ctx *c) {
    static unsigned n; if (!out_started || (++n & 31)) return;
    { static int tn = -1; if (tn < 0) tn = getenv("BLUR_AUDIO_TESTNOISE") != NULL;     // flip the game's own 'Audio.System.TestNoise' tunable (object 0x113eb20: value bytes at +0x24/+0x25)
      if (tn == 1 && st_total_frames > AM_RATE * 3) { G[0x113eb44] = G[0x113eb45] = 1; tn = 2; fprintf(stderr, "[audio] Audio.System.TestNoise forced on\n"); } }
    ds_advance(c);
}

// ---- host-fed stream voices: movie (Bink) audio tracks and the like ---------------------------------------------------------------
// A stream is a ring voice the host fills with int16 interleaved PCM (single producer thread, the mixer is the consumer). See README
// "Bink movie audio" for how BinkSetSoundTrack/BinkDoFrame would drive this.
#define MAXSTREAM 8
static struct { int used; AmVoice v; int16_t *ring; uint32_t frames; } streams[MAXSTREAM];
int audio_stream_open(uint32_t rate, int channels, uint32_t ring_frames) {
    ds_init_once();
    for (int i = 0; i < MAXSTREAM; i++) if (!streams[i].used) {
        memset(&streams[i], 0, sizeof streams[i]); streams[i].used = 1; streams[i].frames = ring_frames ? ring_frames : rate;      // default: 1 s ring
        streams[i].ring = calloc(streams[i].frames * (uint32_t)channels, 2);
        AmVoice *v = &streams[i].v; v->data = (const uint8_t *)streams[i].ring; v->channels = (uint16_t)channels; v->bits = 16; v->rate = rate; v->size = streams[i].frames * (uint32_t)channels * 2;
        v->stream = 1; v->playing = 1; am_add(&mixer, v);
        if (!out_started) { out_started = 1; t_last = audio_clock_us(); st_last_log = t_last; }
        return i;
    }
    return -1;
}
// returns frames accepted (0 when the ring is full: the caller waits and retries)
uint32_t audio_stream_push(int id, const int16_t *pcm, uint32_t frames) {
    if (id < 0 || id >= MAXSTREAM || !streams[id].used) return 0;
    AmVoice *v = &streams[id].v; uint32_t ch = v->channels, cap = streams[id].frames;
    uint64_t wr = v->wr, rd = __atomic_load_n(&v->rd, __ATOMIC_ACQUIRE); uint32_t room = cap - (uint32_t)(wr - rd);
    if (frames > room) frames = room;
    for (uint32_t i = 0; i < frames; i++) memcpy(streams[id].ring + ((wr + i) % cap) * ch, pcm + (size_t)i * ch, ch * 2);
    __atomic_store_n(&v->wr, wr + frames, __ATOMIC_RELEASE); return frames;
}
void audio_stream_volume(int id, int vol_mb) { if (id >= 0 && id < MAXSTREAM && streams[id].used) streams[id].v.vol_mb = vol_mb; }
uint32_t audio_stream_queued(int id) { if (id < 0 || id >= MAXSTREAM || !streams[id].used) return 0; return (uint32_t)(__atomic_load_n(&streams[id].v.wr, __ATOMIC_ACQUIRE) - __atomic_load_n(&streams[id].v.rd, __ATOMIC_ACQUIRE)); }
void audio_stream_close(int id) { if (id < 0 || id >= MAXSTREAM || !streams[id].used) return; am_remove(&mixer, &streams[id].v); streams[id].used = 0; /* ring leaked until process exit: the mixer may still hold it this tick */ }

// BLUR_AUDIO_TESTMOVIE=<movie.bik|wav>[:<stream index>]: decode a movie's audio track with ffmpeg and play it through a stream voice
// (proves the Bink-track routing end to end without a Bink implementation).
#include <pthread.h>
#include <unistd.h>
static void *testmovie_thread(void *arg) {
    char cmd[1024]; snprintf(cmd, sizeof cmd, "ffmpeg -v quiet -i '%s' -map 0:a:%d -f s16le -ar 48000 -ac 2 -", (char *)arg, getenv("BLUR_AUDIO_TESTMOVIE_TRACK") ? atoi(getenv("BLUR_AUDIO_TESTMOVIE_TRACK")) : 0);
    FILE *p = popen(cmd, "r"); if (!p) return NULL; int id = audio_stream_open(48000, 2, 48000);
    fprintf(stderr, "[ds] test movie audio stream %d from %s\n", id, (char *)arg);
    int16_t buf[2 * 1024]; size_t n;
    while ((n = fread(buf, 4, 1024, p)) > 0) { size_t off = 0; while (off < n) { uint32_t k = audio_stream_push(id, buf + 2 * off, (uint32_t)(n - off)); off += k; if (!k) usleep(5000); } }
    while (audio_stream_queued(id)) usleep(10000);
    pclose(p); audio_stream_close(id); return NULL;
}
void audio_start_testmovie(void) {
    const char *m = getenv("BLUR_AUDIO_TESTMOVIE"); static int done; if (!m || done) return; done = 1;
    pthread_t t; pthread_create(&t, NULL, testmovie_thread, strdup(m)); pthread_detach(t);
}

// ---- COM helpers -------------------------------------------------------------------------------------------------------------
static Buf *B(Ctx *c, uint32_t p) { uint32_t i = rd32(c, p + 8); return i && i < (uint32_t)nbufs && bufs[i].used ? &bufs[i] : NULL; }
static const uint8_t IID_Notify[4] = {0x83, 0x07, 0x21, 0xb0}, IID_3DBuf[4] = {0x86, 0xfa, 0x9a, 0x27}, IID_3DList[4] = {0x84, 0xfa, 0x9a, 0x27};
static const uint8_t IID_Buf[4] = {0x85, 0xfa, 0x9a, 0x27}, IID_Buf8[4] = {0x49, 0xa4, 0x25, 0x68}, IID_DS8[4] = {0x93, 0x7e, 0x0a, 0xc5}, IID_Unk[4] = {0, 0, 0, 0};
static uint32_t buf_make(Ctx *c, int idx);
static uint32_t notify_make(Ctx *c, int idx), d3buf_make(Ctx *c, int idx), listener_make(Ctx *c);

static uint32_t buf_qi(Ctx *c, uint32_t *a) {
    uint8_t iid[16]; rdbuf(c, a[1], iid, 16); Buf *b = B(c, a[0]); uint32_t out = 0;
    if (!memcmp(iid, IID_Notify, 4)) { if (b && !b->notify_obj) b->notify_obj = notify_make(c, (int)(b - bufs)); out = b ? b->notify_obj : 0; }
    else if (!memcmp(iid, IID_3DBuf, 4)) { if (b && !b->d3_obj) b->d3_obj = d3buf_make(c, (int)(b - bufs)); if (b) b->v.is3d = 1; out = b ? b->d3_obj : 0; }
    else if (!memcmp(iid, IID_3DList, 4)) { if (!g_listener_obj) g_listener_obj = listener_make(c); out = g_listener_obj; }
    else if (!memcmp(iid, IID_Buf, 4) || !memcmp(iid, IID_Buf8, 4) || !memcmp(iid, IID_Unk, 4) || !memcmp(&iid[4], "\x00\x00\x00\x00\x00\x00\x00\x00\xc0\x00\x00\x00\x00\x00\x00\x46", 12)) out = a[0];
    LOG("Buffer::QueryInterface(%08x, iid=%02x%02x%02x%02x) -> %08x\n", a[0], iid[3], iid[2], iid[1], iid[0], out);
    if (!out) { if (a[2]) wr32(c, a[2], 0); return DSERR_NOINTERFACE; }
    wr32(c, out + 4, rd32(c, out + 4) + 1); if (a[2]) wr32(c, a[2], out); return DS_OK;
}
static uint32_t any_qi(Ctx *c, uint32_t *a) {                     // IDirectSound8 / listener / notify / 3D: every interface is the object itself
    LOG("QueryInterface(%08x)\n", a[0]); if (a[2]) wr32(c, a[2], a[0]); wr32(c, a[0] + 4, rd32(c, a[0] + 4) + 1); return DS_OK;
}
static uint32_t any_addref(Ctx *c, uint32_t *a) { uint32_t n = rd32(c, a[0] + 4) + 1; wr32(c, a[0] + 4, n); return n; }
static uint32_t any_release(Ctx *c, uint32_t *a) { uint32_t n = rd32(c, a[0] + 4); if (n) n--; wr32(c, a[0] + 4, n); return n; }
static void buf_destroy(Ctx *c, Buf *b) {
    LOG("destroy buffer #%d\n", (int)(b - bufs)); am_remove(&mixer, &b->v);
    if (b->data && !b->primary && b->refs == 1 && !getenv("BLUR_AUDIO_NOFREE")) guest_free(c, b->data);
    b->used = 0;
}
static uint32_t buf_addref(Ctx *c, uint32_t *a) { return any_addref(c, a); }
static uint32_t buf_release(Ctx *c, uint32_t *a) {
    uint32_t n = rd32(c, a[0] + 4); if (n) n--; wr32(c, a[0] + 4, n); Buf *b = B(c, a[0]);
    LOG("Buffer::Release(%08x) -> %u\n", a[0], n); if (!n && b) buf_destroy(c, b); return n;
}

// ---- IDirectSoundBuffer8 ------------------------------------------------------------------------------------------------------
static void set_format(Buf *b, const uint8_t *w, uint32_t n) {
    if (n > sizeof b->wfx) n = sizeof b->wfx; memcpy(b->wfx, w, n); b->wfx_size = n;
    uint16_t tag = w[0] | w[1] << 8; b->v.channels = w[2] | w[3] << 8; b->v.rate = w[4] | w[5] << 8 | w[6] << 16 | (uint32_t)w[7] << 24; b->v.bits = w[14] | w[15] << 8;
    b->v.is_float = tag == 3;
    if (tag == 0xfffe && n >= 40) b->v.is_float = (w[24] | w[25] << 8) == 3;            // WAVEFORMATEXTENSIBLE: SubFormat GUID data1
    if (!b->v.channels) b->v.channels = 2; if (!b->v.bits) b->v.bits = 16; if (!b->v.rate) b->v.rate = 44100;
}
static uint32_t pos_play(Buf *b) { return b->v.pos; }
static uint32_t pos_write(Buf *b) {
    if (!b->v.playing) return b->v.pos;
    uint32_t bpf = buf_bpf(b), lead = (uint32_t)((uint64_t)b->v.rate * 15 / 1000) * bpf;      // write cursor ~15 ms ahead of the play cursor
    return b->bytes ? (b->v.pos + lead) % b->bytes : 0;
}
static uint32_t b_getcaps(Ctx *c, uint32_t *a) { Buf *b = B(c, a[0]); if (!b || !a[1]) return DSERR_INVALIDPARAM; wr32(c, a[1], 20); wr32(c, a[1] + 4, b->flags); wr32(c, a[1] + 8, b->bytes); wr32(c, a[1] + 12, 0); wr32(c, a[1] + 16, 0); return DS_OK; }
static uint32_t b_getcur(Ctx *c, uint32_t *a) {
    Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; ds_advance(c);
    // The game's device thread polls GetCurrentPosition + SwitchToThread in a tight loop until its ring has room (every poll is a baton switch and a few
    // hundred thousand API calls per minute). A real driver would wake it at the next notification position: when the same position is seen twice in a
    // row, put the caller to sleep until the cursor reaches the next notify offset (BLUR_AUDIO_NOSLEEP=1 restores the busy poll).
    { static int nosleep = -1; if (nosleep < 0) nosleep = getenv("BLUR_AUDIO_NOSLEEP") != NULL; Thread *t = cur_thread(c);
      if (!nosleep && b->nnot >= 2 && b->v.playing && !b->primary) {
        if (t->wake_at) { if (now_us() < t->wake_at) { c->block = 1; return 0; } t->wake_at = 0; b->poll_same = 0; }
        else { uint32_t p0 = pos_play(b); if (p0 == b->poll_pos) b->poll_same++; else { b->poll_same = 0; b->poll_pos = p0; }
          if (b->poll_same >= 2) {
            uint32_t step = b->not_[1].off > b->not_[0].off ? b->not_[1].off - b->not_[0].off : 512, bpf = buf_bpf(b) ? buf_bpf(b) : 4, next = (p0 / step + 1) * step - p0;
            uint64_t us = (uint64_t)next / bpf * 1000000ull / (b->v.rate ? b->v.rate : 24000); if (us < 500) us = 500; t->wake_at = now_us() + us; c->block = 1; return 0; } } } }
    uint32_t p = pos_play(b), w = pos_write(b); if (a[1]) wr32(c, a[1], p); if (a[2]) wr32(c, a[2], w);
    LOG("GetCurrentPosition(#%d) play=%u write=%u (playing=%d)\n", (int)(b - bufs), p, w, b->v.playing); return DS_OK;
}
static uint32_t b_getformat(Ctx *c, uint32_t *a) {
    Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM;
    if (a[1]) { uint32_t n = a[2] < b->wfx_size ? a[2] : b->wfx_size; wrbuf(c, a[1], b->wfx, n); } if (a[3]) wr32(c, a[3], b->wfx_size); return DS_OK;
}
static uint32_t b_getvol(Ctx *c, uint32_t *a) { Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; wr32(c, a[1], (uint32_t)b->v.vol_mb); return DS_OK; }
static uint32_t b_getpan(Ctx *c, uint32_t *a) { Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; wr32(c, a[1], (uint32_t)b->v.pan_mb); return DS_OK; }
static uint32_t b_getfreq(Ctx *c, uint32_t *a) { Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; wr32(c, a[1], b->v.freq ? b->v.freq : b->v.rate); return DS_OK; }
static uint32_t b_getstatus(Ctx *c, uint32_t *a) {
    Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; ds_advance(c);
    wr32(c, a[1], (b->v.playing ? 1u : 0u) | (b->v.playing && b->v.looping ? 4u : 0u) | (b->primary ? 0 : 0)); return DS_OK;
}
static uint32_t b_lock(Ctx *c, uint32_t *a) {   // this, offset, bytes, pp1, pb1, pp2, pb2, flags
    Buf *b = B(c, a[0]); if (!b || !b->bytes) return DSERR_INVALIDPARAM;
    uint32_t off = a[1], n = a[2], fl = a[7];
    if (fl & 1) { ds_advance(c); off = pos_write(b); }
    if (fl & 2) { off = 0; n = b->bytes; }
    if (off >= b->bytes || n > b->bytes) { LOG("Lock(#%d) invalid off=%u n=%u\n", (int)(b - bufs), off, n); return DSERR_INVALIDPARAM; }
    uint32_t n1 = n, n2 = 0; if (off + n > b->bytes) { n1 = b->bytes - off; n2 = n - n1; }
    wr32(c, a[3], b->data + off); if (a[4]) wr32(c, a[4], n1);
    if (a[5]) wr32(c, a[5], n2 ? b->data : 0); if (a[6]) wr32(c, a[6], n2);
    b->lock_bytes += n;
    { static int nlog; if (dlog_() >= 1 && nlog++ < 3) fprintf(stderr, "[ds] Lock caller=%08x thread=%d\n", g_api_ret, c->cur); }
    LOG("Lock(#%d, off=%u n=%u flags=%x) -> %u+%u\n", (int)(b - bufs), off, n, fl, n1, n2); return DS_OK;
}
static uint32_t b_play(Ctx *c, uint32_t *a) {   // this, reserved, priority, flags
    Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; ds_advance(c);
    if (!b->primary) {
        if (!(a[3] & 1) && 0) return DSERR_INVALIDPARAM;
        b->v.looping = a[3] & 1; b->v.playing = 1; b->was_playing = 1; b->play_calls++;
        if (!out_started) { out_started = 1; t_last = audio_clock_us(); st_last_log = t_last; }
    }
    if (dlog_() >= 1) fprintf(stderr, "[ds] Play(#%d, flags=%x) caller=%08x thread=%d\n", (int)(b - bufs), a[3], g_api_ret, c->cur); return DS_OK;
}
static uint32_t b_setcur(Ctx *c, uint32_t *a) { Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; uint32_t bpf = buf_bpf(b); b->v.pos = bpf ? a[1] / bpf * bpf : 0; b->v.frac = 0; if (b->v.pos >= b->bytes) b->v.pos = 0; LOG("SetCurrentPosition(#%d, %u)\n", (int)(b - bufs), a[1]); return DS_OK; }
static uint32_t b_setformat(Ctx *c, uint32_t *a) { Buf *b = B(c, a[0]); if (!b || !a[1]) return DSERR_INVALIDPARAM; uint8_t w[40]; rdbuf(c, a[1], w, 40); LOG("SetFormat(#%d)\n", (int)(b - bufs)); if (b->primary) set_format(b, w, 18 + (w[16] | w[17] << 8)); return DS_OK; }
static uint32_t b_setvol(Ctx *c, uint32_t *a) { Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; int v = (int)a[1]; if (v > 0 || v < -10000) return DSERR_INVALIDPARAM; b->v.vol_mb = v; return DS_OK; }
static uint32_t b_setpan(Ctx *c, uint32_t *a) { Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; b->v.pan_mb = (int)a[1]; return DS_OK; }
static uint32_t b_setfreq(Ctx *c, uint32_t *a) { Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; b->v.freq = a[1] == 0 ? 0 : a[1]; return DS_OK; }
static uint32_t b_stop(Ctx *c, uint32_t *a) { if (dlog_() >= 1) fprintf(stderr, "[ds] Stop(%08x) caller=%08x\n", a[0], g_api_ret); Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; ds_advance(c); if (!b->primary) { b->v.playing = 0; } LOG("Stop(#%d)\n", (int)(b - bufs)); return DS_OK; }
// what the game wrote into DirectSound buffers (diagnostics)
static void scan_written(Buf *b, uint32_t p, uint32_t n) {
    if (!p || !n) return; const uint8_t *d = GP(p); int nz = 0; float pk = 0;
    if (b->v.is_float && b->v.bits == 32) { for (uint32_t i = 0; i + 4 <= n; i += 4) { float f; memcpy(&f, d + i, 4); if (f != 0) nz = 1; if (fabsf(f) > pk) pk = fabsf(f); } }
    else if (b->v.bits == 16) { for (uint32_t i = 0; i + 2 <= n; i += 2) { int s = (int16_t)(d[i] | d[i + 1] << 8); if (s) nz = 1; if (abs(s) / 32768.0f > pk) pk = abs(s) / 32768.0f; } }
    wr_total += n; if (nz) wr_nonzero += n; if (pk > wr_peak) wr_peak = pk;
}
// BLUR_AUDIO_SELFTEST=1: mix a synthetic signal into whatever the game writes to its output buffer (the showroom engine writes digital silence),
// to exercise the device path (cursor math, mixer, WAV, WebSocket, AudioWorklet) with a known spectrum: 440 Hz tone + 110 Hz beat + 2.2 kHz blips.
static void selftest_mix(Buf *b, uint32_t p, uint32_t n) {
    static uint64_t frame; if (!p || !n || b->v.bits != 16) return;
    int16_t *s = (int16_t *)GP(p); uint32_t ch = b->v.channels, frames = n / (2 * ch);
    for (uint32_t i = 0; i < frames; i++, frame++) {
        double t = (double)frame / b->v.rate, env = (frame / (b->v.rate / 4)) % 4 == 3 ? 1.0 : 0.0;
        double v = 0.20 * sin(6.283185307 * 440 * t) + 0.15 * sin(6.283185307 * 110 * t) * (0.5 + 0.5 * sin(6.283185307 * 2 * t)) + 0.10 * env * sin(6.283185307 * 2200 * t);
        for (uint32_t k = 0; k < ch; k++) { int x = s[i * ch + k] + (int)(v * 32767.0 * (k == 0 ? 1.0 : 0.8)); s[i * ch + k] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }
    }
}
static uint32_t b_unlock(Ctx *c, uint32_t *a) {      // this, p1, n1, p2, n2
    Buf *b = B(c, a[0]);
    { static int st = -1; if (st < 0) st = getenv("BLUR_AUDIO_SELFTEST") != NULL; if (st && b && b == &bufs[1]) { selftest_mix(b, a[1], a[2]); selftest_mix(b, a[3], a[4]); } }
    if (b) { scan_written(b, a[1], a[2]); scan_written(b, a[3], a[4]); }
    LOG("Unlock(#%d)\n", b ? (int)(b - bufs) : -1); return DS_OK;
}
static uint32_t b_dsinit(Ctx *c, uint32_t *a) { (void)c; (void)a; return 0x88780032u; }      // DSERR_ALREADYINITIALIZED
static uint32_t b_getobj(Ctx *c, uint32_t *a) { (void)c; (void)a; return DSERR_UNSUPPORTED; }
static uint32_t b_restore(Ctx *c, uint32_t *a) { (void)c; (void)a; return DS_OK; }
static uint32_t b_setfx(Ctx *c, uint32_t *a) { (void)c; if (a[1] == 0) return DS_OK; return DSERR_UNSUPPORTED; }

#define IUN { "QueryInterface", 3, buf_qi }, { "AddRef", 1, buf_addref }, { "Release", 1, buf_release }
static const ComMethod buf_m[] = {
    IUN, { "GetCaps", 2, b_getcaps }, { "GetCurrentPosition", 3, b_getcur }, { "GetFormat", 4, b_getformat }, { "GetVolume", 2, b_getvol }, { "GetPan", 2, b_getpan },
    { "GetFrequency", 2, b_getfreq }, { "GetStatus", 2, b_getstatus }, { "Initialize", 3, b_dsinit }, { "Lock", 8, b_lock }, { "Play", 4, b_play },
    { "SetCurrentPosition", 2, b_setcur }, { "SetFormat", 2, b_setformat }, { "SetVolume", 2, b_setvol }, { "SetPan", 2, b_setpan }, { "SetFrequency", 2, b_setfreq },
    { "Stop", 1, b_stop }, { "Unlock", 5, b_unlock }, { "Restore", 1, b_restore },
    { "SetFX", 4, b_setfx }, { "AcquireResources", 5, b_getobj }, { "GetObjectInPath", 5, b_getobj },
};
static uint32_t buf_make(Ctx *c, int idx) {
    uint32_t o = com_make(c, "IDirectSoundBuffer8", buf_m, sizeof buf_m / sizeof buf_m[0], 8); wr32(c, o + 8, (uint32_t)idx); return o;
}

// ---- IDirectSoundNotify -------------------------------------------------------------------------------------------------------
static uint32_t n_set(Ctx *c, uint32_t *a) {      // this, count, DSBPOSITIONNOTIFY*
    Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; b->nnot = 0;
    for (uint32_t i = 0; i < a[1] && b->nnot < MAXNOT; i++) { b->not_[b->nnot].off = rd32(c, a[2] + 8 * i); b->not_[b->nnot].ev = rd32(c, a[2] + 8 * i + 4); LOG("Notify(#%d)[%u] off=%u event=%08x\n", (int)(b - bufs), i, b->not_[b->nnot].off, b->not_[b->nnot].ev); b->nnot++; }
    return DS_OK;
}
static const ComMethod notify_m[] = { { "QueryInterface", 3, any_qi }, { "AddRef", 1, any_addref }, { "Release", 1, any_release }, { "SetNotificationPositions", 3, n_set } };
static uint32_t notify_make(Ctx *c, int idx) { uint32_t o = com_make(c, "IDirectSoundNotify", notify_m, 4, 8); wr32(c, o + 8, (uint32_t)idx); return o; }

// ---- IDirectSound3DBuffer / listener (positions only influence pan + attenuation) --------------------------------------------------
static uint32_t d3_setpos(Ctx *c, uint32_t *a) { Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; float f[3]; for (int i = 0; i < 3; i++) memcpy(&f[i], &a[1 + i], 4); b->v.x = f[0]; b->v.y = f[1]; b->v.z = f[2]; return DS_OK; }
static uint32_t d3_setmin(Ctx *c, uint32_t *a) { Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; memcpy(&b->v.min_dist, &a[1], 4); return DS_OK; }
static uint32_t d3_setmax(Ctx *c, uint32_t *a) { Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; memcpy(&b->v.max_dist, &a[1], 4); return DS_OK; }
static uint32_t d3_setall(Ctx *c, uint32_t *a) { Buf *b = B(c, a[0]); if (!b || !a[1]) return DSERR_INVALIDPARAM; memcpy(&b->v.x, G + a[1] + 4, 12); memcpy(&b->v.max_dist, G + a[1] + 40, 4); memcpy(&b->v.min_dist, G + a[1] + 44, 4); return DS_OK; }
static uint32_t d3_getpos(Ctx *c, uint32_t *a) { Buf *b = B(c, a[0]); if (!b) return DSERR_INVALIDPARAM; memcpy(G + a[1], &b->v.x, 12); return DS_OK; }
static uint32_t d3_ok(Ctx *c, uint32_t *a) { (void)c; (void)a; return DS_OK; }
#define OKM(n, na) { n, na, d3_ok }
static const ComMethod d3buf_m[] = { { "QueryInterface", 3, any_qi }, { "AddRef", 1, any_addref }, { "Release", 1, any_release },
    { "GetAllParameters", 2, d3_ok }, OKM("GetConeAngles", 3), OKM("GetConeOrientation", 2), OKM("GetConeOutsideVolume", 2), OKM("GetMaxDistance", 2), OKM("GetMinDistance", 2),
    OKM("GetMode", 2), { "GetPosition", 2, d3_getpos }, OKM("GetVelocity", 2), { "SetAllParameters", 3, d3_setall }, OKM("SetConeAngles", 4), OKM("SetConeOrientation", 5),
    OKM("SetConeOutsideVolume", 3), { "SetMaxDistance", 3, d3_setmax }, { "SetMinDistance", 3, d3_setmin }, OKM("SetMode", 3), { "SetPosition", 5, d3_setpos }, OKM("SetVelocity", 5) };
static uint32_t d3buf_make(Ctx *c, int idx) { uint32_t o = com_make(c, "IDirectSound3DBuffer", d3buf_m, sizeof d3buf_m / sizeof d3buf_m[0], 8); wr32(c, o + 8, (uint32_t)idx); return o; }
static const ComMethod list_m[] = { { "QueryInterface", 3, any_qi }, { "AddRef", 1, any_addref }, { "Release", 1, any_release },
    OKM("GetAllParameters", 2), OKM("GetDistanceFactor", 2), OKM("GetDopplerFactor", 2), OKM("GetOrientation", 3), OKM("GetPosition", 2), OKM("GetRolloffFactor", 2), OKM("GetVelocity", 2),
    OKM("SetAllParameters", 3), OKM("SetDistanceFactor", 3), OKM("SetDopplerFactor", 3), OKM("SetOrientation", 8), OKM("SetPosition", 5), OKM("SetRolloffFactor", 3), OKM("SetVelocity", 5), OKM("CommitDeferredSettings", 1) };
static uint32_t listener_make(Ctx *c) { return com_make(c, "IDirectSound3DListener", list_m, sizeof list_m / sizeof list_m[0], 0); }

// ---- IDirectSound8 --------------------------------------------------------------------------------------------------------------
static int new_buf(void) { for (int i = 1; i < nbufs; i++) if (!bufs[i].used) return i; if (nbufs >= MAXBUF) return -1; return nbufs++; }
static uint32_t ds_createbuf(Ctx *c, uint32_t *a) {      // this, DSBUFFERDESC*, ppBuf, pUnkOuter
    if (!a[1] || !a[2]) return DSERR_INVALIDPARAM; ds_init_once();
    uint32_t dsz = rd32(c, a[1]), fl = rd32(c, a[1] + 4), bytes = rd32(c, a[1] + 8), pw = rd32(c, a[1] + 16);
    int i = new_buf(); if (i < 0) return 0x8007000e;
    Buf *b = &bufs[i]; memset(b, 0, sizeof *b); b->used = 1; b->flags = fl; b->bytes = bytes; b->refs = 1; b->primary = (fl & DSBCAPS_PRIMARY) != 0; b->v.vol_mb = 0;
    if (pw) { uint8_t w[40] = {0}; rdbuf(c, pw, w, 40); set_format(b, w, 18 + (w[16] | w[17] << 8)); }
    else { uint8_t w[18] = {1, 0, 2, 0, 0x44, 0xac, 0, 0, 0x10, 0xb1, 2, 0, 4, 0, 16, 0, 0, 0}; set_format(b, w, 18); }       // primary default: 44.1k/16/stereo
    if (!b->primary) {
        if (!bytes) { b->used = 0; return DSERR_INVALIDPARAM; }
        b->data = guest_alloc(c, bytes); memset(GP(b->data), b->v.bits == 8 ? 0x80 : 0, bytes);
        b->v.data = GP(b->data); b->v.size = bytes - bytes % (buf_bpf(b) ? buf_bpf(b) : 1); b->mixer_slot = am_add(&mixer, &b->v);
    }
    b->v.is3d = 0; b->guest = buf_make(c, i); wr32(c, a[2], b->guest);
    fprintf(stderr, "[ds] CreateSoundBuffer #%d flags=%08x bytes=%u %s fmt: %uch %uHz %ubit%s -> %08x\n", i, fl, bytes, b->primary ? "PRIMARY" : "secondary", b->v.channels, b->v.rate, b->v.bits, b->v.is_float ? " float" : "", b->guest);
    (void)dsz; return DS_OK;
}
static uint32_t ds_getcaps(Ctx *c, uint32_t *a) {
    if (!a[1]) return DSERR_INVALIDPARAM; uint32_t sz = rd32(c, a[1]); if (sz < 96) sz = 96;
    uint8_t z[96] = {0}; wrbuf(c, a[1], z, 96); wr32(c, a[1], sz);
    wr32(c, a[1] + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800);   // PRIMARYMONO..SECONDARY16BIT, CONTINUOUSRATE etc.
    wr32(c, a[1] + 8, 100); wr32(c, a[1] + 12, 200000); for (int k = 4; k < 9; k++) wr32(c, a[1] + 8 + 4 * k, 0);
    wr32(c, a[1] + 28, 32); wr32(c, a[1] + 32, 32); wr32(c, a[1] + 36, 32); wr32(c, a[1] + 40, 32); wr32(c, a[1] + 44, 32); wr32(c, a[1] + 48, 32); wr32(c, a[1] + 52, 32);
    wr32(c, a[1] + 56, 0x1000000); wr32(c, a[1] + 60, 0x1000000);
    return DS_OK;
}
static uint32_t ds_dup(Ctx *c, uint32_t *a) {      // this, src, ppDst: share the storage
    Buf *s = B(c, a[1]); if (!s || !a[2]) return DSERR_INVALIDPARAM;
    int i = new_buf(); if (i < 0) return 0x8007000e; Buf *b = &bufs[i]; s = B(c, a[1]); *b = *s; b->used = 1; b->refs = 2; s->refs++; b->notify_obj = b->d3_obj = 0; b->nnot = 0; b->v.playing = 0; b->v.pos = 0;
    b->mixer_slot = am_add(&mixer, &b->v); b->guest = buf_make(c, i); wr32(c, a[2], b->guest); LOG("DuplicateSoundBuffer #%d -> #%d\n", (int)(s - bufs), i); return DS_OK;
}
static uint32_t ds_coop(Ctx *c, uint32_t *a) { (void)c; LOG("SetCooperativeLevel(hwnd=%08x level=%u)\n", a[1], a[2]); return DS_OK; }
static uint32_t ds_speaker(Ctx *c, uint32_t *a) { wr32(c, a[1], 4); return DS_OK; }     // DSSPEAKER_STEREO
static uint32_t ds_ok(Ctx *c, uint32_t *a) { (void)c; (void)a; return DS_OK; }
static uint32_t ds_verify(Ctx *c, uint32_t *a) { if (a[1]) wr32(c, a[1], 0); return DS_OK; }   // certified
static const ComMethod ds_m[] = { { "QueryInterface", 3, any_qi }, { "AddRef", 1, any_addref }, { "Release", 1, any_release },
    { "CreateSoundBuffer", 4, ds_createbuf }, { "GetCaps", 2, ds_getcaps }, { "DuplicateSoundBuffer", 3, ds_dup }, { "SetCooperativeLevel", 3, ds_coop }, { "Compact", 1, ds_ok },
    { "GetSpeakerConfig", 2, ds_speaker }, { "SetSpeakerConfig", 2, ds_ok }, { "Initialize", 2, ds_ok }, { "VerifyCertification", 2, ds_verify } };

// DirectSoundCreate / DirectSoundCreate8(lpGuid, ppDS, pUnkOuter)
uint32_t h_DirectSoundCreate8(Ctx *c, uint32_t *a) {
    if (getenv("BLUR_AUDIO_OFF")) return 0x80004005u;
    ds_init_once();
    if (!g_ds_obj) g_ds_obj = com_make(c, "IDirectSound8", ds_m, sizeof ds_m / sizeof ds_m[0], 0);
    fprintf(stderr, "[ds] DirectSoundCreate8(guid=%08x) -> %08x (caller %08x)\n", a[0], g_ds_obj, g_api_ret);
    if (a[1]) wr32(c, a[1], g_ds_obj); return DS_OK;
}
// DirectSoundEnumerateA(callback, ctx): report the default device once
uint32_t h_DirectSoundEnumerateA(Ctx *c, uint32_t *a) {
    if (getenv("BLUR_AUDIO_OFF")) return 0x80004005u;
    static uint32_t desc, mod; if (!desc) { desc = guest_alloc(c, 64); mod = desc + 32; strcpy((char *)GP(desc), "Primary Sound Driver"); strcpy((char *)GP(mod), ""); }
    fprintf(stderr, "[ds] DirectSoundEnumerateA(cb=%08x ctx=%08x)\n", a[0], a[1]);
    if (a[0]) { c->req.pending = 1; c->req.fn = a[0]; c->req.nargs = 4; c->req.args[0] = 0; c->req.args[1] = desc; c->req.args[2] = mod; c->req.args[3] = a[1]; c->req.have_cont = 1; c->req.use_result = 0; c->req.cont_value = DS_OK; }
    return DS_OK;
}
