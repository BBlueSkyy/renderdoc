#!/usr/bin/env bash
# Build the standalone Android inspector after the upstream ARM64 core is built.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDK="${ANDROID_HOME:?Set ANDROID_HOME}"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK:?Set ANDROID_NDK or ANDROID_NDK_HOME}}"
APP="$ROOT/mobile/android"
BUILD="$ROOT/build-renderdoc-mobile"
CORE="$ROOT/build-android-arm64"

if [[ ! -f "$CORE/lib/libVkLayer_GLES_RenderDoc.so" ]]; then
  echo "Build the RenderDoc Android ARM64 core first." >&2
  exit 1
fi
TOOLS="$SDK/build-tools/26.0.1"
JAR="$SDK/platforms/android-23/android.jar"
if [[ ! -f "$JAR" || ! -x "$TOOLS/aapt" ]]; then
  echo "Android SDK platform 23 and build-tools 26.0.1 are required." >&2
  exit 1
fi

cmake -S "$APP/native" -B "$BUILD/native" \
  "-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DANDROID_STL=c++_static \
  "-DRENDERDOC_SOURCE_ROOT=$ROOT" \
  "-DRENDERDOC_CORE_LIBRARY=$CORE/lib/libVkLayer_GLES_RenderDoc.so" \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD/native" --parallel 2

# Validate replay-mode registration before shipping an APK:
# the ELF symbol and DT_FLAGS_1 GLOBAL flag must both be present.
MARKER="$BUILD/native/lib/librenderdoc_replay_marker.so"
readelf -Ws "$MARKER" | grep -q "renderdoc__replay__marker" || {
  echo "Replay marker symbol missing from bootstrap library" >&2; exit 1;
}
readelf -d "$MARKER" | grep -q "GLOBAL" || {
  echo "Replay marker library not exported to Android's global linker group" >&2; exit 1;
}

STAGE="$BUILD/package"
mkdir -p "$STAGE/gen" "$STAGE/obj" "$STAGE/bin" "$STAGE/libs/lib/arm64-v8a"

"$TOOLS/aapt" package -f -m -J "$STAGE/gen" \
  -M "$APP/AndroidManifest.xml" -S "$APP/res" -I "$JAR"

javac -source 7 -target 7 \
  -bootclasspath "$JAVA_HOME/jre/lib/rt.jar" \
  -classpath "$JAR" -d "$STAGE/obj" \
  "$APP/src/org/renderdoc/mobile/MainActivity.java" \
  "$APP/src/org/renderdoc/mobile/DemoCaptureActivity.java" \
  "$APP/src/org/renderdoc/mobile/ReplayActivity.java" \
  "$APP/src/org/renderdoc/mobile/ReplayDiagnostics.java" \
  "$APP/src/org/renderdoc/mobile/ReplayMarker.java" \
  "$STAGE/gen/org/renderdoc/mobile/R.java"

"$TOOLS/dx" --dex --output="$STAGE/bin/classes.dex" "$STAGE/obj"

cp "$CORE/lib/libVkLayer_GLES_RenderDoc.so" "$STAGE/libs/lib/arm64-v8a/"
cp "$BUILD/native/lib/librenderdoc_mobile_jni.so" "$STAGE/libs/lib/arm64-v8a/"
cp "$BUILD/native/lib/librenderdoc_replay_jni.so" "$STAGE/libs/lib/arm64-v8a/"
cp "$BUILD/native/lib/librenderdoc_replay_marker.so" "$STAGE/libs/lib/arm64-v8a/"
cp "$BUILD/native/lib/librenderdoc_vulkan_demo.so" "$STAGE/libs/lib/arm64-v8a/"

(
  cd "$STAGE"
  "$TOOLS/aapt" package -f -M "$APP/AndroidManifest.xml" -S "$APP/res" \
    -I "$JAR" --version-code 5 --version-name "0.3.2" \
    -F unsigned.apk bin libs
  "$TOOLS/zipalign" -f 4 unsigned.apk aligned.apk

  KEYSTORE="$CORE/renderdoccmd/debug.keystore"
  if [[ ! -f "$KEYSTORE" ]]; then
    echo "Expected upstream generated debug keystore is missing." >&2
    exit 1
  fi
  "$TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android \
    --key-pass pass:android --ks-key-alias rdocandroidkey aligned.apk
  "$TOOLS/apksigner" verify aligned.apk
)
mkdir -p "$ROOT/dist"
cp "$STAGE/aligned.apk" "$ROOT/dist/RenderDoc-Mobile-ARM64-Debug.apk"
echo "Created dist/RenderDoc-Mobile-ARM64-Debug.apk"
