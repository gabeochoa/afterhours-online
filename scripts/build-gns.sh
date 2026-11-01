#!/bin/sh
# Build the pinned GameNetworkingSockets release as a static library and
# install it (plus its BSD-3 headers) into ../vendor/gns/.
#
# This is the "build once" step from the afterhours-online plan: nobody
# cloning a game should rebuild GNS per-clone. A future CI job publishes
# this same artifact per platform and a fetch script will download it;
# until then this script produces it locally in ~1-2 minutes.
#
# Requirements (macOS/Homebrew): cmake, protobuf, abseil, openssl@3
#   brew install cmake protobuf abseil openssl@3
#
# Note: libGameNetworkingSockets_s.a is static. Its protobuf/Abseil
# dependencies still link dynamically from Homebrew on a dev machine
# (Homebrew ships no static protobuf). The release pipeline must build
# protobuf statically as well so shipped binaries have no protobuf dylib.
set -eu

GNS_VERSION="v1.6.1" # interface SteamNetworkingSockets012 (LibV12)
PLUGIN_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
WORK="$PLUGIN_ROOT/vendor/gns-work"
OUT="$PLUGIN_ROOT/vendor/gns"

if [ -f "$OUT/lib/libGameNetworkingSockets_s.a" ] && [ "${FORCE:-0}" != "1" ]; then
  echo "GNS already built: $OUT/lib/libGameNetworkingSockets_s.a (FORCE=1 to rebuild)"
  exit 0
fi

if [ ! -d "$WORK/src/.git" ]; then
  mkdir -p "$WORK"
  git clone --depth 1 --branch "$GNS_VERSION" \
    https://github.com/ValveSoftware/GameNetworkingSockets.git "$WORK/src"
fi

OPENSSL_ROOT="${OPENSSL_ROOT_DIR:-$(brew --prefix openssl@3 2>/dev/null || echo /opt/homebrew/opt/openssl@3)}"
PROTOBUF_ROOT="${Protobuf_ROOT:-$(brew --prefix protobuf 2>/dev/null || echo /opt/homebrew/opt/protobuf)}"

cmake -S "$WORK/src" -B "$WORK/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIB=OFF -DBUILD_STATIC_LIB=ON \
  -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF \
  -DENABLE_ICE=OFF \
  -DOPENSSL_ROOT_DIR="$OPENSSL_ROOT" \
  -DProtobuf_ROOT="$PROTOBUF_ROOT"
cmake --build "$WORK/build" -j 8

mkdir -p "$OUT/lib" "$OUT/include"
cp "$WORK/build/src/libGameNetworkingSockets_s.a" "$OUT/lib/"
rm -rf "$OUT/include/steam"
cp -R "$WORK/src/include/steam" "$OUT/include/"
cp "$WORK/src/LICENSE" "$OUT/" 2>/dev/null || true
echo "GNS $GNS_VERSION installed to $OUT"
