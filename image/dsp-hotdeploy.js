#!/usr/bin/env node
// image/dsp-hotdeploy.js — fast DSP-only iteration: push .dsp edits to CI,
// wait for the real musl/aarch64 cross-compile, then SSH the changed
// artifact straight onto a live device and restart only the aloop service.
//
// Skips image/build-netboot.sh's full image assembly and any reboot: a pure
// DSP edit needs neither. Never bypasses the CI cross-compile (see AGENTS.md
// "the device runs Alpine/musl/aarch64" -- a host-built .so silently fails
// to load), it only skips everything downstream of "artifact is green".
//
// Usage:
//   node image/dsp-hotdeploy.js --target home       (dsp/*.dsp -> aloop binary, home-fx-lv2)
//   node image/dsp-hotdeploy.js --target guitar     (guitar_lofi_fx.dsp -> guitar-lofi-fx-lv2)
//   node image/dsp-hotdeploy.js --target both
//
// Requires: gh CLI authenticated (gh auth status), ssh2 (already a repo
// devDependency), a pushed commit containing the .dsp edit (this script
// polls the run CI started for that commit -- it does not create a run).

const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');
const { Client } = require('ssh2');

function arg(name, envName, def) {
  const i = process.argv.indexOf(name);
  if (i > 0 && process.argv[i + 1]) return process.argv[i + 1];
  if (envName && process.env[envName]) return process.env[envName];
  return def;
}

const TARGET = arg('--target', 'DSP_TARGET', 'home');
const HOST = arg('--host', 'PI_HOST', '192.168.137.100');
const SSH_USER = arg('--user', 'PI_SSH_USER', 'root');
const SSH_PASS = arg('--pass', 'PI_SSH_PASS', 'aloop');
const POLL_MS = parseInt(arg('--poll-ms', 'DSP_POLL_MS', '10000'));
const POLL_TIMEOUT_MS = parseInt(arg('--timeout-ms', 'DSP_TIMEOUT_MS', '900000'));

function sh(cmd, args) {
  return execFileSync(cmd, args, { encoding: 'utf8' });
}

function headSha() {
  return sh('git', ['rev-parse', 'HEAD']).trim();
}

function waitForRun(workflowFile, sha, deadline) {
  for (;;) {
    if (Date.now() > deadline) throw new Error(`timed out waiting for ${workflowFile} run on ${sha}`);
    const out = sh('gh', ['run', 'list', '--workflow', workflowFile, '--json', 'headSha,status,conclusion,databaseId', '--limit', '20']);
    const runs = JSON.parse(out);
    const run = runs.find(r => r.headSha === sha);
    if (run) {
      if (run.status === 'completed') {
        if (run.conclusion !== 'success') throw new Error(`${workflowFile} run ${run.databaseId} concluded ${run.conclusion} -- fix CI before hot-deploying`);
        return run.databaseId;
      }
      console.log(`[dsp-hotdeploy] ${workflowFile} run ${run.databaseId} status=${run.status}, waiting...`);
    } else {
      console.log(`[dsp-hotdeploy] no ${workflowFile} run found yet for ${sha}, waiting...`);
    }
    execFileSync('node', ['-e', `setTimeout(()=>{}, ${POLL_MS})`]);
  }
}

function downloadArtifact(runId, artifactName, destDir) {
  fs.mkdirSync(destDir, { recursive: true });
  sh('gh', ['run', 'download', String(runId), '--name', artifactName, '--dir', destDir]);
  return destDir;
}

function shellQuote(s) {
  return `'${String(s).replace(/'/g, `'\\''`)}'`;
}

function sshExec(conn, cmd) {
  return new Promise((resolve, reject) => {
    conn.exec(cmd, (err, stream) => {
      if (err) return reject(err);
      let out = '', errOut = '';
      stream.on('close', (code) => resolve({ code, out, errOut }));
      stream.on('data', (d) => { out += d.toString(); });
      stream.stderr.on('data', (d) => { errOut += d.toString(); });
    });
  });
}

function sftpPut(conn, localPath, remotePath) {
  return new Promise((resolve, reject) => {
    conn.sftp((err, sftp) => {
      if (err) return reject(err);
      sftp.fastPut(localPath, remotePath, (err2) => {
        if (err2) return reject(err2);
        resolve();
      });
    });
  });
}

async function deployToDevice(files) {
  const conn = new Client();
  await new Promise((resolve, reject) => {
    conn.on('ready', resolve).on('error', reject)
      .connect({ host: HOST, port: 22, username: SSH_USER, password: SSH_PASS, readyTimeout: 15000 });
  });
  try {
    console.log('[dsp-hotdeploy] stopping aloop service before overwriting its binary (musl ETXTBSY guard)');
    await sshExec(conn, 'rc-service aloop stop');
    for (const f of files) {
      console.log(`[dsp-hotdeploy] pushing ${f.local} -> ${f.remote}`);
      await sftpPut(conn, f.local, f.remote);
      if (f.exec) await sshExec(conn, `chmod +x ${shellQuote(f.remote)}`);
    }
    console.log('[dsp-hotdeploy] starting aloop service (not a reboot)');
    const r = await sshExec(conn, 'rc-service aloop start');
    console.log(r.out || r.errOut);
    const status = await sshExec(conn, 'rc-service aloop status');
    console.log('[dsp-hotdeploy] post-restart status:', status.out.trim());
    if (!/started/.test(status.out)) {
      throw new Error(`aloop did not report started after restart: ${status.out}`);
    }
  } finally {
    conn.end();
  }
}

async function main() {
  const sha = headSha();
  const deadline = Date.now() + POLL_TIMEOUT_MS;
  const workDir = fs.mkdtempSync(path.join(os.tmpdir(), 'dsp-hotdeploy-'));
  const files = [];

  if (TARGET === 'home' || TARGET === 'both') {
    const runId = waitForRun('build-binary.yml', sha, deadline);
    const dir = downloadArtifact(runId, 'aloop-aarch64-musl', path.join(workDir, 'bin'));
    const bin = path.join(dir, 'aloop');
    if (!fs.existsSync(bin)) throw new Error(`expected ${bin} in aloop-aarch64-musl artifact`);
    files.push({ local: bin, remote: '/opt/aloop/aloop', exec: true });
  }
  if (TARGET === 'guitar' || TARGET === 'both') {
    const runId = waitForRun('build-lv2.yml', sha, deadline);
    const dir = downloadArtifact(runId, 'guitar-lofi-fx-lv2', path.join(workDir, 'lv2'));
    const bundleName = fs.readdirSync(dir).find(n => n.endsWith('.lv2'));
    if (!bundleName) throw new Error('expected a *.lv2 dir in guitar-lofi-fx-lv2 artifact');
    if (!/^[A-Za-z0-9_.-]+\.lv2$/.test(bundleName)) throw new Error(`unsafe bundle name in artifact: ${bundleName}`);
    const so = path.join(dir, bundleName, `${bundleName.replace(/\.lv2$/, '')}.so`);
    const ttl = fs.readdirSync(path.join(dir, bundleName)).filter(n => n.endsWith('.ttl'));
    files.push({ local: so, remote: `/effects/home/${bundleName}/${path.basename(so)}`, exec: true });
    for (const t of ttl) {
      files.push({ local: path.join(dir, bundleName, t), remote: `/effects/home/${bundleName}/${t}`, exec: false });
    }
  }

  if (!files.length) throw new Error(`unknown --target ${TARGET} (use home|guitar|both)`);
  await deployToDevice(files);
  console.log('[dsp-hotdeploy] done -- no OS rebuild, no netboot reflash, no reboot');
}

main().catch((e) => { console.error('[dsp-hotdeploy] FAILED:', e.message); process.exit(1); });
