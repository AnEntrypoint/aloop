#!/usr/bin/env node
// Single entry point for all of this session's device-dependent
// verifications, so the next on-device session is exactly "run one command,
// read the results" instead of re-deriving which script to run in what
// order. Runs, in sequence:
//   1. verify-glitch-no-shift.js -- glitch/SHIFT independence (control-
//      surface/state level; fast, no side effects on existing loop content).
//   2. verify-quantization.js -- loop-1-exact-length + power-of-2 snapping
//      spec (records 3 loops on whatever looper slots 0-2 currently hold;
//      see its own header for the "assumes a clear rig" caveat).
//   3. bisect-1hz-stall.js -- the disable_core3_lv2 A/B test (restarts the
//      aloop service twice, ~2x captureSeconds total wall time; restores
//      the original config when done regardless of outcome).
//
// Each script's own exit code is preserved in the final summary; a failure
// in one does NOT stop the others (so a full report is produced even if,
// say, the quantization fix still needs another pass) -- but the script's
// own process exit code is non-zero if ANY verification failed, so this is
// still safe to use as a single pass/fail gate in a larger workflow.
//
// Usage: node run-all-verifications.js <host> [bisectCaptureSeconds=15]

const { spawn } = require('child_process');
const path = require('path');

const [, , host, bisectSeconds] = process.argv;
if (!host) {
  console.error('usage: node run-all-verifications.js <host> [bisectCaptureSeconds=15]');
  process.exit(2);
}

function run(scriptName, args) {
  return new Promise((resolve) => {
    console.log(`\n${'='.repeat(70)}\nRunning ${scriptName} ${args.join(' ')}\n${'='.repeat(70)}`);
    const child = spawn(process.execPath, [path.join(__dirname, scriptName), ...args], { stdio: 'inherit' });
    child.on('close', (code) => resolve({ scriptName, code }));
    child.on('error', (err) => { console.error(`[run-all] failed to launch ${scriptName}:`, err.message); resolve({ scriptName, code: 1 }); });
  });
}

async function main() {
  const results = [];
  results.push(await run('verify-glitch-no-shift.js', [host]));
  results.push(await run('verify-quantization.js', [host]));
  results.push(await run('bisect-1hz-stall.js', [host, bisectSeconds || '15']));

  console.log(`\n${'='.repeat(70)}\nSUMMARY\n${'='.repeat(70)}`);
  let anyFailed = false;
  for (const r of results) {
    // bisect-1hz-stall.js has no fixed pass/fail (it's a diagnostic
    // comparison, not a boolean check) -- exit code 0 there just means "ran
    // to completion without an error," so report it distinctly.
    const isBisect = r.scriptName === 'bisect-1hz-stall.js';
    const label = isBisect ? (r.code === 0 ? 'RAN (read its own verdict above)' : 'ERRORED') : (r.code === 0 ? 'PASS' : 'FAIL');
    console.log(`${r.scriptName}: ${label} (exit ${r.code})`);
    if (r.code !== 0) anyFailed = true;
  }
  process.exit(anyFailed ? 1 : 0);
}

main();
