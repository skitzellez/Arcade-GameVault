#!/bin/sh
# Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0.
# Testing on the owner's console: xbox_run.sh [install] [only-text] [wait-seconds]
# installs dist\ArcadeLoader_x64.appx (with "install"), writes only.txt, starts the loader test app, waits, and
# fetches arcade_log.txt (+ mesa_log.txt and DXVK's log when there) into out\xbox\.
cd "$(dirname "$0")/.." || exit 1
OUT="$PWD/out/xbox"
mkdir -p "$OUT"
XB=/c/Users/GamerDad/XboxEmu/XemuUWP/tools/xb.py
export MSYS2_ENV_CONV_EXCL=XB_DIR XB_FAMILY=GamerDad.ArcadeLoaderTest XB_DIR=/LocalState
timeout 30 python -I $XB stop >/dev/null 2>&1
if [ "$1" = install ]; then
    timeout 300 python -I $XB install "$PWD/dist/ArcadeLoader_x64.appx" | tail -1
    shift
fi
ONLY="${1:-}"
WAIT="${2:-45}"
cd "$OUT" || exit 1
rm -f arcade_log.txt mesa_log.txt _d3d9.log
printf '%s' "$ONLY" > only.txt
timeout 60 python -I $XB put only.txt >/dev/null
timeout 60 python -I $XB launch >/dev/null
sleep "$WAIT"
for f in arcade_log.txt mesa_log.txt _d3d9.log; do
    timeout 60 python -I $XB get "$f" "$f" >/dev/null 2>&1
done
ls -la "$OUT"
