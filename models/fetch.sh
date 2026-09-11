#!/usr/bin/env bash
# Copyright (c) 2026 PacketFlow (packetflow.dev)
# SPDX-License-Identifier: Apache-2.0
#
# Download ASR models + TTS voice for vpp-rtp-asr tests.
#
# Usage:
#   bash models/fetch.sh              # download all required models
#   bash models/fetch.sh --tiny-only  # Moonshine tiny + piper voice only (CI default)

set -euo pipefail

MODELS_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${MODELS_DIR}"

TINY_ONLY=0
[[ "${1:-}" == "--tiny-only" ]] && TINY_ONLY=1

# ── helpers ──────────────────────────────────────────────────────────────────

fetch() {
  local url="$1" dest="$2"
  if [[ -f "$dest" ]]; then
    echo "[fetch] ${dest##*/} already present — skipping"
    return 0
  fi
  echo "[fetch] ${dest##*/} ← ${url}"
  curl -fL --retry 3 --progress-bar -o "${dest}.part" "$url"
  mv "${dest}.part" "$dest"
}

untar_if_needed() {
  local archive="$1" dest_dir="$2"
  if [[ -d "$dest_dir" ]]; then
    echo "[fetch] ${dest_dir##*/}/ already present — skipping"
    return 0
  fi
  echo "[fetch] extracting ${archive##*/}"
  tar -xjf "$archive"
}

# ── Moonshine-tiny (required by all tests) ───────────────────────────────────

TINY_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-moonshine-tiny-en-int8.tar.bz2"
fetch "$TINY_URL" sherpa-onnx-moonshine-tiny-en-int8.tar.bz2
untar_if_needed sherpa-onnx-moonshine-tiny-en-int8.tar.bz2 sherpa-onnx-moonshine-tiny-en-int8

# ── Piper voice (required by TTS roundtrip test) ─────────────────────────────

PIPER_BASE="https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium"
mkdir -p piper
fetch "${PIPER_BASE}/en_US-lessac-medium.onnx"      piper/en_US-lessac-medium.onnx
fetch "${PIPER_BASE}/en_US-lessac-medium.onnx.json" piper/en_US-lessac-medium.onnx.json

if [[ $TINY_ONLY -eq 1 ]]; then
  echo "[fetch] --tiny-only: skipping moonshine-base"
  exit 0
fi

# ── Moonshine-base (optional, better accuracy) ────────────────────────────────

BASE_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-moonshine-base-en-int8.tar.bz2"
fetch "$BASE_URL" sherpa-onnx-moonshine-base-en-int8.tar.bz2
untar_if_needed sherpa-onnx-moonshine-base-en-int8.tar.bz2 sherpa-onnx-moonshine-base-en-int8

echo "[fetch] all models ready"
