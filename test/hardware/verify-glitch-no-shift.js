#!/usr/bin/env node
// Verifies the user's specific report: "glitches should record even if
// shift isnt pressed" -- i.e. engaging microrepeat/glitch (notes 82-86)
// WITHOUT holding SHIFT must still route glitched content into whatever is
// currently recording, exactly as it does while SHIFT is held.
//
// Static review this session (src/control/apc_grid.cpp/midi.cpp's note
// 82-86 dispatch, dsp/aloop.dsp's GLITCHFOLD comment) found the code as
// WRITTEN already gates glitch-fold purely by fx/microrepeat_div > 0,
// independent of monitorMode/SHIFT -- no SHIFT dependency was found in the
// glitch-engage path itself. This script verifies that INDEPENDENTLY-
// CONFIRMED design claim against the REAL running device via the new
// glitch_engaged/monitor_mode telemetry fields (src/control/telemetry.cpp),
// rather than trusting the static read alone -- if this script ever DOES
// find glitchEngaged=true only reachable together with monitorMode=true,
// that is real evidence of a bug the static read missed (e.g. a hardware-
// level MIDI routing quirk, not visible in the C++ dispatch tables).
//
// What it does:
//   1. Query telemetry, confirm monitor_mode=false and glitch_engaged=false
//      at rest (sanity baseline).
//   2. Send microrepeat latch note-on (note 82, div=1) WITHOUT touching
//      SHIFT at all.
//   3. Query telemetry again -- glitch_engaged must now be true,
//      monitor_mode must STILL be false (proving they're independent, and
//      that glitch engaged without SHIFT).
//   4. Release the microrepeat note, confirm glitch_engaged returns to
//      false.
//
// This verifies the STATE (glitch engaged independent of SHIFT) reaches the
// DSP correctly. It does NOT verify the AUDIBLE/recorded content itself
// (that needs either real audio analysis or a human's ears -- out of scope
// for byte-level MIDI injection, per gm's own skill guidance on that
// distinction). If this state-level check passes but the user still hears
// the bug, the remaining gap is specifically in the audio content routing
// (dsp/aloop.dsp's actual signal chain), not the control-surface dispatch.
//
// Usage: node verify-glitch-no-shift.js <host>

const net = require('net');
const dgram = require('dgram');

const [, , host] = process.argv;
if (!host) {
  console.error('usage: node verify-glitch-no-shift.js <host>');
  process.exit(2);
}

function sendBytes(bytes) {
  return new Promise((resolve, reject) => {
    const sock = net.connect({ host, port: 9401 }, () => {
      sock.write(Buffer.from(bytes), (err) => {
        if (err) return reject(err);
        sock.end();
      });
    });
    sock.setTimeout(5000);
    sock.on('timeout', () => { sock.destroy(); reject(new Error(`connect/write to ${host}:9401 timed out after 5s`)); });
    sock.on('close', resolve);
    sock.on('error', reject);
  });
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
    sock.send('status', 4445, host);
  });
}

// notes 82-86 = microrepeat divisions {1,2,4,8,16} per apc_grid.cpp's own
// onMicrorepeatOn table -- note 82 = division 1 (the simplest/coarsest).
const MICROREPEAT_NOTE = 82;

async function main() {
  console.log(`[verify-glitch] target=${host}`);

  console.log('[verify-glitch] querying baseline telemetry...');
  const before = await queryTelemetry();
  console.log(`[verify-glitch] baseline: monitor_mode=${before.monitor_mode}, glitch_engaged=${before.glitch_engaged}`);
  if (before.monitor_mode || before.glitch_engaged) {
    console.warn('[verify-glitch] WARNING: baseline is not at rest (SHIFT or glitch already engaged) -- results below may be affected by pre-existing state.');
  }

  console.log(`[verify-glitch] engaging microrepeat (note ${MICROREPEAT_NOTE}) WITHOUT touching SHIFT...`);
  await sendBytes([0x90, MICROREPEAT_NOTE, 127]);
  await new Promise((r) => setTimeout(r, 300));   // let the control thread's ~5Hz poll pick it up

  const during = await queryTelemetry();
  console.log(`[verify-glitch] while engaged: monitor_mode=${during.monitor_mode}, glitch_engaged=${during.glitch_engaged}`);

  console.log(`[verify-glitch] releasing microrepeat (note ${MICROREPEAT_NOTE})...`);
  await sendBytes([0x80, MICROREPEAT_NOTE, 0]);
  await new Promise((r) => setTimeout(r, 300));

  const after = await queryTelemetry();
  console.log(`[verify-glitch] after release: monitor_mode=${after.monitor_mode}, glitch_engaged=${after.glitch_engaged}`);

  const glitchEngagedWithoutShift = during.glitch_engaged === true && during.monitor_mode === false;
  const glitchClearedOnRelease = after.glitch_engaged === false;

  console.log('[verify-glitch] === VERDICT ===');
  if (glitchEngagedWithoutShift && glitchClearedOnRelease) {
    console.log('[verify-glitch] PASS: glitch_engaged reached true WITHOUT SHIFT held, and cleared correctly on release. The CONTROL-SURFACE/STATE path is confirmed independent of SHIFT, matching the confirmed design intent.');
    console.log('[verify-glitch] NOTE: this does not verify the AUDIBLE/recorded content itself -- if the bug persists despite this passing, the remaining gap is in the audio signal routing (dsp/aloop.dsp), not this control-surface dispatch.');
    process.exit(0);
  } else {
    console.log('[verify-glitch] FAIL:');
    if (!glitchEngagedWithoutShift) console.log(`  - glitch_engaged did not reach true-without-SHIFT as expected (got glitch_engaged=${during.glitch_engaged}, monitor_mode=${during.monitor_mode})`);
    if (!glitchClearedOnRelease) console.log(`  - glitch_engaged did not clear on release (got ${after.glitch_engaged})`);
    process.exit(1);
  }
}

main().catch((err) => {
  console.error('[verify-glitch] error:', err.message);
  process.exit(1);
});
