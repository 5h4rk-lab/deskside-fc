// Short synthesized air-horn. Off by default; only runs after the visitor
// turns sound on and then presses the goal button (a user gesture).
let ctx = null;

export function playHorn() {
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) return;
  try {
    ctx = ctx || new AC();
    if (ctx.state === "suspended") ctx.resume();
    const t = ctx.currentTime;
    const out = ctx.createGain();
    out.gain.setValueAtTime(0.0001, t);
    out.gain.exponentialRampToValueAtTime(0.28, t + 0.04);
    out.gain.setValueAtTime(0.28, t + 0.7);
    out.gain.exponentialRampToValueAtTime(0.0001, t + 1.15);
    const lp = ctx.createBiquadFilter();
    lp.type = "lowpass";
    lp.frequency.value = 2400;
    out.connect(lp).connect(ctx.destination);
    [[311, 0], [392, 0], [466, 0], [313, 3]].forEach(([f, det]) => {
      const o = ctx.createOscillator();
      o.type = "sawtooth";
      o.frequency.value = f;
      o.detune.value = det;
      o.connect(out);
      o.start(t);
      o.stop(t + 1.2);
    });
    // crowd swell: filtered noise
    const len = ctx.sampleRate * 2;
    const buf = ctx.createBuffer(1, len, ctx.sampleRate);
    const d = buf.getChannelData(0);
    for (let i = 0; i < len; i++) d[i] = Math.random() * 2 - 1;
    const n = ctx.createBufferSource();
    n.buffer = buf;
    const bp = ctx.createBiquadFilter();
    bp.type = "bandpass";
    bp.frequency.value = 900;
    const ng = ctx.createGain();
    ng.gain.setValueAtTime(0.0001, t);
    ng.gain.linearRampToValueAtTime(0.09, t + 1.2);
    ng.gain.linearRampToValueAtTime(0.0001, t + 2.4);
    n.connect(bp).connect(ng).connect(ctx.destination);
    n.start(t);
    n.stop(t + 2.4);
  } catch (e) {
    /* audio is optional */
  }
}
