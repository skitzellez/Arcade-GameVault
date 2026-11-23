#!/bin/bash
# ship_test.sh: bump the loader's version, build for Xbox, install. When the signed-in user (UserMgr1) does not get
# it (the console keeps the version that user last ran), the default account's copy is started once and a newer
# version is built and installed: that has always reached both accounts (2026-11-21).
cd /c/Users/GamerDad/XboxEmu/ArcadeLoader
export MSYS2_ENV_CONV_EXCL=XB_DIR XB_FAMILY=GamerDad.ArcadeLoaderTest
XB=/c/Users/GamerDad/XboxEmu/XemuUWP/tools/xb.py
for round in 1 2 3; do
  v=$(grep -o 'Version="0\.1\.0\.[0-9]*"' pkg/AppxManifest.xml | head -1 | grep -o '[0-9]*"$' | tr -d '"'); n=$((v+1))
  sed -i "s/Version=\"0.1.0.$v\"/Version=\"0.1.0.$n\"/" pkg/AppxManifest.xml
  cmd //c "C:\Users\GamerDad\XboxEmu\ArcadeLoader\build_xbox.bat" > out/build_xbox.log 2>&1
  tail -1 out/build_xbox.log | grep -q XBOX_BUILD_OK || { grep "error" out/build_xbox.log | head -5; exit 1; }
  timeout 30 python -I $XB stop >/dev/null 2>&1
  r=$(timeout 300 python -I $XB install "$PWD/dist/ArcadeLoader_x64.appx" | tail -1); echo "$r"
  echo "$r" | grep -q UserMgr1 && exit 0
  timeout 60 python -I $XB launch >/dev/null 2>&1; sleep 15; timeout 30 python -I $XB stop >/dev/null 2>&1; sleep 3
done
exit 1
