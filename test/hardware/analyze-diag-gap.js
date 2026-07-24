#!/usr/bin/env node
// Parses timestamped [diag-gap] log lines (src/dsp/audio_thread.cpp's
// wall-clock-stamped gap logging, added this session specifically because
// line-count density is NOT a reliable proxy for real elapsed time -- see
// AGENTS.md's "Diagnosing periodic audio stalls" caveat) and computes the
// REAL inter-event period automatically, instead of eyeballing timestamps by
// hand every time. Built for the 1Hz-stall bisection: run once against a
// baseline build's log and once against a candidate fix's log, compare.
//
// Usage:
//   node ssh-exec.js <host> "cat /var/log/aloop.log" > /tmp/log.txt
//   node analyze-diag-gap.js /tmp/log.txt
//
// Or pipe directly:
//   node ssh-exec.js <host> "cat /var/log/aloop.log" | node analyze-diag-gap.js -

const fs = require('fs');

const [, , inputPath] = process.argv;
if (!inputPath) {
  console.error('usage: node analyze-diag-gap.js <log-file>   (or "-" for stdin)');
  process.exit(2);
}

function readInput() {
  if (inputPath === '-') return fs.readFileSync(0, 'utf8');
  return fs.readFileSync(inputPath, 'utf8');
}

// Matches BOTH diag-gap line shapes this session's logging emits:
//   [diag-gap] t=26.037 readi gap=33.437ms (expected ~1.333ms)
//   [diag-gap] t=26.039 readi ITSELF took=2.291ms (expected ~1.333ms)
const LINE_RE = /\[diag-gap\] t=(\d+)\.(\d+) readi (gap|ITSELF took)=([\d.]+)ms \(expected ~([\d.]+)ms\)/;

function parse(text) {
  const events = [];
  for (const line of text.split('\n')) {
    const m = LINE_RE.exec(line);
    if (!m) continue;
    const [, sec, msFrac, kind, magnitude, expected] = m;
    const t = parseInt(sec, 10) + parseInt(msFrac, 10) / 1000;
    events.push({ t, kind: kind === 'gap' ? 'gap' : 'itself', magnitudeMs: parseFloat(magnitude), expectedMs: parseFloat(expected) });
  }
  return events;
}

function analyze(events) {
  if (events.length === 0) {
    console.log('No [diag-gap] lines found -- either a clean run (good) or the log format changed.');
    return;
  }
  // Focus on the "big" events (gap events specifically, since ITSELF-took
  // lines are paired 1:1 with most gap lines and would double-count the
  // same underlying stall).
  const gapEvents = events.filter((e) => e.kind === 'gap');
  const bigThresholdMs = 10; // anything under 10ms is routine block-scheduling noise, not the stall being hunted
  const bigEvents = gapEvents.filter((e) => e.magnitudeMs >= bigThresholdMs);

  console.log(`Total diag-gap lines: ${events.length} (${gapEvents.length} gap, ${events.length - gapEvents.length} ITSELF-took)`);
  console.log(`"Big" gap events (>=${bigThresholdMs}ms): ${bigEvents.length} out of ${gapEvents.length} gap events`);

  if (bigEvents.length < 2) {
    console.log('Fewer than 2 big events -- cannot compute a period. If 0, the stall may genuinely be absent in this log.');
    return;
  }

  const intervals = [];
  for (let i = 1; i < bigEvents.length; i++) {
    intervals.push(bigEvents[i].t - bigEvents[i - 1].t);
  }
  const mean = intervals.reduce((a, b) => a + b, 0) / intervals.length;
  const variance = intervals.reduce((a, b) => a + (b - mean) ** 2, 0) / intervals.length;
  const stddev = Math.sqrt(variance);
  const sorted = [...intervals].sort((a, b) => a - b);
  const median = sorted[Math.floor(sorted.length / 2)];

  console.log(`Inter-event intervals (seconds): n=${intervals.length}, mean=${mean.toFixed(4)}, median=${median.toFixed(4)}, stddev=${stddev.toFixed(4)}`);
  console.log(`Min=${Math.min(...intervals).toFixed(4)}s, Max=${Math.max(...intervals).toFixed(4)}s`);

  const isRegular = stddev < mean * 0.1;   // within 10% of mean = "regular period", not noise
  if (isRegular) {
    console.log(`VERDICT: REGULAR PERIOD ~${mean.toFixed(3)}s (stddev is <10% of mean) -- matches a genuine periodic cause, not random jitter.`);
  } else {
    console.log('VERDICT: IRREGULAR spacing (stddev >=10% of mean) -- likely bursty/load-dependent, not a clean fixed-period cause.');
  }

  const magMean = bigEvents.reduce((a, e) => a + e.magnitudeMs, 0) / bigEvents.length;
  console.log(`Mean magnitude of big events: ${magMean.toFixed(2)}ms (expected block period: ${bigEvents[0].expectedMs}ms)`);
}

analyze(parse(readInput()));
