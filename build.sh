#!/bin/sh
# Builds build/oniria on Linux with g++. Fetches the prebuilt raylib on first run.
# Needs: g++, curl, tar, and the usual X11/OpenGL development packages
# (on Debian/Ubuntu: libgl1-mesa-dev libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev).
#
# The Linux build is experimental: it shares all the game code, but timestamped
# raw key input and the file dialog exist only on Windows. Keys are read once a
# frame, and songs are opened by dropping a file on the window or passing its
# path on the command line.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
VERSION=6.2
NAME="raylib-${VERSION}_linux_amd64"
RAYLIB="$ROOT/third_party/raylib-linux"

if [ ! -f "$RAYLIB/include/raylib.h" ]; then
    mkdir -p "$ROOT/third_party"
    echo "Downloading raylib $VERSION"
    curl -fsSL -o "$ROOT/third_party/$NAME.tar.gz" "https://github.com/raysan5/raylib/releases/download/$VERSION/$NAME.tar.gz"
    tar -xzf "$ROOT/third_party/$NAME.tar.gz" -C "$ROOT/third_party"
    rm -rf "$RAYLIB"
    mv "$ROOT/third_party/$NAME" "$RAYLIB"
    rm "$ROOT/third_party/$NAME.tar.gz"
fi

mkdir -p "$ROOT/build"
g++ -std=c++17 -O2 -ffast-math -DNDEBUG -Wall -Wno-missing-field-initializers \
    -I"$RAYLIB/include" \
    "$ROOT/src/main.cpp" "$ROOT/src/analysis.cpp" "$ROOT/src/sfx.cpp" "$ROOT/src/platform_win.cpp" \
    "$RAYLIB/lib/libraylib.a" \
    -lGL -lm -lpthread -ldl -lrt -lX11 \
    -o "$ROOT/build/oniria"
echo "Built $ROOT/build/oniria"
