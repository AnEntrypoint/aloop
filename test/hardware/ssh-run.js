#!/usr/bin/env node
// Run a remote command on the aloop device over SSH using the ssh2 npm
// package directly (no shelling out to an ssh/sshpass binary) -- avoids
// Windows Git-Bash/OpenSSH quoting and process-spawning quirks entirely.
//
// Usage: node ssh-run.js <host> "<command>" [user=root] [password=aloop]
const { Client } = require('ssh2');

const [, , host, command, user = 'root', password = 'aloop'] = process.argv;
if (!host || !command) {
  console.error('usage: node ssh-run.js <host> "<command>" [user] [password]');
  process.exit(2);
}

const conn = new Client();
conn
  .on('ready', () => {
    conn.exec(command, (err, stream) => {
      if (err) { console.error('[ssh-run] exec error:', err.message); conn.end(); process.exitCode = 1; return; }
      let out = '';
      let errOut = '';
      stream
        .on('close', (code) => {
          if (out) process.stdout.write(out);
          if (errOut) process.stderr.write(errOut);
          conn.end();
          process.exitCode = code || 0;
        })
        .on('data', (data) => { out += data.toString(); })
        .stderr.on('data', (data) => { errOut += data.toString(); });
    });
  })
  .on('error', (err) => {
    console.error('[ssh-run] connection error:', err.message);
    process.exitCode = 1;
  })
  .connect({ host, username: user, password, readyTimeout: 15000 });
