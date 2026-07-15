#!/usr/bin/env node
// Deploy a locally-built aloop binary to the device over SFTP (ssh2 package),
// stop the service, replace the binary (keeping a .bak), fix the executable
// bit on the REMOTE Linux filesystem (a local Windows chmod would be a
// silent no-op on NTFS -- this deliberately does it over the SSH connection
// instead), and restart the service. Pure JS, no shelling out to
// scp/sshpass/ssh binaries (none of which are reliably available in this
// Windows dev environment).
//
// Usage: node deploy.js <host> <localBinaryPath> [user=root] [password=aloop]
const { Client } = require('ssh2');
const fs = require('fs');

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

      console.log('[deploy] starting aloop service...');
      r = await execOnce(conn, 'rc-service aloop start');
      console.log(r.out.trim() || r.errOut.trim());

      await new Promise((res) => setTimeout(res, 1500));
      r = await execOnce(conn, 'rc-service aloop status');
      console.log('[deploy]', r.out.trim() || r.errOut.trim());

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
