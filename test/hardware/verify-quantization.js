#!/usr/bin/env node
// Scripted, byte-level reproduction of aloop's quantization behavior, built
// to verify the loop-length fixes from a real live-hardware session without
// needing a human to press pads (see midi-inject.js's own header comment
// and gm's skill guidance on preferring byte-level injection over asking a
// human to reproduce input).
//
// What this verifies against the CONFIRMED spec (this session's own grilling):
//   - Loop 1 (first recording on a clear rig): its FINAL length must equal
//     its raw played duration EXACTLY (within one block's worth of sample
//     accuracy) -- no musical/tempo snapping at all. This is what sets the
//     shared master phrase M (and separately proposes an Ableton Link tempo
//     derived FROM that exact length, never the reverse).
//   - Loop 2+ (every subsequent recording): its final length must snap to
//     the NEAREST power-of-2 candidate in {M/16, M/8, M/4, M/2, M, 2M, 4M,
//     8M, ...} (floored at M/16), using a 68% extend-vs-trim threshold
//     between the bracketing pair.
//
// Reads back the ACTUAL latched length via udp/4445's new "wraplen" field
// (src/control/telemetry.cpp), not by ear -- see dsp/loop.dsp's "wraplen"
// hbargraph comment for why this zone was added.
//
// Usage:
//   node verify-quantization.js <host> [holdMs1] [holdMs2] [holdMs3...]
//   e.g. node verify-quantization.js 192.168.137.100 2000 600 8100
//        (records loop 0 for ~2s, loop 1 for ~0.6s, loop 2 for ~8.1s)
//
// Requires the device to already be on a CLEAR rig (no existing loop
// content) before running -- this script does not clear/erase anything
// itself, to avoid accidentally wiping real work; run cmd/clearall via the
// SHIFT+STOP_ALL note sequence yourself first if needed.

const net = require('net');
const dgram = require('dgram');

const [, , host, ...holdArgs] = process.argv;
if (!host) {
  console.error('usage: node verify-quantization.js <host> [holdMs...]');
  process.exit(2);
}
const holds = (holdArgs.length ? holdArgs : ['2000', '600', '8100']).map(Number);

function sendBytes(bytes) {
  return new Promise((resolve, reject) => {
    const sock = net.connect({ host, port: 9401 }, () => {
      sock.write(Buffer.from(bytes), (err) => {
        if (err) return reject(err);
        sock.end();
      });
    });
    sock.on('close', resolve);
    sock.on('error', reject);
  });
}

function padNote(looperIndex) {
  // gridLooperIndex's inverse: row*4+(col-2) = looperIndex, row=0 for the
  // first 4 loopers -- note = row*8+col. For looperIndex 0..3, row=0,
  // col=2..5, so note = 2..5. This only covers the first row (loopers 0-3);
  // extend with the real row/col math (apc_grid.h's gridLooperIndex) if a
  // test ever needs looper 4+.
  if (looperIndex < 0 || looperIndex > 3) {
    throw new Error(`padNote only covers loopers 0-3 (row 0); got ${looperIndex}`);
  }
  const row = 0, col = looperIndex + 2;
  return row * 8 + col;
}

async function pressPad(note) {
  await sendBytes([0x90, note, 127]);
}
async function releasePad(note) {
  await sendBytes([0x80, note, 0]);
}

function queryTelemetry() {
  return new Promise((resolve, reject) => {
    const sock = dgram.createSocket('udp4');
    const timeout = setTimeout(() => { sock.close(); reject(new Error('telemetry query timed out')); }, 3000);
    sock.on('message', (msg) => {
      clearTimeout(timeout);
      sock.close();
      try { resolve(JSON.parse(msg.toString())); }
      catch (e) { reject(e); }
    });
    sock.on('error', (e) => { clearTimeout(timeout); reject(e); });
    // The exact query string telemetry.cpp expects -- see its recvfrom/
    // sendto pair; empty/any non-empty request triggers a status reply.
    sock.send('status', 4445, host);
  });
}

async function recordLooper(looperIndex, holdMs) {
  const note = padNote(looperIndex);
  console.log(`[verify-quant] looper${looperIndex}: ARM (note ${note}), holding ${holdMs}ms`);
  await pressPad(note);
  await new Promise((r) => setTimeout(r, holdMs));
  console.log(`[verify-quant] looper${looperIndex}: FINISH`);
  await releasePad(note);
  // Give the control thread's ~5Hz poll loop (main.cpp's usleep(200ms))
  // time to both (a) apply finishtarget/finishreq via applyRecPlayCycle and
  // (b) publish a FRESH telemetry snapshot reflecting the post-finish
  // wrapLen -- worst case is just under 2 full poll periods (one to apply,
  // one to publish), so wait comfortably past that.
  await new Promise((r) => setTimeout(r, 500));
  const t = await queryTelemetry();
  const wrapLenSamples = t.loopers.wraplen[looperIndex];
  const wrapLenSeconds = wrapLenSamples / 48000;
  return { holdMs, wrapLenSamples, wrapLenSeconds };
}

function nearestPow2Candidate(rawSamples, masterLenSamples) {
  let ratio = rawSamples / masterLenSamples;
  if (ratio < 1 / 16) ratio = 1 / 16;
  const lowerExp = Math.floor(Math.log2(ratio));
  const lowerCand = masterLenSamples * Math.pow(2, lowerExp);
  const upperCand = masterLenSamples * Math.pow(2, lowerExp + 1);
  const span = upperCand - lowerCand;
  if (span <= 0) return lowerCand;
  const frac = (rawSamples - lowerCand) / span;
  return frac >= 0.68 ? upperCand : lowerCand;
}

async function main() {
  console.log(`[verify-quant] target=${host}, holds=${holds.join(',')}ms`);
  console.log('[verify-quant] WARNING: assumes a clear rig already exists -- run clear-all yourself first if unsure.');

  const results = [];
  let masterLenSamples = null;
  for (let i = 0; i < holds.length; i++) {
    const r = await recordLooper(i, holds[i]);
    if (i === 0) {
      masterLenSamples = r.wrapLenSamples;
      const expectedSamples = (r.holdMs / 1000) * 48000;
      const errSamples = Math.abs(r.wrapLenSamples - expectedSamples);
      const errMs = (errSamples / 48000) * 1000;
      r.expectedSamples = expectedSamples;
      r.errMs = errMs;
      // Loosely tolerant: real press-to-press timing has genuine, small
      // MIDI-round-trip + control-thread-poll jitter (~5Hz = up to ~200ms
      // worst case) that this script itself introduces -- NOT the thing
      // being verified (that's the DSP's own sample-accurate writeIdx
      // latch, which this test can't isolate from its own injection
      // latency without a hardware timestamp). Flag anything wildly off
      // (>250ms) as a likely real regression, not just injection jitter.
      r.pass = errMs < 250;
      console.log(`[verify-quant] loop0 (FIRST): held=${r.holdMs}ms expected=${r.expectedSamples.toFixed(0)}samp actual=${r.wrapLenSamples}samp err=${r.errMs.toFixed(1)}ms ${r.pass ? 'PASS' : 'FAIL -- check for musical-snapping regression'}`);
    } else {
      const rawSamplesEstimate = (r.holdMs / 1000) * 48000;
      const expectedCandidate = nearestPow2Candidate(rawSamplesEstimate, masterLenSamples);
      const errSamples = Math.abs(r.wrapLenSamples - expectedCandidate);
      const errRatio = errSamples / expectedCandidate;
      r.expectedCandidate = expectedCandidate;
      r.pass = errRatio < 0.05;   // 5% tolerance for injection-side timing jitter
      console.log(`[verify-quant] loop${i}: held=${r.holdMs}ms M=${masterLenSamples}samp expectedCandidate=${expectedCandidate.toFixed(0)}samp actual=${r.wrapLenSamples}samp ${r.pass ? 'PASS' : 'FAIL -- possible quantization-collapse regression'}`);
    }
    results.push(r);
  }

  const allPass = results.every((r) => r.pass);
  console.log(`[verify-quant] ${allPass ? 'ALL PASS' : 'SOME FAILED'}`);
  process.exit(allPass ? 0 : 1);
}

main().catch((err) => {
  console.error('[verify-quant] error:', err.message);
  process.exit(1);
});
