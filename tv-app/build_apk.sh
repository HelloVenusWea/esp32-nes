#!/usr/bin/env bash
# Build a signed debug APK without Gradle/AndroidX.
# Requires the Android SDK (platform + build-tools) and a JDK.
set -e

W() { cygpath -w "$1"; }
JAVA_HOME="${JAVA_HOME:-C:/Program Files/Android/Android Studio/jbr}"
JAVA="$JAVA_HOME/bin/java.exe"
JAVAC="$JAVA_HOME/bin/javac.exe"
KEYTOOL="$JAVA_HOME/bin/keytool.exe"

ROOT="$(cd "$(dirname "$0")" && pwd)"
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-C:/Users/venus/AppData/Local/Android/Sdk}}"
BT_VERSION="${BT_VERSION:-36.1.0}"
PLATFORM="${PLATFORM:-android-36}"

AAPT2="$SDK/build-tools/$BT_VERSION/aapt2.exe"
ZIPALIGN="$SDK/build-tools/$BT_VERSION/zipalign.exe"
D8_JAR="$SDK/build-tools/$BT_VERSION/lib/d8.jar"
APKSIGNER_JAR="$SDK/build-tools/$BT_VERSION/lib/apksigner.jar"
ANDROID_JAR="$SDK/platforms/$PLATFORM/android.jar"

for f in "$AAPT2" "$ZIPALIGN" "$D8_JAR" "$APKSIGNER_JAR" "$ANDROID_JAR"; do
    if [ ! -f "$f" ]; then
        echo "missing: $f" >&2
        exit 1
    fi
done

BUILD="$ROOT/build"
OUT="$ROOT/esp32nes-tv.apk"
KEYSTORE="$ROOT/debug.keystore"
rm -rf "$BUILD"
mkdir -p "$BUILD/gen" "$BUILD/classes" "$BUILD/dex"

echo "[1/7] aapt2 compile"
"$AAPT2" compile --dir "$(W "$ROOT/res")" -o "$(W "$BUILD/res.zip")"

echo "[2/7] aapt2 link"
"$AAPT2" link \
    -o "$BUILD/base.apk" \
    -I "$ANDROID_JAR" \
    --manifest "$ROOT/AndroidManifest.xml" \
    --java "$BUILD/gen" \
    --min-sdk-version 21 \
    --target-sdk-version 34 \
    "$BUILD/res.zip"

echo "[3/7] javac"
find "$ROOT/java" "$BUILD/gen" -name '*.java' | while read -r f; do cygpath -w "$f"; done > "$BUILD/sources.txt"
"$JAVAC" -source 8 -target 8 -encoding UTF-8 -cp "$(W "$ANDROID_JAR")" -d "$(W "$BUILD/classes")" @"$(W "$BUILD/sources.txt")"

echo "[4/7] d8"
CLASSES=$(find "$BUILD/classes" -name '*.class' | while read -r f; do cygpath -w "$f"; done)
java -cp "$D8_JAR" com.android.tools.r8.D8 \
    --min-api 21 --lib "$(W "$ANDROID_JAR")" --output "$(W "$BUILD/dex")" $CLASSES

echo "[5/7] package"
python - "$(W "$BUILD/base.apk")" "$(W "$BUILD/dex/classes.dex")" <<'PYADD'
import sys, zipfile
apk, dex = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk, 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(dex, 'classes.dex')
PYADD
"$ZIPALIGN" -f 4 "$(W "$BUILD/base.apk")" "$(W "$BUILD/aligned.apk")"

echo "[6/7] sign"
if [ ! -f "$KEYSTORE" ]; then
    "$KEYTOOL" -genkeypair -v \
        -keystore "$(W "$KEYSTORE")" \
        -storepass android -alias androiddebugkey -keypass android \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi
java -jar "$APKSIGNER_JAR" sign \
    --ks "$KEYSTORE" \
    --ks-pass pass:android --key-pass pass:android \
    --out "$OUT" "$BUILD/aligned.apk"

echo "[7/7] verify"
"$JAVA" -jar "$(W "$APKSIGNER_JAR")" verify --verbose "$(W "$OUT")" | head -12
"$AAPT2" dump badging "$(W "$OUT")" | grep -E "package:|launchable-activity:|application-label:|sdkVersion|targetSdkVersion" | head -8
ls -lh "$OUT"
