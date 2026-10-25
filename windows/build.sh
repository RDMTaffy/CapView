#!/bin/bash
# Cross-builds CapView for Windows (x64) with mingw-w64, from macOS or Linux.
#
#   windows/build.sh           # -> build/windows/CapView.exe
#   windows/build.sh package   # also build/windows/CapView-<version>-windows-x64.zip
#                              # and CapView-<version>-Setup.exe (needs makensis)
#
# macOS: brew install mingw-w64 makensis
set -euo pipefail

cd "$(dirname "$0")"
OUT="$(cd .. && pwd)/build/windows"
OBJ="$OUT/obj"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
WINDRES="${WINDRES:-x86_64-w64-mingw32-windres}"
DLLTOOL="${DLLTOOL:-x86_64-w64-mingw32-dlltool}"
VERSION="$(sed -n 's/^#define CAPVIEW_VERSION_STRING "\(.*\)"/\1/p' src/version.h)"

CXXFLAGS=(-std=c++20 -O2 -municode -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
          -Wall -Wextra -Wno-missing-field-initializers -Isrc)
# Media Foundation is delay-loaded (delayload/*.def), so Windows N/KN without the Media Feature
# Pack reaches main.cpp's explanation instead of the loader's "MFPlat.DLL was not found".
LIBS=(-L"$OBJ" -lmfplat_delay -lmf_delay -lmfreadwrite_delay -ldelayimp -lmfuuid -ld3d11 -ldxgi -ld3dcompiler -ld2d1 -ldwrite -lwindowscodecs
      -lavrt -lcfgmgr32 -lsetupapi -lcomctl32 -lpropsys -limm32 -lole32 -loleaut32 -luuid -lshlwapi
      -lwinmm -lshell32 -ladvapi32 -luser32 -lgdi32)

mkdir -p "$OBJ"
for def in delayload/*.def; do
    "$DLLTOOL" -d "$def" -y "$OBJ/lib$(basename "${def%.def}")_delay.a"
done
objects=()
for source in src/*.cpp; do
    object="$OBJ/$(basename "${source%.cpp}").o"
    echo "  CXX $source"
    "$CXX" "${CXXFLAGS[@]}" -c "$source" -o "$object"
    objects+=("$object")
done
echo "  RC  res/app.rc"
"$WINDRES" --include-dir res --include-dir src -O coff res/app.rc -o "$OBJ/app.res.o"

# Static runtime: the exe needs nothing beyond what Windows 10/11 ships.
"$CXX" -static -municode -mwindows -O2 -s "${objects[@]}" "$OBJ/app.res.o" -o "$OUT/CapView.exe" "${LIBS[@]}"
echo "Built $OUT/CapView.exe"

if [[ "${1:-}" == package ]]; then
    STAGE="$(mktemp -d)"
    trap 'rm -rf "$STAGE"' EXIT
    mkdir -p "$STAGE/CapView"
    cp "$OUT/CapView.exe" "$STAGE/CapView/"
    # Notepad-friendly: UTF-8 with BOM and CRLF line endings.
    { printf '\xEF\xBB\xBF'; sed 's/$/\r/' README-Windows.txt; } > "$STAGE/CapView/README.txt"
    ZIP="$OUT/CapView-${VERSION}-windows-x64.zip"
    rm -f "$ZIP"
    (cd "$STAGE" && zip -qr "$ZIP" CapView)
    echo "Packaged $ZIP"

    SETUP="$OUT/CapView-${VERSION}-Setup.exe"
    # makensis aborts with std::bad_alloc when no UTF-8 locale is set (empty LANG).
    LC_ALL=en_US.UTF-8 makensis -V2 -DVERSION="$VERSION" -DEXE="$OUT/CapView.exe" -DOUTFILE="$SETUP" installer.nsi
    echo "Packaged $SETUP"
fi
