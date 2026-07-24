#!/usr/bin/env node
// Fully automated 1Hz-stall bisection using the disable_core3_lv2 config
// toggle added this session (src/dsp/audio_thread.h/config/aloop.conf) --
// no manual editing/restarting/log-reading required. Compares the
// [diag-gap] timestamped stall pattern (analyze-diag-gap.js) WITH and
// WITHOUT the Core-3 LV2 host's per-block process() calls, to isolate
// whether that code path is involved in the ~30-37ms/~1.000s periodic
// stall (CPU governor and Ableton Link already ruled out this session via
// direct A/B tests).
//
// Usage: node bisect-1hz-stall.js <host> [captureSeconds=15]
//
// What it does, in order:
//   1. Read /etc/aloop.conf, confirm disable_core3_lv2 is NOT already set
//      (or warn + use whatever state it finds -- never silently assumes).
//   2. Clear /var/log/aloop.log's rotation point by restarting the service
//      fresh (rc-service aloop restart), wait for it to report "started".
//   3. Capture ~captureSeconds of the BASELINE log (disable_core3_lv2
//      absent/0 -- Core-3 host active, matching current shipped default).
//   4. Append "disable_core3_lv2 = 1" to /etc/aloop.conf, restart, capture
//      the same window with the Core-3 host path skipped.
//   5. Restore /etc/aloop.conf to its original content (byte-for-byte,
//      via a saved backup) and restart ONE more time, so the device is
//      left in its normal, un-modified state regardless of the bisection
//      result.
//   6. Run analyze-diag-gap.js's own parsing/verdict logic against both
//      captures and print a side-by-side comparison.
//
// Requires `npm install` in this directory first (ssh2 dependency).

const { Client } = require('ssh2');
const fs = require('fs');
const path = require('path');

const [, , host, capSecondsArg] = process.argv;
if (!host) {
  console.error('usage: node bisect-1hz-stall.js <host> [captureSeconds=15]');
  process.exit(2);
}
const captureSeconds = parseInt(capSecondsArg || '15', 10);
const CONF_PATH = '/etc/aloop.conf';
const LOG_PATH = '/var/log/aloop.log';

function connect() {
  return new Promise((resolve, reject) => {
    const conn = new Client();
    conn.on('ready', () => resolve(conn)).on('error', reject)
      .connect({ host, username: 'root', password: 'aloop', readyTimeout: 15000 });
  });
}

function execOnce(conn, command) {
  return new Promise((resolve, reject) => {
    conn.exec(command, (err, stream) => {
      if (err) return reject(err);
      let out = '', errOut = '';
      stream
        .on('close', (code) => resolve({ code, out, errOut }))
        .on('data', (d) => { out += d.toString(); })
        .stderr.on('data', (d) => { errOut += d.toString(); });
    });
  });
}

// Writes config content via SFTP (deploy.js's own proven pattern), never by
// shell-quoting arbitrary file content into an exec() command -- avoids any
// risk of a stray quote/special character in aloop.conf breaking a
// hand-escaped printf string, and needs no escaping logic to reason about
// at all.
function writeFileContent(conn, remotePath, content) {
  return new Promise((resolve, reject) => {
    conn.sftp((err, sftp) => {
      if (err) return reject(err);
      const tmpPath = remotePath + '.tmp-bisect';
      const writeStream = sftp.createWriteStream(tmpPath);
      writeStream.on('close', () => {
        // Atomic-ish rename into place once the write is fully flushed.
        sftp.rename(tmpPath, remotePath, (err2) => (err2 ? reject(err2) : resolve()));
      });
      writeStream.on('error', reject);
      writeStream.end(content);
    });
  });
}

async function restartAndWait(conn) {
  await execOnce(conn, 'rc-service aloop restart');
  // Poll for "started" -- bounded retries, matching this session's own
  // documented dead-watcher-recovery discipline (never an unbounded/blind
  // wait). 20 tries * 1.5s = 30s max, generous for a real service restart.
  for (let i = 0; i < 20; i++) {
    const r = await execOnce(conn, 'rc-service aloop status');
    if (/started/.test(r.out)) return true;
    await new Promise((res) => setTimeout(res, 1500));
  }
  return false;
}

async function captureLogWindow(conn, seconds) {
  // Truncate the log, wait the capture window, then read it -- gives a
  // clean, bounded sample instead of re-parsing however much history
  // happened to accumulate before this run.
  await execOnce(conn, `: > ${LOG_PATH}`);
  console.log(`[bisect] capturing ${seconds}s of log...`);
  await new Promise((res) => setTimeout(res, seconds * 1000));
  const r = await execOnce(conn, `cat ${LOG_PATH}`);
  return r.out;
}

// Inlined from analyze-diag-gap.js (kept as a single self-contained script
// so this bisection tool has no cross-file require() path issues over an
// SSH-driven remote workflow) -- see that file for the fuller comment
// explaining WHY wall-clock timestamps matter here.
const LINE_RE = /\[diag-gap\] t=(\d+)\.(\d+) readi (gap|ITSELF took)=([\d.]+)ms \(expected ~([\d.]+)ms\)/;
function parseDiagGap(text) {
  const events = [];
  for (const line of text.split('\n')) {
    const m = LINE_RE.exec(line);
    if (!m) continue;
    const [, sec, msFrac, kind, magnitude] = m;
    events.push({ t: parseInt(sec, 10) + parseInt(msFrac, 10) / 1000, kind: kind === 'gap' ? 'gap' : 'itself', magnitudeMs: parseFloat(magnitude) });
  }
  return events;
}
function summarize(label, text) {
  const events = parseDiagGap(text);
  const gapEvents = events.filter((e) => e.kind === 'gap');
  const bigEvents = gapEvents.filter((e) => e.magnitudeMs >= 10);
  let periodStr = 'n/a (fewer than 2 big events)';
  if (bigEvents.length >= 2) {
    const intervals = [];
    for (let i = 1; i < bigEvents.length; i++) intervals.push(bigEvents[i].t - bigEvents[i - 1].t);
    const mean = intervals.reduce((a, b) => a + b, 0) / intervals.length;
    const variance = intervals.reduce((a, b) => a + (b - mean) ** 2, 0) / intervals.length;
    const stddev = Math.sqrt(variance);
    const regular = stddev < mean * 0.1;
    periodStr = `mean=${mean.toFixed(3)}s stddev=${stddev.toFixed(4)} (${regular ? 'REGULAR' : 'irregular'})`;
  }
  console.log(`[bisect] ${label}: ${events.length} diag-gap lines, ${bigEvents.length}/${gapEvents.length} big (>=10ms) gap events, period: ${periodStr}`);
  return { bigCount: bigEvents.length, totalGapCount: gapEvents.length };
}

async function main() {
  console.log(`[bisect] target=${host}, captureSeconds=${captureSeconds}`);
  const conn = await connect();
  try {
    const confRead = await execOnce(conn, `cat ${CONF_PATH}`);
    if (confRead.code !== 0) throw new Error(`could not read ${CONF_PATH}: ${confRead.errOut}`);
    const originalConf = confRead.out;
    if (/disable_core3_lv2\s*=\s*1/.test(originalConf)) {
      console.warn('[bisect] WARNING: disable_core3_lv2=1 already present in the live config -- this run will still restore whatever was there, but the "baseline" capture below is NOT a true Core-3-enabled baseline.');
    }
    const backupPath = path.join(__dirname, `aloop.conf.backup.${Date.now()}`);
    fs.writeFileSync(backupPath, originalConf);
    console.log(`[bisect] saved a local backup of the live config -> ${backupPath}`);

    console.log('[bisect] === BASELINE (Core-3 LV2 host active, current shipped default) ===');
    if (!(await restartAndWait(conn))) throw new Error('service did not report started after baseline restart');
    const baselineLog = await captureLogWindow(conn, captureSeconds);
    const baselineSummary = summarize('BASELINE', baselineLog);

    console.log('[bisect] === CANDIDATE (disable_core3_lv2=1, Core-3 host path skipped) ===');
    const candidateConf = originalConf.replace(/\n?# ?disable_core3_lv2 = 1.*$/m, '') + '\ndisable_core3_lv2 = 1\n';
    await writeFileContent(conn, CONF_PATH, candidateConf);
    if (!(await restartAndWait(conn))) throw new Error('service did not report started after candidate restart');
    const candidateLog = await captureLogWindow(conn, captureSeconds);
    const candidateSummary = summarize('CANDIDATE', candidateLog);

    console.log('[bisect] === RESTORING original config ===');
    await writeFileContent(conn, CONF_PATH, originalConf);
    if (!(await restartAndWait(conn))) console.warn('[bisect] WARNING: service did not report started after restore restart -- check the device manually.');
    else console.log('[bisect] device restored to its original config and confirmed started.');

    console.log('[bisect] === VERDICT ===');
    if (baselineSummary.bigCount === 0 && candidateSummary.bigCount === 0) {
      console.log('[bisect] Neither capture showed the stall -- either it is intermittent beyond this capture window, or it is genuinely fixed. Re-run with a longer captureSeconds to be sure.');
    } else if (baselineSummary.bigCount > 0 && candidateSummary.bigCount === 0) {
      console.log('[bisect] Stall present in BASELINE, ABSENT with disable_core3_lv2=1 -- strong evidence the Core-3 LV2 host code path IS involved.');
    } else if (baselineSummary.bigCount === 0 && candidateSummary.bigCount > 0) {
      console.log('[bisect] Stall ABSENT in baseline, present with disable_core3_lv2=1 -- unexpected; the toggle itself may have introduced something, or this is noise. Re-run to confirm.');
    } else {
      console.log('[bisect] Stall present in BOTH captures -- the Core-3 LV2 host code path is NOT the (sole) cause; look elsewhere.');
    }
  } finally {
    conn.end();
  }
}

main().catch((err) => {
  console.error('[bisect] error:', err.message);
  process.exit(1);
});
