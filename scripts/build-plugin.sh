#!/usr/bin/env bash
# Build the vpp-rtp-asr plugin out-of-tree against installed VPP +
# Sherpa-ONNX + FFmpeg + libopus.
#
# Designed to run inside the Docker dev container (see Dockerfile.base)
# where the dependency toolchain is pinned. Running on the bare host
# works too if those deps are installed.
#
# Usage:
#   ./scripts/build-plugin.sh            # build only
#   ./scripts/build-plugin.sh --test     # build + run tests

set -euo pipefail

SRC_DIR="${SRC_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
BUILD_DIR="${BUILD_DIR:-${SRC_DIR}/build}"
RUN_TESTS=false

for arg in "$@"; do
  case "$arg" in
    --test) RUN_TESTS=true ;;
  esac
done

if [ ! -f "$BUILD_DIR/build.ninja" ] && [ ! -f "$BUILD_DIR/Makefile" ]; then
  echo "==> Configuring vpp-rtp-asr plugin build..."
  cmake -G Ninja -S "$SRC_DIR" -B "$BUILD_DIR"
fi

echo "==> Building vpp-rtp-asr plugin..."
cmake --build "$BUILD_DIR" -j"$(nproc)"

PLUGIN_SO=$(find "$BUILD_DIR" -name 'rtp_asr_plugin.so' 2>/dev/null | head -1)
echo
echo "==> Build complete."
echo "    Plugin .so:  ${PLUGIN_SO:-not found}"

if [ "$RUN_TESTS" = true ]; then
  echo
  echo "==> Running tests..."
  cd "$SRC_DIR"
  PLUGIN_BUILD_DIR="$BUILD_DIR" SRC_DIR="$SRC_DIR" pytest test/ -v
fi
