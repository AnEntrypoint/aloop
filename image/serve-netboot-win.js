#!/usr/bin/env node
// image/serve-netboot-win.js — native Windows DHCP + TFTP + HTTP netboot server for
// the aloop Alpine diskless netboot root. Modeled on the WITNESSED-working looper
// tftp-server.js: WSL dnsmasq loses the DHCP race to Windows ICS (ICS answers :67
// first with no boot-filename, so the Pi never asks for TFTP). A native Windows
// server binds :67 with reuseAddr and answers WITH option 66 (TFTP server) + 67
// (bootfile bootcode.bin), so the Pi 4 fetches its firmware over TFTP even while ICS
// is running. It also serves the Alpine HTTP root on :8080 (apks/modloop/apkovl).
//
// Run from an ADMIN shell (ports 67 + 69 need privilege):
//   node image/serve-netboot-win.js [--root <netboot-root>] [--server 192.168.137.1]
//
// Default root is .netboot-serve/ (build-netboot.sh OUT). Ctrl-C stops everything.

const fs = require('fs');
const path = require('path');
const dgram = require('dgram');
const http = require('http');

function arg(name, def) { const i = process.argv.indexOf(name); return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : def; }

// Mirror all console output to a logfile so an elevated (UAC) launch, which cannot
// redirect stdout, is still observable. --log <path> overrides the default.
const LOG_FILE = arg('--log', path.join(__dirname, '..', '.netboot-serve.log'));
try { fs.writeFileSync(LOG_FILE, ''); } catch (e) {}
const _log = (...a) => { const line = a.join(' '); try { fs.appendFileSync(LOG_FILE, line + '\n'); } catch (e) {} process.stdout.write(line + '\n'); };
console.log = _log; console.error = _log;

const ROOT      = path.resolve(arg('--root', path.join(__dirname, '..', '.netboot-serve')));
const SERVER_IP = arg('--server', '192.168.137.1');
const HTTP_PORT = parseInt(arg('--http', '8080'));
const BOOTFILE  = 'bootcode.bin';                 // Pi 4 firmware entry point (Alpine tree)
const POOL_START = [192, 168, 137, 100];
const SUBNET     = [255, 255, 255, 0];
const LEASE_SECS = 3600;

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
  if (msg.length < 240 || msg[0] !== 1 || msg.readUInt32BE(236) !== 0x63825363) return;
  const xid = msg.readUInt32BE(4), mac = msg.slice(28, 34);
  const macStr = Array.from(mac).map(b => b.toString(16).padStart(2, '0')).join(':');
  let msgType = 0, o = 240;
  while (o < msg.length) { const opt = msg[o++]; if (opt === 255) break; if (opt === 0) continue; const len = msg[o++]; if (opt === 53) msgType = msg[o]; o += len; }
  const offeredIp = allocate(macStr);
  console.log('[DHCP] ' + (msgType === 1 ? 'DISCOVER' : msgType === 3 ? 'REQUEST' : 'type' + msgType) + ' from ' + macStr + ' -> ' + offeredIp + ' (boot=' + BOOTFILE + ', tftp=' + SERVER_IP + ')');
  const reply = buildDhcpReply(msgType === 1 ? 2 : 5, xid, mac, offeredIp);       // OFFER for DISCOVER, ACK otherwise
  dhcp.send(reply, 68, '255.255.255.255', err => err && console.error('[DHCP]', err.message));
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

console.log('[serve] ready — power-cycle the Pi (SD out, network boot). Ctrl-C to stop.');
