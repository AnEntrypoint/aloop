#!/usr/bin/env node
// image/serve-netboot-win.js — native Windows DHCP + TFTP + HTTP netboot server for
// the aloop Alpine diskless netboot root. Modeled on the WITNESSED-working looper
// tftp-server.js: WSL dnsmasq loses the DHCP race to Windows ICS (ICS answers :67
// first with no boot-filename, so the Pi never asks for TFTP). A native Windows
// server binds :67 with reuseAddr and answers WITH option 66 (TFTP server) + 67
// (bootfile bootcode.bin), so the Pi 4 fetches its firmware over TFTP even while ICS
// is running. It also serves the Alpine HTTP root on :8080 (apks/modloop/apkovl).
//
// SELF-UPDATE (mirrors looper's tftp-server.js checkAndUpdate): looper polls
// GitHub Releases every 30s for a new looper-sd.zip and rewrites its SD-resident
// kernel; aloop has no Releases (CI artifacts only) and boots diskless (the WHOLE
// root is re-fetched fresh every power-cycle, not just a kernel file), so the
// equivalent here polls the latest GREEN build-binary + build-lv2 Actions runs,
// downloads their artifacts when the head_sha changes, rebuilds the netboot root
// in place (image/build-netboot.sh) with the new binary+lv2, and sends REBOOT to
// the already-running Pi (src/control/remote_control.cpp, udp/4446) so it picks
// up the new build on its next boot. aloop's repo is PRIVATE, so a GITHUB_TOKEN
// (env, or --token) is REQUIRED for this (unlike looper's public-repo Releases,
// which need no auth) — the loop refuses to start without one, loudly, rather
// than silently never finding anything.
//
// Run from an ADMIN shell (ports 67 + 69 need privilege):
//   GITHUB_TOKEN=<token> node image/serve-netboot-win.js [--root <netboot-root>] [--server 192.168.137.1]
//   ALOOP_NO_AUTO_UPDATE=1 to disable the poll loop (matches looper's LOOPER_NO_AUTO_UPDATE).
//
// Default root is .netboot-serve/ (build-netboot.sh OUT). Ctrl-C stops everything.

const fs = require('fs');
const path = require('path');
const dgram = require('dgram');
const http = require('http');
const https = require('https');
const { execFileSync, execFile } = require('child_process');
const { promisify } = require('util');
const execFileAsync = promisify(execFile);

function arg(name, def) { const i = process.argv.indexOf(name); return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : def; }

// Mirror all console output to a logfile so an elevated (UAC) launch, which cannot
// redirect stdout, is still observable. --log <path> overrides the default.
// This process runs indefinitely (self-update poll loop) and mirrors every
// TFTP/HTTP/DHCP line — with no cap the file grows unbounded over a
// multi-day serve session. Truncate back to zero once it crosses LOG_MAX_BYTES
// (a crude rotation: no history kept, but that matches this script's own
// existing "truncate on every restart" behavior, just applied mid-run too).
const LOG_FILE = arg('--log', path.join(__dirname, '..', '.netboot-serve.log'));
const LOG_MAX_BYTES = parseInt(arg('--log-max-bytes', String(10 * 1024 * 1024)));   // 10 MiB
try { fs.writeFileSync(LOG_FILE, ''); } catch (e) {}
const _log = (...a) => {
  const line = a.join(' ');
  try {
    const st = fs.statSync(LOG_FILE);
    if (st.size > LOG_MAX_BYTES) fs.writeFileSync(LOG_FILE, '[log] rotated (exceeded ' + LOG_MAX_BYTES + ' bytes)\n');
    fs.appendFileSync(LOG_FILE, line + '\n');
  } catch (e) {}
  process.stdout.write(line + '\n');
};
console.log = _log; console.error = _log;

const ROOT      = path.resolve(arg('--root', path.join(__dirname, '..', '.netboot-serve')));
const SERVER_IP = arg('--server', '192.168.137.1');
const HTTP_PORT = parseInt(arg('--http', '8080'));
const BOOTFILE  = 'bootcode.bin';                 // Pi 4 firmware entry point (Alpine tree)
const POOL_START = [192, 168, 137, 100];
const SUBNET     = [255, 255, 255, 0];
const LEASE_SECS = 3600;

// ---- self-update config (see the file header comment) -----------------------
const REPO           = 'AnEntrypoint/aloop';
const GITHUB_TOKEN    = arg('--token', process.env.GITHUB_TOKEN || process.env.ALOOP_GITHUB_TOKEN || '');
const AUTO_UPDATE     = process.env.ALOOP_NO_AUTO_UPDATE !== '1';
const UPDATE_INTERVAL_MS = parseInt(arg('--update-interval', '30000'));   // matches looper's 30s poll
const SHA_FILE  = path.join(path.dirname(ROOT), '.netboot-update-sha');
const PI_HOST   = arg('--pi', '192.168.137.100');
const PI_TOKEN  = arg('--pi-token', process.env.PI_TOKEN || '');
const NETBOOT_SERVER = SERVER_IP;   // build-netboot.sh's NETBOOT_SERVER = the same IP we serve HTTP on

if (!fs.existsSync(path.join(ROOT, 'start4.elf'))) {
  console.error('[serve] netboot root looks wrong: no start4.elf in ' + ROOT + ' (run image/build-netboot.sh)');
  process.exit(2);
}
console.log('[serve] netboot root: ' + ROOT);
console.log('[serve] server IP:    ' + SERVER_IP + '  (DHCP :67, TFTP :69, HTTP :' + HTTP_PORT + ')');

const OP_RRQ = 1, OP_DATA = 3, OP_ACK = 4, OP_ERR = 5, OP_OACK = 6;
const leases = {};
function ip2buf(s) { return Buffer.from(s.split('.').map(Number)); }
function allocate(mac) { if (leases[mac]) return leases[mac]; const ip = [...POOL_START]; ip[3] += Object.keys(leases).length; return leases[mac] = ip.join('.'); }

// ---- TFTP -------------------------------------------------------------------
// The Pi 4 requests files under its BOARD-SERIAL subdir (<serial>/start4.elf). We
// serve the flat root; if <serial>/foo is missing, fall back to foo at the root.
function parseOpts(msg, offset) {
  const opts = {};
  while (offset < msg.length) {
    let e = msg.indexOf(0, offset); if (e < 0) break;
    const k = msg.slice(offset, e).toString().toLowerCase(); offset = e + 1;
    e = msg.indexOf(0, offset); if (e < 0) break;
    const v = msg.slice(offset, e).toString(); offset = e + 1;
    opts[k] = v;
  }
  return opts;
}
function buildOACK(o) {
  const parts = [];
  for (const k in o) { parts.push(Buffer.from(k), Buffer.from([0]), Buffer.from(String(o[k])), Buffer.from([0])); }
  return Buffer.concat([Buffer.from([0, OP_OACK]), ...parts]);
}
function handleRRQ(filename, rinfo, options) {
  const safe = path.normalize(filename).replace(/^(\.\.[/\\])+/, '');
  let full = path.join(ROOT, safe);
  if (!full.startsWith(ROOT)) return;
  if (!fs.existsSync(full)) {
    const parts = safe.split(/[/\\]/);
    if (parts.length > 1) {
      const fallback = path.join(ROOT, ...parts.slice(1));
      if (fallback.startsWith(ROOT) && fs.existsSync(fallback)) { full = fallback; }
    }
  }
  const xfer = dgram.createSocket('udp4');
  xfer.bind(0, () => {
    if (!fs.existsSync(full) || fs.statSync(full).isDirectory()) {
      const e = Buffer.alloc(4 + 15); e.writeUInt16BE(OP_ERR, 0); e.writeUInt16BE(1, 2); Buffer.from('File not found').copy(e, 4);
      xfer.send(e, rinfo.port, rinfo.address); setTimeout(() => xfer.close(), 500);
      console.error('[TFTP] NOT FOUND: ' + safe); return;
    }
    const data = fs.readFileSync(full);
    const blksize = options.blksize ? parseInt(options.blksize) : 512;
    const replyOpts = {};
    if (options.blksize) replyOpts.blksize = String(blksize);
    if (options.tsize) replyOpts.tsize = String(data.length);
    const blocks = Math.ceil(data.length / blksize) || 1;
    let acked = (options.blksize || options.tsize) ? -1 : 0;
    if (Object.keys(replyOpts).length) xfer.send(buildOACK(replyOpts), rinfo.port, rinfo.address);
    else { const d = Buffer.alloc(4 + Math.min(blksize, data.length)); d.writeUInt16BE(OP_DATA, 0); d.writeUInt16BE(1, 2); data.copy(d, 4, 0, blksize); xfer.send(d, rinfo.port, rinfo.address); acked = 0; }
    console.log('[TFTP] ' + safe + ' -> ' + rinfo.address + ' (' + data.length + 'B)');
    xfer.on('message', msg => {
      if (msg.readUInt16BE(0) !== OP_ACK) return;
      const blk = msg.readUInt16BE(2);
      if (blk !== acked + 1 && !(blk === 0 && acked === -1)) return;
      acked = blk;
      if (acked >= blocks) { xfer.close(); return; }
      const start = acked * blksize, chunk = data.slice(start, start + blksize);
      const pkt = Buffer.alloc(4 + chunk.length); pkt.writeUInt16BE(OP_DATA, 0); pkt.writeUInt16BE((acked + 1) & 0xffff, 2); chunk.copy(pkt, 4);
      xfer.send(pkt, rinfo.port, rinfo.address);
    });
  });
}
const tftp = dgram.createSocket('udp4');
tftp.on('message', (msg, rinfo) => {
  if (msg.readUInt16BE(0) !== OP_RRQ) return;
  let offset = 2; const end = msg.indexOf(0, offset);
  const filename = msg.slice(offset, end).toString(); offset = end + 1;
  const modeEnd = msg.indexOf(0, offset); offset = modeEnd + 1;
  handleRRQ(filename, rinfo, parseOpts(msg, offset));
});
tftp.on('error', err => { if (err.code === 'EACCES') { console.error('[TFTP] need ADMIN: port 69'); process.exit(1); } console.error('[TFTP]', err.message); });
tftp.bind(69, '0.0.0.0', () => console.log('[TFTP] listening :69'));

// ---- DHCP -------------------------------------------------------------------
// reuseAddr lets us share :67 with Windows ICS; we answer WITH the boot options ICS
// omits, so the Pi accepts our offer and proceeds to TFTP.
function buildDhcpReply(type, xid, mac, offeredIp) {
  const buf = Buffer.alloc(576);
  buf[0] = 2; buf[1] = 1; buf[2] = 6; buf.writeUInt32BE(xid, 4); buf.writeUInt16BE(0x8000, 10);
  ip2buf(offeredIp).copy(buf, 16); ip2buf(SERVER_IP).copy(buf, 20); mac.copy(buf, 28);
  buf.writeUInt32BE(0x63825363, 236);
  let o = 240;
  buf[o++] = 53; buf[o++] = 1; buf[o++] = type;                                   // msg type
  buf[o++] = 54; buf[o++] = 4; ip2buf(SERVER_IP).copy(buf, o); o += 4;            // server id
  buf[o++] = 51; buf[o++] = 4; buf.writeUInt32BE(LEASE_SECS, o); o += 4;          // lease
  buf[o++] = 1;  buf[o++] = 4; Buffer.from(SUBNET).copy(buf, o); o += 4;          // subnet mask
  const ti = Buffer.from(SERVER_IP); buf[o++] = 66; buf[o++] = ti.length; ti.copy(buf, o); o += ti.length; // TFTP server (opt 66)
  const boot = Buffer.from(BOOTFILE + '\0'); buf[o++] = 67; buf[o++] = boot.length; boot.copy(buf, o); o += boot.length; // bootfile (opt 67)
  buf[o++] = 255;
  return buf.slice(0, o);
}
const dhcp = dgram.createSocket({ type: 'udp4', reuseAddr: true });
dhcp.on('message', (msg) => {
  // A malformed/truncated packet from anything on the LAN must never take the
  // whole serve process down (same defensive posture as the TFTP/HTTP
  // handlers, which already guard every path). Node's Buffer indexing itself
  // can't throw (out-of-range reads return undefined, not an exception), but
  // wrap the handler anyway — cheap insurance against any future change here
  // (e.g. a readUInt16BE on a short slice DOES throw) regressing that safety.
  try {
    if (msg.length < 240 || msg[0] !== 1 || msg.readUInt32BE(236) !== 0x63825363) return;
    const xid = msg.readUInt32BE(4), mac = msg.slice(28, 34);
    const macStr = Array.from(mac).map(b => b.toString(16).padStart(2, '0')).join(':');
    let msgType = 0, o = 240;
    while (o < msg.length) {
      const opt = msg[o++];
      if (opt === 255) break;
      if (opt === 0) continue;
      if (o >= msg.length) break;                 // truncated: a length byte was promised but absent
      const len = msg[o++];
      if (opt === 53 && o < msg.length) msgType = msg[o];
      o += len;                                    // may overshoot msg.length; the while-guard ends the loop next iteration
    }
    const offeredIp = allocate(macStr);
    console.log('[DHCP] ' + (msgType === 1 ? 'DISCOVER' : msgType === 3 ? 'REQUEST' : 'type' + msgType) + ' from ' + macStr + ' -> ' + offeredIp + ' (boot=' + BOOTFILE + ', tftp=' + SERVER_IP + ')');
    const reply = buildDhcpReply(msgType === 1 ? 2 : 5, xid, mac, offeredIp);       // OFFER for DISCOVER, ACK otherwise
    dhcp.send(reply, 68, '255.255.255.255', err => err && console.error('[DHCP]', err.message));
  } catch (e) {
    console.error('[DHCP] malformed packet, ignored:', e.message);
  }
});
dhcp.on('error', err => { if (err.code === 'EACCES') { console.error('[DHCP] need ADMIN: port 67'); process.exit(1); } console.error('[DHCP]', err.message); });
dhcp.bind(67, '0.0.0.0', () => { dhcp.setBroadcast(true); console.log('[DHCP] listening :67'); });

// ---- HTTP root (Alpine initramfs fetches apks/modloop/apkovl over HTTP) ------
const mime = { '.tar': 'application/x-tar', '.gz': 'application/gzip' };
const httpSrv = http.createServer((req, res) => {
  const safe = path.normalize(decodeURIComponent(req.url.split('?')[0])).replace(/^(\.\.[/\\])+/, '');
  const full = path.join(ROOT, safe);
  if (!full.startsWith(ROOT) || !fs.existsSync(full) || fs.statSync(full).isDirectory()) { res.statusCode = 404; res.end('not found'); console.log('[HTTP] 404 ' + safe); return; }
  res.setHeader('Content-Type', mime[path.extname(full)] || 'application/octet-stream');
  res.setHeader('Content-Length', fs.statSync(full).size);
  fs.createReadStream(full).pipe(res);
  console.log('[HTTP] 200 ' + safe + ' (' + fs.statSync(full).size + 'B)');
});
httpSrv.on('error', err => console.error('[HTTP]', err.message));
httpSrv.listen(HTTP_PORT, SERVER_IP, () => console.log('[HTTP] listening http://' + SERVER_IP + ':' + HTTP_PORT + '/'));

// ---- self-update (mirrors looper's tftp-server.js checkAndUpdate) -----------
let currentSha = null, rateLimitedUntil = 0;
try { currentSha = fs.readFileSync(SHA_FILE, 'utf8').trim(); console.log('[update] last-built sha: ' + currentSha); } catch (e) {}

function ghGet(apiPath) {
  return new Promise((resolve, reject) => {
    https.get('https://api.github.com' + apiPath, {
      headers: { 'User-Agent': 'aloop-netboot-serve/1.0', 'Authorization': 'token ' + GITHUB_TOKEN, 'Accept': 'application/vnd.github+json' }
    }, res => {
      if (res.statusCode === 301 || res.statusCode === 302) return ghGet(new URL(res.headers.location).pathname + new URL(res.headers.location).search).then(resolve).catch(reject);
      let body = ''; res.on('data', c => body += c); res.on('end', () => resolve({ status: res.statusCode, body, headers: res.headers }));
    }).on('error', reject);
  });
}
// Artifact downloads are a zip; write it, unzip via PowerShell's Expand-Archive
// (no extra dependency on a bare Windows host, matching looper's use of
// PowerShell for its own zip extraction in dev-server.js's sibling scripts).
//
// GitHub's artifact endpoint answers with a 303 (not 301/302) to a pre-signed
// Azure Blob Storage URL (the SAS token is IN the query string, e.g.
// productionresultsNN.blob.core.windows.net/...&sig=...). WITNESSED: sending
// our GitHub `Authorization: token ...` header on to that redirect target
// makes Azure return 403 (it sees an auth header it doesn't recognize on a
// URL that's already authorized via its own signature) -- the header must be
// stripped once we leave api.github.com, only ever sent to the ORIGINAL host.
function downloadArtifactZip(archiveUrl, destZip) {
  return new Promise((resolve, reject) => {
    const originalHost = new URL(archiveUrl).host;
    const follow = u => {
      const sameHost = new URL(u).host === originalHost;
      const headers = { 'User-Agent': 'aloop-netboot-serve/1.0' };
      if (sameHost) headers['Authorization'] = 'token ' + GITHUB_TOKEN;
      https.get(u, { headers }, res => {
        if (res.statusCode === 301 || res.statusCode === 302 || res.statusCode === 303) return follow(res.headers.location);
        if (res.statusCode !== 200) return reject(new Error('artifact download HTTP ' + res.statusCode));
        const tmp = destZip + '.tmp', out = fs.createWriteStream(tmp);
        const cleanupAndReject = err => { fs.rmSync(tmp, { force: true }); reject(err); };
        res.on('error', cleanupAndReject);   // a mid-stream socket error on the readable itself, not just the write side
        res.pipe(out);
        out.on('finish', () => { fs.renameSync(tmp, destZip); resolve(); });
        out.on('error', cleanupAndReject);
      }).on('error', reject);
    };
    follow(archiveUrl);
  });
}
async function latestGreenRun(workflowFile) {
  const r = await ghGet('/repos/' + REPO + '/actions/workflows/' + workflowFile + '/runs?status=success&branch=main&per_page=1');
  if (r.status === 403 || r.status === 429) { rateLimitedUntil = Date.now() + (parseInt(r.headers['retry-after'] || '60') * 1000); console.error('[update] rate-limited'); return null; }
  if (r.status !== 200) { console.log('[update] ' + workflowFile + ' runs: GitHub status ' + r.status); return null; }
  const runs = JSON.parse(r.body).workflow_runs;
  return runs && runs[0] ? runs[0] : null;
}
async function downloadRunArtifact(runId, artifactName, destDir) {
  const r = await ghGet('/repos/' + REPO + '/actions/runs/' + runId + '/artifacts');
  if (r.status !== 200) throw new Error('list artifacts HTTP ' + r.status);
  const art = JSON.parse(r.body).artifacts.find(a => a.name === artifactName);
  if (!art) throw new Error('no artifact named ' + artifactName + ' on run ' + runId);
  fs.mkdirSync(destDir, { recursive: true });
  const zipPath = path.join(destDir, artifactName + '.zip');
  await downloadArtifactZip(art.archive_download_url, zipPath);
  execFileSync('powershell', ['-NoProfile', '-Command', 'Expand-Archive -Path "' + zipPath + '" -DestinationPath "' + destDir + '" -Force'], { stdio: 'pipe' });
  return destDir;
}
function sendPiReboot() {
  if (!PI_TOKEN) { console.log('[update] no --pi-token/PI_TOKEN set — skipping REBOOT (Pi will pick up the new build on its NEXT power-cycle anyway)'); return; }
  const sock = dgram.createSocket('udp4');
  const msg = Buffer.from('REBOOT:' + PI_TOKEN);
  sock.send(msg, 4446, PI_HOST, err => { sock.close(); console.log(err ? '[update] REBOOT send failed: ' + err.message : '[update] REBOOT sent to ' + PI_HOST + ':4446'); });
}
async function checkAndUpdate() {
  if (!AUTO_UPDATE) return;
  if (!GITHUB_TOKEN) { console.error('[update] no GITHUB_TOKEN/ALOOP_GITHUB_TOKEN/--token set — aloop/aloop is PRIVATE, auto-update cannot list runs or download artifacts without one. Set the env var or pass --token, or set ALOOP_NO_AUTO_UPDATE=1 to silence this.'); return; }
  try {
    if (Date.now() < rateLimitedUntil) { console.log('[update] rate-limited, skipping this tick'); return; }
    const [binRun, lv2Run] = await Promise.all([latestGreenRun('build-binary.yml'), latestGreenRun('build-lv2.yml')]);
    if (!binRun || !lv2Run) { console.log('[update] no green build-binary/build-lv2 run found yet'); return; }
    const sha = binRun.head_sha + ':' + lv2Run.head_sha;
    if (sha === currentSha) { console.log('[update] up to date (sha ' + sha.slice(0, 16) + '...)'); return; }
    console.log('[update] new build found: ' + sha.slice(0, 16) + '... (was ' + (currentSha ? currentSha.slice(0, 16) + '...' : 'none') + ')');

    const work = path.join(path.dirname(ROOT), '.netboot-update-work');
    fs.rmSync(work, { recursive: true, force: true });
    const binDir = await downloadRunArtifact(binRun.id, 'aloop-aarch64-musl', path.join(work, 'bin'));
    const lv2Dir = await downloadRunArtifact(lv2Run.id, 'home-fx-lv2', path.join(work, 'lv2'));
    const aloopBin = path.join(binDir, 'aloop');
    if (!fs.existsSync(aloopBin)) throw new Error('aloop binary not found in downloaded artifact at ' + aloopBin);

    console.log('[update] rebuilding netboot root -> ' + ROOT);
    // Windows commonly has THREE different "bash.exe" on PATH: Git-Bash
    // (understands native C:/... paths), the WSL launcher stub under
    // System32, and a WindowsApps alias. Plain execFileSync('bash', ...)
    // resolves whichever one PATH lists first for the CURRENT process/shell —
    // WITNESSED live: under this script's launch environment that resolved to
    // the WSL stub, which cannot see C:/dev/... at all ("No such file or
    // directory" on a path that verifiably exists). Pin the real Git-Bash
    // install explicitly so this doesn't depend on PATH order.
    const gitBashCandidates = [
      'C:\\Program Files\\Git\\bin\\bash.exe',
      'C:\\Program Files\\Git\\usr\\bin\\bash.exe',
      'C:\\Program Files (x86)\\Git\\bin\\bash.exe',
    ];
    const bashExe = gitBashCandidates.find(p => fs.existsSync(p)) || 'bash';
    const buildScript = path.join(__dirname, 'build-netboot.sh').replace(/\\/g, '/');
    // WITNESSED live: a SYNCHRONOUS execFileSync rebuild call freezes the
    // ENTIRE Node event loop for as long as it runs — no more DHCP/TFTP/HTTP
    // serving, no more checkAndUpdate ticks, nothing, even on a successful
    // rebuild that just takes a while (tar/gzip a real apkovl is not
    // instant). Worse, with no timeout, a genuinely stuck child (a hung
    // apk-fetch, a hung subprocess waiting on stdin) freezes the server
    // indefinitely with zero error, zero log line — indistinguishable from
    // "still working" until someone checks wall-clock time against the last
    // log line (exactly what happened here). Unlike looper's own
    // checkAndUpdate (a simple download+unzip+file-copy, no long subprocess
    // at all), aloop's rebuild genuinely shells out to a multi-step bash
    // script — so run it ASYNC (execFile, awaited) so the event loop keeps
    // serving DHCP/TFTP/HTTP throughout, AND cap it with an explicit
    // timeout so a stuck child is killed and the catch below logs a real
    // error and retries next tick — matching looper's "any failure is
    // caught, next tick tries again" self-healing design.
    const REBUILD_TIMEOUT_MS = 5 * 60 * 1000;
    await execFileAsync(bashExe, [buildScript], {
      cwd: path.join(__dirname, '..'),
      env: Object.assign({}, process.env, { OUT: ROOT, ALOOP_BIN: aloopBin, LV2_DIR: lv2Dir, NETBOOT_SERVER: NETBOOT_SERVER }),
      timeout: REBUILD_TIMEOUT_MS,
      killSignal: 'SIGKILL',
      maxBuffer: 64 * 1024 * 1024
    });
    currentSha = sha; fs.writeFileSync(SHA_FILE, sha);
    console.log('[update] netboot root rebuilt with the new build; sending REBOOT so the Pi re-fetches it');
    sendPiReboot();
  } catch (e) {
    console.error('[update] failed:', e.message);
  }
}

if (AUTO_UPDATE && !GITHUB_TOKEN) {
  console.error('[update] AUTO-UPDATE DISABLED: no GitHub token available (set GITHUB_TOKEN or --token, or ALOOP_NO_AUTO_UPDATE=1 to silence this warning)');
} else if (AUTO_UPDATE) {
  console.log('[update] self-update ENABLED — polling build-binary/build-lv2 every ' + (UPDATE_INTERVAL_MS / 1000) + 's');
  checkAndUpdate();
  setInterval(checkAndUpdate, UPDATE_INTERVAL_MS);
} else {
  console.log('[update] self-update DISABLED (ALOOP_NO_AUTO_UPDATE=1)');
}

console.log('[serve] ready — power-cycle the Pi (SD out, network boot). Ctrl-C to stop.');
