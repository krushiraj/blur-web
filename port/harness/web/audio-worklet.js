// AudioWorklet that plays the host's mixed 48 kHz stereo output (see audio.c / audio_mix.c).
// Two input modes, same processor:
//  * message mode (native harness + WebSocket): main thread posts {pcm: ArrayBuffer(int16 interleaved stereo), rate} chunks into a 2 s ring;
//  * shared-ring mode (wasm build): processorOptions.ring = {sab, frames} -- a SharedArrayBuffer laid out as
//      Int32[0] = write index (frames, monotonically increasing, written by the producer with Atomics.store)
//      Int32[1] = read index  (frames, written here)
//      Float32[4..] = interleaved stereo ring of `frames` frames.  Producer (the wasm mixer) must never write more than
//      `frames` ahead of the read index; this processor never blocks.
//
// Click-free behaviour (the output is never cut hard):
//  * one per-sample gain with ramps: start/resume fade-in 10 ms, running dry fade-out 5 ms (it starts BEFORE the last samples are consumed), stop fade-out 30 ms;
//  * 'stop' message (page hide / unload / socket close / host exit) -> fade to silence, stays silent until 'resume';
//  * after an underrun the stream is re-primed (100 ms) before it fades back in;
//  * drift/backlog control without dropping audio: the read speed is nudged by up to +5 % / -2 % towards a ~120 ms buffer (linear interpolation);
//    only a backlog above 1 s (host stalled and then burst) is cut, and that cut happens while the gain is faded to zero.
class BlurPcm extends AudioWorkletProcessor {
  constructor(opts) {
    super();
    const o = (opts && opts.processorOptions) || {};
    this.srcRate = 48000; this.base = this.srcRate / sampleRate;
    this.cap = 96000; this.ring = new Float32Array(this.cap * 2); this.wpos = 0; this.rpos = 0;     // message mode ring (frames, monotonically increasing positions)
    this.prime = Math.round(0.10 * this.srcRate); this.target = Math.round(0.12 * this.srcRate); this.maxBack = this.srcRate;      // 100 ms prime, 120 ms target, 1 s hard limit
    this.dryFade = 360;                                           // start fading out when fewer than this many frames (7.5 ms) are buffered
    this.gain = 0; this.playing = false; this.stopped = false; this.jumping = false;
    this.underruns = 0; this.dropped = 0; this.played = 0; this.received = 0; this.tick = 0; this.speed = 1;
    if (o.ring) { this.sab = { i32: new Int32Array(o.ring.sab, 0, 4), f32: new Float32Array(o.ring.sab, 16), frames: o.ring.frames }; this.rpos = Atomics.load(this.sab.i32, 1); }
    this.port.onmessage = (e) => {
      const m = e.data;
      if (m.pcm) this.push(m);
      else if (m.stop) this.stopped = true;
      else if (m.resume) { this.stopped = false; }
      else if (m.stats) this.port.postMessage({ stats: this.stat() });
    };
  }
  buffered() { return this.sab ? (Atomics.load(this.sab.i32, 0) - this.rpos) | 0 : this.wpos - this.rpos; }
  stat() { return { buffered: Math.max(0, Math.round(this.buffered())), played: this.played, underruns: this.underruns, dropped: this.dropped, received: this.received, started: this.playing, gain: this.gain, speed: this.speed }; }
  push(m) {
    if (m.rate && m.rate !== this.srcRate) { this.srcRate = m.rate; this.base = this.srcRate / sampleRate; }
    const s = new Int16Array(m.pcm), n = s.length >> 1; this.received += n;
    for (let i = 0; i < n; i++) { const k = ((this.wpos + i) % this.cap) * 2; this.ring[k] = s[2 * i] / 32768; this.ring[k + 1] = s[2 * i + 1] / 32768; }
    this.wpos += n;
    if (this.wpos - this.rpos > this.cap - 4800) { const cut = this.wpos - this.rpos - (this.cap - 4800); this.rpos += cut; this.dropped += cut; this.gain = 0; this.playing = false; }   // ring overflow (should not happen: >1.9 s backlog)
  }
  process(inputs, outputs) {
    const out = outputs[0], L = out[0], R = out[1] || out[0], n = L.length;
    const sab = this.sab; let buf = this.buffered();
    if (sab) { const wr = Atomics.load(sab.i32, 0); if (((wr - this.rpos) | 0) > sab.frames - 480) this.rpos = (wr - this.target) | 0; }
    // state machine
    if (!this.playing) { if (!this.stopped && buf >= this.prime) { this.playing = true; this.jumping = false; } }
    let speed = 1;
    if (this.playing) {
      const corr = Math.max(-0.02, Math.min(0.05, (buf - this.target) / this.srcRate));
      speed = 1 + corr; this.speed = speed;
    }
    for (let i = 0; i < n; i++) {
      buf = this.buffered();
      let tg = 1, inc = 1 / 480;                                         // fade-in 10 ms
      if (!this.playing || this.stopped) { tg = 0; inc = this.stopped ? 1 / 1440 : 1 / 240; }
      else if (buf < this.dryFade) { tg = 0; inc = 1 / 240; }              // running dry: fade out over 5 ms instead of cutting
      else if (buf > this.maxBack) { tg = 0; inc = 1 / 96; this.jumping = true; }   // huge backlog: fade out fast, jump, fade back in
      if (this.gain < tg) this.gain = Math.min(tg, this.gain + inc); else if (this.gain > tg) this.gain = Math.max(tg, this.gain - inc);
      if (this.jumping && this.gain <= 0.0005) { const skip = (this.sab ? 0 : this.wpos - this.rpos - this.target); if (skip > 0) { this.rpos += skip; this.dropped += skip; } this.jumping = false; }
      if (this.playing && buf < 2) { this.playing = false; this.underruns++; this.gain = 0; }
      let l = 0, r = 0;
      if (this.playing && this.gain > 0) {
        const p = Math.floor(this.rpos), f = this.rpos - p;
        if (sab) { const c = sab.frames, k0 = (p % c + c) % c * 2, k1 = ((p + 1) % c) * 2; l = sab.f32[k0] + (sab.f32[k1] - sab.f32[k0]) * f; r = sab.f32[k0 + 1] + (sab.f32[k1 + 1] - sab.f32[k0 + 1]) * f; }
        else { const k0 = (p % this.cap) * 2, k1 = ((p + 1) % this.cap) * 2; l = this.ring[k0] + (this.ring[k1] - this.ring[k0]) * f; r = this.ring[k0 + 1] + (this.ring[k1 + 1] - this.ring[k0 + 1]) * f; }
        l *= this.gain; r *= this.gain; this.played++;
      }
      if (this.playing) { this.rpos += this.base * speed; }
      L[i] = l; R[i] = r;
    }
    if (sab) Atomics.store(sab.i32, 1, Math.floor(this.rpos) | 0);
    if ((++this.tick & 63) === 0) this.port.postMessage({ stats: this.stat() });
    return true;
  }
}
registerProcessor('blur-pcm', BlurPcm);
