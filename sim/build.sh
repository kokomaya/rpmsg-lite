#!/bin/bash
# RPMsg-Lite Simulator Build Script (MSYS2/MinGW)
# Usage: ./build.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LIB_DIR="$SCRIPT_DIR/../lib"
BUILD_DIR="$SCRIPT_DIR/build_sim"

mkdir -p "$BUILD_DIR"

SOURCES=(
    # RPMsg-Lite core (unmodified)
    "$LIB_DIR/rpmsg_lite/rpmsg_lite.c"
    "$LIB_DIR/rpmsg_lite/rpmsg_ns.c"
    "$LIB_DIR/rpmsg_lite/rpmsg_queue.c"
    "$LIB_DIR/virtio/virtqueue.c"
    "$LIB_DIR/common/llist.c"
    # Simulation
    "$SCRIPT_DIR/backend/main.c"
    "$SCRIPT_DIR/backend/sim_env.c"
    "$SCRIPT_DIR/backend/sim_platform.c"
    "$SCRIPT_DIR/backend/sim_core.c"
    "$SCRIPT_DIR/backend/sim_events.c"
    # Third party
    "$SCRIPT_DIR/third_party/mongoose/mongoose.c"
)

INCLUDES=(
    "-I$LIB_DIR/include"
    "-I$SCRIPT_DIR/backend"
    "-I$SCRIPT_DIR/third_party/mongoose"
)

CFLAGS="-Wall -Wno-unused-parameter -O2 -std=c11 -DRL_USE_STATIC_API=1 -DSIM_BUILD=1 -D_CRT_SECURE_NO_WARNINGS -DMG_ENABLE_WINSOCK=1"
LDFLAGS="-lws2_32"

echo "Building RPMsg-Lite Simulator..."
echo "Sources: ${#SOURCES[@]} files"

gcc $CFLAGS "${INCLUDES[@]}" "${SOURCES[@]}" $LDFLAGS -o "$BUILD_DIR/rpmsg_sim.exe"

echo "Build successful: $BUILD_DIR/rpmsg_sim.exe"

# Copy web files
cp -r "$SCRIPT_DIR/web" "$BUILD_DIR/"
echo "Web files copied to $BUILD_DIR/web/"
echo ""
echo "Run: cd $BUILD_DIR && ./rpmsg_sim.exe"
echo "Then open: http://localhost:8080"
