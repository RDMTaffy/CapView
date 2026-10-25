#!/bin/bash
# Packages build/CapView.app into a drag-to-install DMG whose window shows the
# first-open steps (Resources/dmg/background*.png, drawn by dmg-background.py).
#
#   scripts/build-app.sh dist && scripts/make-dmg.sh   # -> build/CapView-<version>.dmg
#
# Finder lays out the window, so the first run asks to allow controlling Finder.
set -euo pipefail

cd "$(dirname "$0")/.."
APP="build/CapView.app"
VOLUME="CapView"
[[ -d "$APP" ]] || { echo "error: $APP is missing; run scripts/build-app.sh dist first" >&2; exit 1; }
if [[ -e "/Volumes/$VOLUME" ]]; then
    echo "error: a volume named $VOLUME is already mounted; eject it first" >&2
    exit 1
fi

VERSION="$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$APP/Contents/Info.plist")"
DMG="build/CapView-${VERSION}.dmg"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

# Volume contents: the app, an Applications link to drop it on, the background.
mkdir -p "$STAGE/src/.background"
ditto "$APP" "$STAGE/src/CapView.app"
ln -s /Applications "$STAGE/src/Applications"
tiffutil -cathidpicheck Resources/dmg/background.png Resources/dmg/background@2x.png \
    -out "$STAGE/src/.background/background.tiff" 2>/dev/null

hdiutil create -quiet -srcfolder "$STAGE/src" -volname "$VOLUME" -fs HFS+ -format UDRW \
    -size 40m "$STAGE/rw.dmg"
DEVICE="$(hdiutil attach -readwrite -noverify -noautoopen "$STAGE/rw.dmg" | awk '/Apple_HFS/ { print $1 }')"
# Window: 720x480 content, icons where the background's arrow expects them.
osascript <<'EOF'
tell application "Finder"
    tell disk "CapView"
        open
        set current view of container window to icon view
        set toolbar visible of container window to false
        set statusbar visible of container window to false
        set the bounds of container window to {200, 120, 920, 628}
        set viewOptions to the icon view options of container window
        set arrangement of viewOptions to not arranged
        set icon size of viewOptions to 112
        set text size of viewOptions to 13
        set background picture of viewOptions to file ".background:background.tiff"
        set position of item "CapView.app" of container window to {180, 128}
        set position of item "Applications" of container window to {540, 128}
        close
        open
        update without registering applications
        delay 2
        close
    end tell
end tell
EOF

# Disk icon last: hdiutil -srcfolder skips .VolumeIcon.icns, and Finder's
# "update" above deletes it again.
cp Resources/AppIcon.icns "/Volumes/$VOLUME/.VolumeIcon.icns"
SetFile -a C "/Volumes/$VOLUME"

sync
hdiutil detach -quiet "$DEVICE"
rm -f "$DMG"
hdiutil convert -quiet "$STAGE/rw.dmg" -format UDZO -imagekey zlib-level=9 -o "$DMG"
echo "Created $(pwd)/$DMG"
