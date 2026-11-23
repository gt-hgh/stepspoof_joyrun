#!/bin/bash
# Build and package Zygisk module in WSL
set -euo pipefail

NDK="${ANDROID_NDK_R27D:-$HOME/android/android-ndk-r27d}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build"
DIST="$ROOT/dist"
MODID="stepspoof_joyrun"
VER="v1.3.8"

echo "[*] NDK   = $NDK"
echo "[*] ROOT  = $ROOT"

if [ ! -x "$NDK/ndk-build" ]; then
  echo "[!] ndk-build not found: $NDK/ndk-build"
  exit 1
fi

rm -rf "$OUT" "$DIST"
mkdir -p "$OUT" "$DIST"

"$NDK/ndk-build" \
  NDK_PROJECT_PATH="$ROOT" \
  APP_BUILD_SCRIPT="$ROOT/jni/Android.mk" \
  NDK_APPLICATION_MK="$ROOT/jni/Application.mk" \
  NDK_OUT="$OUT/obj" \
  NDK_LIBS_OUT="$OUT/libs" \
  -j"$(nproc)"

SO="$OUT/libs/arm64-v8a/libstepspoof.so"
if [ ! -f "$SO" ]; then
  echo "[!] build failed, $SO not found"
  exit 1
fi
echo "[*] built: $SO ($(stat -c%s "$SO") bytes)"

STAGE="$OUT/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/META-INF/com/google/android" "$STAGE/zygisk"

cp "$ROOT/module.prop" "$STAGE/module.prop"
cp "$ROOT/META-INF/com/google/android/update-binary" "$STAGE/META-INF/com/google/android/"
cp "$ROOT/META-INF/com/google/android/updater-script" "$STAGE/META-INF/com/google/android/"
cp "$SO" "$STAGE/zygisk/arm64-v8a.so"

# service.sh: sync user-0 config to user-999 on boot
if [ -f "$ROOT/service.sh" ]; then
  cp "$ROOT/service.sh" "$STAGE/service.sh"
  chmod 0755 "$STAGE/service.sh"
  echo "[*] service.sh included"
fi

# KSU WebUI
if [ -d "$ROOT/webroot" ]; then
  cp -r "$ROOT/webroot" "$STAGE/webroot"
  echo "[*] webroot included"
fi

ZIP="$DIST/${MODID}-${VER}.zip"
( cd "$STAGE" && zip -r -q "$ZIP" . )
echo "[*] module zip: $ZIP ($(stat -c%s "$ZIP") bytes)"
echo "[*] done"