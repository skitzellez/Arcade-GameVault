#!/bin/bash
# gv_sweep.sh "<title part>" <name> [secs] [exe part]: starts a game from Game Vault (its autoplay.txt test hook, as if A
# was pressed on it), waits until the arcade loader's log names that game's program (up to 3 minutes: Game Vault may be
# searching first), then [secs] more, and keeps a console screenshot and the loader's log in out/sweep/<name>.*
cd /c/Users/GamerDad/XboxEmu/ArcadeLoader
export MSYS2_ENV_CONV_EXCL=XB_DIR
XB=/c/Users/GamerDad/XboxEmu/XemuUWP/tools/xb.py
S=out/sweep; secs=${3:-80}
printf '%s' "$1" > $S/autoplay.txt
XB_FAMILY=GamerDad.GameVault XB_DIR=/LocalState timeout 60 python -I $XB put "$(cygpath -w $S/autoplay.txt)" >/dev/null 2>&1
want=${4:-}
if [ -n "$want" ]; then
  for i in $(seq 1 18); do
    timeout 10 tail -f /dev/null
    XB_FAMILY=GamerDad.ArcadeLoaderTest XB_DIR=/LocalState timeout 60 python -I $XB get arcade_log.txt "$(cygpath -w $S/$2.log)" >/dev/null 2>&1
    grep -a '^=== ' $S/$2.log 2>/dev/null | tail -1 | grep -qi "$want" && break
  done
fi
for i in $(seq 1 $((secs/10))); do timeout 10 tail -f /dev/null; done
XB_FAMILY=GamerDad.ArcadeLoaderTest XB_DIR=/LocalState timeout 60 python -I $XB get arcade_log.txt "$(cygpath -w $S/$2.log)" >/dev/null 2>&1
cd /c/Users/GamerDad/XboxEmu/XemuUWP/tools && timeout 60 python -I xb.py shot "$(cygpath -w /c/Users/GamerDad/XboxEmu/ArcadeLoader/$S/$2.png)" >/dev/null 2>&1
cd /c/Users/GamerDad/XboxEmu/ArcadeLoader
echo "== $2: $(grep -a '^=== ' $S/$2.log | tail -1 | cut -c1-140)"
grep -a "presents in\|exception\|game profile" $S/$2.log | tail -3 | cut -c1-150
XB_FAMILY=GamerDad.ArcadeLoaderTest timeout 30 python -I $XB stop >/dev/null 2>&1
XB_FAMILY=GamerDad.GameVault timeout 60 python -I $XB launch >/dev/null 2>&1
for i in 1 2 3; do timeout 10 tail -f /dev/null; done
