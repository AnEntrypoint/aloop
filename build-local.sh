#!/bin/sh
# Resilient local build for aloop, replacing GitHub Actions CI while its
# artifact-storage quota is blocked. Builds two persistent, cached Docker
# images ONCE (aloop-codegen for Faust codegen, aloop-arm64build for the
# arm64/musl cross-compile) instead of re-downloading the whole toolchain
# on every invocation -- this is the actual fix for the repeated-download
# waste this session hit. Each network-dependent step is retried with
# backoff to ride out the transient connectivity gaps witnessed this
# session (a `wget` that hung twice succeeded on a 3rd retry -- real,
# short-lived gaps, not a persistent block), with a hard per-attempt
# timeout so a genuine hang doesn't stall forever undetected.
#
# Usage: ./build-local.sh [--codegen-only|--compile-only]
set -eu
cd "$(dirname "$0")"

MSYS_NO_PATHCONV=1
export MSYS_NO_PATHCONV

retry() {
  # retry <timeout_seconds> <description> -- <command...>
  timeout_s="$1"; desc="$2"; shift 2
  [ "$1" = "--" ] && shift
  attempt=1
  max_attempts=4
  while [ "$attempt" -le "$max_attempts" ]; do
    echo "[build-local] $desc (attempt $attempt/$max_attempts, ${timeout_s}s timeout)"
    if timeout "$timeout_s" "$@"; then
      return 0
    fi
    echo "[build-local] $desc failed/timed out on attempt $attempt"
    attempt=$((attempt + 1))
    [ "$attempt" -le "$max_attempts" ] && sleep $((attempt * 5))
  done
  echo "[build-local] $desc FAILED after $max_attempts attempts -- giving up"
  return 1
}

ensure_codegen_image() {
  if ! docker image inspect aloop-codegen >/dev/null 2>&1; then
    retry 300 "build aloop-codegen image" -- \
      docker build -f Dockerfile.codegen -t aloop-codegen .
  else
    echo "[build-local] aloop-codegen image already cached, skipping build"
  fi
}

ensure_arm64build_image() {
  if ! docker image inspect aloop-arm64build >/dev/null 2>&1; then
    retry 300 "build aloop-arm64build image" -- \
      docker build --platform linux/arm64 -f Dockerfile.arm64build -t aloop-arm64build .
  else
    echo "[build-local] aloop-arm64build image already cached, skipping build"
  fi
}

do_codegen() {
  ensure_codegen_image
  mkdir -p build
  # -vec -fun -dfs -vs 32: vectorized codegen (separated simpler loops over a
  # fixed-size internal buffer instead of one big scalar loop), function
  # inlining, depth-first scheduling -- Faust's own documented standard
  # optimization recipe (faustdoc.grame.fr/manual/optimizing/). -nvi drops the
  # 'virtual' keyword from the generated C++ class (Faust's own docs: "can be
  # especially useful in embedded devices context", directly on point for the
  # Pi 4 target) -- pure codegen-strategy flags, no DSP math change, so the
  # signal graph is bit-identical; only how the compiler schedules/inlines it
  # differs.
  retry 60 "faust codegen" -- \
    docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w aloop-codegen \
      faust -lang cpp -vec -fun -dfs -vs 32 -nvi -cn AloopLoopDsp -I dsp -I effects/home/faust dsp/aloop.dsp -o build/loop.cpp
  echo "[build-local] codegen done: $(wc -l < build/loop.cpp) lines"
}

do_compile() {
  ensure_arm64build_image
  retry 180 "arm64 cross-compile" -- \
    docker run --rm --platform linux/arm64 -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w aloop-arm64build \
      sh -c 'cmake --build build -j"$(nproc)"'
  ls -la build/aloop
  echo "[build-local] compile done"
}

case "${1:-}" in
  --codegen-only) do_codegen ;;
  --compile-only) do_compile ;;
  *) do_codegen; do_compile ;;
esac
