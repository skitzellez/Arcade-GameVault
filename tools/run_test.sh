#!/bin/bash
# run_test.sh <LocalState path of the exe> [seconds]: runs a game in the arcade loader test app, prints the log's key lines
cd /c/Users/GamerDad/XboxEmu/ArcadeLoader/out/xbox
export MSYS2_ENV_CONV_EXCL=XB_DIR XB_FAMILY=GamerDad.ArcadeLoaderTest XB_DIR=/LocalState
XB=/c/Users/GamerDad/XboxEmu/XemuUWP/tools/xb.py
printf '%s' "$1" > run.txt
timeout 30 python -I $XB stop >/dev/null 2>&1
timeout 60 python -I $XB put run.txt >/dev/null
timeout 60 python -I $XB rm arcade_log.txt >/dev/null 2>&1
rm -f arcade_log.txt
timeout 60 python -I $XB launch | tail -1
for i in $(seq 1 $(( ${2:-60} / 5 ))); do sleep 5; timeout 30 python -I $XB get arcade_log.txt arcade_log.txt >/dev/null 2>&1; grep -aqE "exception 0x|program ended|could not read|frames:" arcade_log.txt 2>/dev/null && break; done
sleep 3; timeout 30 python -I $XB get arcade_log.txt arcade_log.txt >/dev/null 2>&1
head -1 arcade_log.txt
grep -av "^memory\|32-bit call\|loads api-ms\|^  missing\|^NAS: [a-z]" arcade_log.txt | grep -aiE "could not|crypto|mapping|registry|calls missing|exception|ended|frames|  32-bit stack|stack:|asks for|D3D9: [0-9]|Direct3DCreate|vkCreateDevice|message box|error" | head -${3:-40} | cut -c1-220
