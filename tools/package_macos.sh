#!/usr/bin/env bash
# Package build/ as a macOS .app and a .dmg holding the app folder plus
# packaging/macos/START_HERE.txt. No disc or save data ships.
# The release gate rejects build-machine paths, so build with them mapped away:
#   cmake -S . -B build-macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DPSX_SDL_BACKEND=SDL3 -DPSX_RECOMP_UI=ON -DPSX_NETPLAY=ON \
#     "-DCMAKE_C_FLAGS=-ffile-prefix-map=$PWD=." "-DCMAKE_CXX_FLAGS=-ffile-prefix-map=$PWD=."
#   cmake --build build-macos --parallel && tools/package_macos.sh
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-build-macos}"
VERSION="v$(cat "$REPO/packaging/release/VERSION")"
OUT="$REPO/release-macos"

bash "$REPO/psxrecomp-v4/tools/package_release_macos.sh" \
    --repo "$REPO" --build "$BUILD" --bin MegaManX4Recomp \
    --app-name "Mega Man X4 Recompiled" --package MegaManX4Recomp \
    --bundle-id io.github.mairajborirug.megamanx4recomp \
    --version "$VERSION" --game-config packaging/release/game.toml \
    --out "$OUT" --exclude-dev-mods --no-zip

STAGE="$(find "$OUT/stage" -mindepth 1 -maxdepth 1 -type d | head -1)"
cp "$REPO/packaging/macos/START_HERE.txt" "$STAGE/START_HERE.txt"
DMG="$OUT/$(basename "$STAGE" | sed "s/-macos-/-$VERSION-macos-/").dmg"
rm -f "$DMG"
hdiutil create -volname "Mega Man X4 Recompiled" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
echo "BUILT: $DMG"
