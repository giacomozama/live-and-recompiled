#!/usr/bin/env bash
# package-android.sh - build, test and zip the Android release: the APK builders (tools/package-android.cmake, which
# says what it does).
#
#   tools/package-android.sh                  build the kit and base APK, test them on assets/, zip them
#   tools/package-android.sh --no-test        skip the test (it needs the game in assets/)
#   tools/package-android.sh --test-download  test with the compiler the builder downloads, not the one in toolchain/
#   tools/package-android.sh -j N             parallel jobs (default: every core)
#
# Output: out/release/LiveAndRecompiled-android/, out/release/LiveAndRecompiled-<version>-android-builder-windows.zip and
# -linux.tar.gz. Needs the Android NDK and SDK (ANDROID_NDK_HOME, ANDROID_HOME) and a JDK 17 to 21 (JAVA_HOME)
# env overrides: XBE_TOOLCHAIN_ROOT (the toolchain, default toolchain/)
set -euo pipefail

ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --no-test) ARGS+=(-DTEST=OFF) ;;
        --test-download) ARGS+=(-DTEST_DOWNLOAD=ON) ;;
        -j) ARGS+=("-DJOBS=${2:?-j needs a number}"); shift ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
exec cmake "${ARGS[@]}" -P "$(dirname "${BASH_SOURCE[0]}")/package-android.cmake"
