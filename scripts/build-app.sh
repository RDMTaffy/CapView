#!/bin/bash
# Builds CapView with SwiftPM and wraps it into a signed .app bundle.
#
#   scripts/build-app.sh            # release build -> build/CapView.app
#   scripts/build-app.sh debug      # debug build
#   scripts/build-app.sh install    # release build, then replace /Applications/CapView.app
#   scripts/build-app.sh dist       # universal (Apple Silicon + Intel), ad-hoc signed,
#                                   # zipped for sharing: build/CapView-<version>-macOS.zip
#   CODESIGN_IDENTITY=- scripts/build-app.sh   # force ad-hoc signing
#
# Signing with a stable identity (e.g. "Apple Development") keeps the camera /
# microphone permission across rebuilds. Ad-hoc signing ("-") works too, but
# macOS may ask for permission again after every rebuild.
set -euo pipefail

cd "$(dirname "$0")/.."
MODE="${1:-release}"
CONFIG=release
[[ "$MODE" == debug ]] && CONFIG=debug
APP_NAME="CapView"
APP_DIR="build/${APP_NAME}.app"

# The audio core relies on the compiler rejecting allocation/locks inside
# real-time callbacks (Xcode 26+). Prove the check is live: this must not compile.
PROBE_DIR="$(mktemp -d)"
printf '#include <CoreAudio/CoreAudio.h>\n#include <stdlib.h>\nvoid probe(void) CA_REALTIME_API { free(malloc(1)); }\n' \
    > "$PROBE_DIR/rt.c"
if xcrun clang -fsyntax-only -Wfunction-effects -Werror=function-effects "$PROBE_DIR/rt.c" 2>/dev/null; then
    echo "warning: this toolchain does not check real-time safety; use Xcode 26 or later" >&2
fi
rm -rf "$PROBE_DIR"

# Link against the current SDK; otherwise the binary is stamped with the
# deployment target's SDK and newer macOS runs it in compatibility mode.
SDK="$(xcrun --show-sdk-path)"
BUILD_FLAGS=(-c "$CONFIG" -Xswiftc -Xclang-linker -Xswiftc -isysroot -Xswiftc -Xclang-linker -Xswiftc "$SDK")
if [[ "$MODE" == dist ]]; then
    BUILD_FLAGS+=(--arch arm64 --arch x86_64)
    # Shared builds are signed ad-hoc: a personal development certificate
    # means nothing on other Macs and would only embed the owner's name.
    CODESIGN_IDENTITY=-
fi
swift build "${BUILD_FLAGS[@]}"
BIN="$(swift build "${BUILD_FLAGS[@]}" --show-bin-path)/${APP_NAME}"

rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources"
cp "$BIN" "$APP_DIR/Contents/MacOS/${APP_NAME}"
cp Resources/Info.plist "$APP_DIR/Contents/Info.plist"
# A new build number per build makes macOS re-read the bundle (icon caches
# in the Dock / app list are keyed by it).
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $(date +%Y%m%d.%H%M%S)" "$APP_DIR/Contents/Info.plist"
cp Resources/AppIcon.icns "$APP_DIR/Contents/Resources/AppIcon.icns"

if [[ -z "${CODESIGN_IDENTITY:-}" ]]; then
    # First "Apple Development" identity's SHA-1, falling back to ad-hoc.
    CODESIGN_IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null \
        | awk '/Apple Development/ { print $2; exit }' || true)"
    CODESIGN_IDENTITY="${CODESIGN_IDENTITY:--}"
fi
# Hardened runtime needs the camera / audio-input entitlements, or capture is denied.
codesign --force --options runtime --entitlements Resources/CapView.entitlements \
    --sign "$CODESIGN_IDENTITY" "$APP_DIR"
codesign --verify --strict "$APP_DIR"
echo "Built $(pwd)/$APP_DIR (signed with: $CODESIGN_IDENTITY)"

if [[ "$MODE" == dist ]]; then
    VERSION="$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" Resources/Info.plist)"
    ZIP="build/${APP_NAME}-${VERSION}-macOS.zip"
    rm -f "$ZIP"
    # ditto keeps the code signature and extended attributes intact.
    ditto -c -k --sequesterRsrc --keepParent "$APP_DIR" "$ZIP"
    echo "Packaged $(pwd)/$ZIP"
fi

if [[ "$MODE" == install ]]; then
    # Replace rather than copy over: overwriting a signed binary that is still
    # mapped can get the running copy killed.
    if pkill -x "$APP_NAME"; then
        while pgrep -x "$APP_NAME" >/dev/null; do sleep 0.2; done
    fi
    rm -rf "/Applications/${APP_NAME}.app"
    ditto "$APP_DIR" "/Applications/${APP_NAME}.app"
    /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
        -f "/Applications/${APP_NAME}.app"
    echo "Installed /Applications/${APP_NAME}.app"
fi
