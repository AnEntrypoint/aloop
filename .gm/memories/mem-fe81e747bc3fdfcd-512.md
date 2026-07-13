---
key: mem-fe81e747bc3fdfcd-512
ns: default
created: 1783985222903
updated: 1783985222903
---

image/serve-netboot-win.js pins the real Git-Bash executable path explicitly (checks C:\Program Files\Git\bin\bash.exe and usr\bin variants) instead of a bare 'bash' PATH lookup when invoking build-netboot.sh, because this Windows host has 3 different bash.exe on PATH (Git-Bash, the WSL launcher stub under System32, a WindowsApps alias) and a bare lookup can resolve to the WSL stub, which cannot see C:/... paths at all and fails with a misleading "No such file or directory" on a path that verifiably exists.
