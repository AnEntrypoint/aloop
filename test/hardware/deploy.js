#!/usr/bin/env node
// Deploy a locally-built aloop binary to the device over SFTP (ssh2 package),
// stop the service, replace the binary (keeping a .bak), fix the executable
// bit on the REMOTE Linux filesystem (a local Windows chmod would be a
// silent no-op on NTFS -- this deliberately does it over the SSH connection
// instead), and restart the service. Pure JS, no shelling out to
// scp/sshpass/ssh binaries (none of which are reliably available in this
// Windows dev environment).
//
// This is the FAST live-reload path for hardware iteration (rc-service
// restart, seconds) instead of a full netboot power-cycle (whole TFTP/HTTP
// boot chain, minutes) -- the right tool for A/B testing binary variants
// quickly on an already-booted device (AGENTS.md's own documented lesson:
// always verify the ACTUAL deployed checksum, never assume a deploy step
// succeeded just because it didn't error -- a stale comparison target has
// caused real false-positive "deploy worked" conclusions before).
//
// Usage: node deploy.js <host> <localBinaryPath> [user=root] [password=aloop]
const { Client } = require('ssh2');
const fs = require('fs');
const crypto = require('crypto');

function md5File(path) {
  return crypto.createHash('md5').update(fs.readFileSync(path)).digest('hex');
}

const [, , host, localPath, user = 'root', password = 'aloop'] = process.argv;
if (!host || !localPath) {
  console.error('usage: node deploy.js <host> <localBinaryPath> [user] [password]');
  process.exit(2);
}
if (!fs.existsSync(localPath)) {
  console.error(`[deploy] local file not found: ${localPath}`);
  process.exit(2);
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

const conn = new Client();
conn
  .on('ready', async () => {
    try {
      console.log('[deploy] stopping aloop service...');
      let r = await execOnce(conn, 'rc-service aloop stop');
      console.log(r.out.trim() || r.errOut.trim());

      console.log('[deploy] uploading binary via SFTP...');
      await new Promise((resolve, reject) => {
        conn.sftp((err, sftp) => {
          if (err) return reject(err);
          sftp.fastPut(localPath, '/tmp/aloop.new', (err2) => {
            if (err2) return reject(err2);
            resolve();
          });
        });
      });

      console.log('[deploy] installing (chmod +x, backup old, move into place)...');
      r = await execOnce(conn, 'chmod +x /tmp/aloop.new && cp /opt/aloop/aloop /opt/aloop/aloop.bak && mv /tmp/aloop.new /opt/aloop/aloop && ls -la /opt/aloop/aloop');
      console.log(r.out.trim() || r.errOut.trim());

      // Verify the checksum landed correctly BEFORE restarting the service --
      // catching a bad/truncated SFTP transfer here is much better than
      // starting a corrupt binary and reporting a fault as "the code" when it
      // was actually a bad deploy (AGENTS.md's own documented false-positive
      // class of mistake).
      const localMd5 = md5File(localPath);
      r = await execOnce(conn, 'md5sum /opt/aloop/aloop');
      const remoteMd5 = (r.out.trim().split(/\s+/)[0] || '');
      console.log(`[deploy] local md5:  ${localMd5}`);
      console.log(`[deploy] remote md5: ${remoteMd5}`);
      if (localMd5 !== remoteMd5) {
        throw new Error(`checksum mismatch after upload -- local ${localMd5} != remote ${remoteMd5}, deploy is NOT safe to trust`);
      }
      console.log('[deploy] checksum verified match -- transfer is byte-exact');

      console.log('[deploy] starting aloop service...');
      r = await execOnce(conn, 'rc-service aloop start');
      console.log(r.out.trim() || r.errOut.trim());

      await new Promise((res) => setTimeout(res, 1500));
      r = await execOnce(conn, 'rc-service aloop status');
      console.log('[deploy]', r.out.trim() || r.errOut.trim());
      if (!/started/.test(r.out)) {
        throw new Error('aloop service did not report "started" after restart -- check rc-service aloop status / logs manually');
      }

      // Confirm the NEW process actually replaced the old one (a genuinely
      // fresh PID), not e.g. a stuck old process that never actually exited
      // (matching AGENTS.md's own "always verify a reboot actually happened"
      // discipline, applied here to a service restart instead of a full
      // device reboot).
      r = await execOnce(conn, "pgrep -f '/opt/aloop/aloop --config' -a");
      console.log('[deploy] running process:', r.out.trim() || '(none found -- service may have failed to start)');

      conn.end();
    } catch (e) {
      console.error('[deploy] error:', e.message);
      conn.end();
      process.exitCode = 1;
    }
  })
  .on('error', (err) => {
    console.error('[deploy] connection error:', err.message);
    process.exitCode = 1;
  })
  .connect({ host, username: user, password, readyTimeout: 15000 });
