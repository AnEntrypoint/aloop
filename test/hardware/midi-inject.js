#!/usr/bin/env node
// Synthetic MIDI byte injection over aloop's own injection socket (tcp/9401,
// src/control/midi.cpp) -- lets a reproduction/regression script send the
// EXACT byte sequence a physical APC Key25 button press/release would have
// produced, without needing a human at the hardware. Every byte written
// here is parsed by the identical state machine real MIDI input goes
// through (see midi.cpp's injection-socket comment), so a scripted
// sequence is indistinguishable from a real button press to ApcGrid, the
// LED refresh, and telemetry.
//
// Built to close a real gap: this project's MIDI-hardware bugs (APC grid
// buttons not responding, the blank-loop-after-erase investigation) could
// previously only be reproduced by asking a human to physically press pads
// and confirm the result, one AskUserQuestion round-trip per diagnostic
// build. This script replaces that physical step for anything driven by
// MIDI message content (not analog timing/audible quality, which still
// need a human -- see gm's skill guidance on this distinction).
//
// Usage:
//   node midi-inject.js <host> note-on <note> <velocity> [channel=0]
//   node midi-inject.js <host> note-off <note> [velocity=0] [channel=0]
//   node midi-inject.js <host> cc <controller> <value> [channel=0]
//   node midi-inject.js <host> raw <hex-bytes...>          # e.g. raw 90 02 7f
//   node midi-inject.js <host> hold <note> <ms> [channel=0]  # note-on, wait ms, note-off
//
// Examples (reproducing this session's diag6 trace: press pad note 2, hold
// past the 1s erase threshold, release):
//   node midi-inject.js 192.168.137.100 hold 2 1100
//   node midi-inject.js 192.168.137.100 note-on 2 127
//   node midi-inject.js 192.168.137.100 note-off 2

const net = require('net');

const [, , host, cmd, ...rest] = process.argv;
if (!host || !cmd) {
  console.error('usage: node midi-inject.js <host> <note-on|note-off|cc|raw|hold> ...');
  process.exit(2);
}

// WITNESSED live: a bare net.connect() with no explicit timeout can hang for
// the OS's own default TCP connect timeout (well over a minute on Windows)
// against an unreachable host -- far too long for a script meant to fail
// fast. sock.setTimeout() bounds this to a few seconds; 'timeout' fires
// WITHOUT closing the socket (Node's own documented behavior), so this
// destroys it manually to force the reject path instead of hanging past
// the bound.
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

async function main() {
  if (cmd === 'raw') {
    const bytes = rest.map((h) => parseInt(h, 16));
    await sendBytes(bytes);
    console.log(`[midi-inject] sent raw bytes: ${rest.join(' ')}`);
  } else if (cmd === 'note-on') {
    const [note, vel = '127', ch = '0'] = rest;
    const status = 0x90 | (parseInt(ch, 10) & 0x0f);
    await sendBytes([status, parseInt(note, 10), parseInt(vel, 10)]);
    console.log(`[midi-inject] note-on note=${note} vel=${vel} ch=${ch}`);
  } else if (cmd === 'note-off') {
    const [note, vel = '0', ch = '0'] = rest;
    const status = 0x80 | (parseInt(ch, 10) & 0x0f);
    await sendBytes([status, parseInt(note, 10), parseInt(vel, 10)]);
    console.log(`[midi-inject] note-off note=${note} vel=${vel} ch=${ch}`);
  } else if (cmd === 'cc') {
    const [controller, value, ch = '0'] = rest;
    const status = 0xb0 | (parseInt(ch, 10) & 0x0f);
    await sendBytes([status, parseInt(controller, 10), parseInt(value, 10)]);
    console.log(`[midi-inject] cc controller=${controller} value=${value} ch=${ch}`);
  } else if (cmd === 'hold') {
    const [note, ms, ch = '0'] = rest;
    const onStatus = 0x90 | (parseInt(ch, 10) & 0x0f);
    const offStatus = 0x80 | (parseInt(ch, 10) & 0x0f);
    await sendBytes([onStatus, parseInt(note, 10), 127]);
    console.log(`[midi-inject] note-on note=${note} (holding ${ms}ms)`);
    await new Promise((r) => setTimeout(r, parseInt(ms, 10)));
    await sendBytes([offStatus, parseInt(note, 10), 0]);
    console.log(`[midi-inject] note-off note=${note} (held ${ms}ms)`);
  } else {
    console.error(`unknown command: ${cmd}`);
    process.exit(2);
  }
}

main().catch((err) => {
  console.error('[midi-inject] error:', err.message);
  process.exit(1);
});
