#!/usr/bin/env node
// Poll BOTH sides of the aloop <-> esp-idf-link Ableton Link mesh and print one
// comparable row per device, so docs/LINK-MESH-TESTING.md's Tests 1-3 can be run
// as a command instead of by eyeballing a serial console next to a curl.
//
// aloop side: reads /run/aloop/status.json over the existing ssh2 channel
//   (root/aloop) -- link.{synced,bpm,peers,playing} plus wifi ap|sta.
// esp side:   one UDP datagram to the status responder on port 20812
//   (main/link_sync.cpp status_responder_task) -- replies with one JSON line
//   carrying peers/bpm/playing/beat/phase/quantum/ap.
//
// Usage:
//   node link-mesh-status.js --aloop 192.168.4.1 --esp 192.168.4.2
//   node link-mesh-status.js --aloop 192.168.137.100 --esp 192.168.4.3 --watch
//
// Either side may be omitted; whatever is given is polled. --watch repolls every
// 2s until interrupted, which is what you want while power-cycling devices to
// check that exactly one AP wins and everyone converges on the same tempo.

const dgram = require('dgram');

const ESP_STATUS_PORT = 20812;

function parseArgs(argv) {
  const a = { aloop: null, esp: null, watch: false, timeoutMs: 2000 };
  for (let i = 2; i < argv.length; i++) {
    if (argv[i] === '--aloop') a.aloop = argv[++i];
    else if (argv[i] === '--esp') a.esp = argv[++i];
    else if (argv[i] === '--watch') a.watch = true;
    else if (argv[i] === '--timeout') a.timeoutMs = parseInt(argv[++i], 10);
  }
  return a;
}

// One UDP round-trip to the ESP status responder. Any payload triggers a reply.
function queryEsp(host, timeoutMs) {
  return new Promise((resolve) => {
    const sock = dgram.createSocket('udp4');
    let done = false;
    const finish = (val) => {
      if (done) return;
      done = true;
      try { sock.close(); } catch (_) {}
      resolve(val);
    };
    const timer = setTimeout(() => finish({ error: `no reply in ${timeoutMs}ms` }), timeoutMs);
    sock.on('message', (msg) => {
      clearTimeout(timer);
      const text = msg.toString('utf8').trim();
      try { finish(JSON.parse(text)); }
      catch (e) { finish({ error: `unparseable reply: ${text}` }); }
    });
    sock.on('error', (e) => { clearTimeout(timer); finish({ error: e.message }); });
    sock.send(Buffer.from('?'), ESP_STATUS_PORT, host, (e) => {
      if (e) { clearTimeout(timer); finish({ error: e.message }); }
    });
  });
}

// Read aloop's status.json over ssh2 (same credentials the other tools here use).
function queryAloop(host, timeoutMs) {
  return new Promise((resolve) => {
    let Client;
    try { ({ Client } = require('ssh2')); }
    catch (e) { return resolve({ error: 'ssh2 not installed (npm install ssh2)' }); }
    const c = new Client();
    let done = false;
    const finish = (val) => {
      if (done) return;
      done = true;
      try { c.end(); } catch (_) {}
      resolve(val);
    };
    const timer = setTimeout(() => finish({ error: `ssh timeout after ${timeoutMs}ms` }), timeoutMs + 4000);
    c.on('ready', () => {
      c.exec('cat /run/aloop/status.json', (err, stream) => {
        if (err) { clearTimeout(timer); return finish({ error: err.message }); }
        let out = '';
        stream.on('data', (d) => { out += d; });
        stream.on('close', () => {
          clearTimeout(timer);
          try { finish(JSON.parse(out)); }
          catch (e) { finish({ error: `unparseable status.json: ${out.slice(0, 120)}` }); }
        });
      });
    });
    c.on('error', (e) => { clearTimeout(timer); finish({ error: e.message }); });
    c.connect({ host, port: 22, username: 'root', password: 'aloop', readyTimeout: timeoutMs });
  });
}

function fmtAloop(host, s) {
  if (s.error) return `aloop ${host.padEnd(15)}  ERROR: ${s.error}`;
  const l = s.link || {};
  const peers = (l.peers === undefined) ? '?' : l.peers;
  const playing = (l.playing === undefined) ? '?' : l.playing;
  return `aloop ${host.padEnd(15)}  role=${String(s.wifi).padEnd(3)} peers=${String(peers).padEnd(3)}` +
         ` bpm=${Number(l.bpm ?? 0).toFixed(2).padStart(7)} synced=${String(l.synced).padEnd(5)}` +
         ` playing=${playing}`;
}

function fmtEsp(host, s) {
  if (s.error) return `esp   ${host.padEnd(15)}  ERROR: ${s.error}`;
  if (s.link === false) return `esp   ${host.padEnd(15)}  Link not constructed yet`;
  return `esp   ${host.padEnd(15)}  role=${s.ap ? 'ap ' : 'sta'} peers=${String(s.peers).padEnd(3)}` +
         ` bpm=${Number(s.bpm).toFixed(2).padStart(7)} phase=${Number(s.phase).toFixed(3)}` +
         ` quantum=${s.quantum} playing=${s.playing}`;
}

async function pollOnce(args) {
  const jobs = [];
  if (args.aloop) jobs.push(queryAloop(args.aloop, args.timeoutMs).then((r) => fmtAloop(args.aloop, r)));
  if (args.esp) jobs.push(queryEsp(args.esp, args.timeoutMs).then((r) => fmtEsp(args.esp, r)));
  if (!jobs.length) {
    console.error('usage: node link-mesh-status.js --aloop <ip> --esp <ip> [--watch]');
    process.exit(2);
  }
  const lines = await Promise.all(jobs);
  console.log(`--- ${new Date().toISOString()} ---`);
  for (const line of lines) console.log(line);
}

async function main() {
  const args = parseArgs(process.argv);
  if (!args.watch) return pollOnce(args);
  for (;;) {
    await pollOnce(args);
    await new Promise((r) => setTimeout(r, 2000));
  }
}

main();
