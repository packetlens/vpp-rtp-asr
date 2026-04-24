#!/usr/bin/env bash
# Copyright (c) 2026 PacketFlow (packetflow.dev)
# SPDX-License-Identifier: Apache-2.0
#
# Download Moonshine ONNX models + Silero VAD with checksum verification.
#
# Usage:
#   bash models/fetch.sh                    # default: base
#   bash models/fetch.sh base small         # multiple variants
#
# Upstream sources (subject to change — pin digests here):
#   Moonshine: huggingface.co/UsefulSensors/moonshine
#   Silero:    huggingface.co/snakers4/silero-vad

set -euo pipefail

MODELS_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${MODELS_DIR}"

VARIANTS=("${@:-base}")

# TODO: pin SHA-256 digests and mirror URLs. Placeholders for now.
declare -A URLS=(
  [base]="https://example.invalid/moonshine-base.onnx"
  [small]="https://example.invalid/moonshine-small-streaming.onnx"
  [medium]="https://example.invalid/moonshine-medium-streaming.onnx"
  [silero]="https://example.invalid/silero_vad.onnx"
)
declare -A SHA256=(
  [base]="TODO"
  [small]="TODO"
  [medium]="TODO"
  [silero]="TODO"
)

fetch_one () {
  local key="$1"
  local url="${URLS[$key]}"
  local sha="${SHA256[$key]}"
  local dest
  case "$key" in
    base)   dest="moonshine-base.onnx" ;;
    small)  dest="moonshine-small-streaming.onnx" ;;
    medium) dest="moonshine-medium-streaming.onnx" ;;
    silero) dest="silero_vad.onnx" ;;
  esac

  if [[ -f "$dest" ]]; then
    echo "[models] ${dest} already present — skipping"
    return 0
  fi

  echo "[models] fetching ${dest} from ${url}"
  curl -fL --retry 3 -o "${dest}.part" "$url"

  if [[ "$sha" == "TODO" ]]; then
    echo "[models] WARNING: SHA-256 not pinned for ${key}; skipping verification"
  else
    echo "${sha}  ${dest}.part" | sha256sum --check
  fi

  mv "${dest}.part" "${dest}"
  echo "[models] ${dest} ready"
}

fetch_one silero
for v in "${VARIANTS[@]}"; do
  fetch_one "$v"
done
