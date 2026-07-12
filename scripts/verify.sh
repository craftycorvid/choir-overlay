#!/usr/bin/env bash
# scripts/verify.sh — the repo's verify contract. The powers verify-gate runs
# this when a subagent finishes; non-zero exit blocks the subagent.
set -euo pipefail
cd "$(dirname "$0")/.."

[ -d build ] || meson setup build . --buildtype=release
meson test -C build --print-errorlogs
