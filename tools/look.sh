#!/bin/bash
# look.sh [name]: the arcade loader's log (key lines, newest) + a console screenshot (scratchpad/<name>.png)
cd /c/Users/GamerDad/XboxEmu/ArcadeLoader/out/xbox
export MSYS2_ENV_CONV_EXCL=XB_DIR XB_FAMILY=GamerDad.ArcadeLoaderTest XB_DIR=/LocalState
XB=/c/Users/GamerDad/XboxEmu/XemuUWP/tools/xb.py
timeout 30 python -I $XB get arcade_log.txt arcade_log.txt >/dev/null 2>&1
grep -av "^memory\|32-bit call\|loads api-ms\|^  missing\|F0 D9 CB" arcade_log.txt | tail -${2:-15} | cut -c1-170
S="$TEMP/claude/761572d41c696ec8/bccf477d-216b-49d7-a5ad-4659bbc4af57/scratchpad"
cd /c/Users/GamerDad/XboxEmu/XemuUWP/tools && timeout 60 python -I xb.py shot "$(cygpath -w "$S")\\${1:-look}.png" | tail -1
