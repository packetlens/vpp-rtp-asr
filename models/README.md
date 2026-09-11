# Models

Moonshine v2 ONNX models are **not committed to git**. The `fetch.sh` script
downloads them from a checksum-verified mirror into this directory.

Default variant: `moonshine-base` (58 MB, ~10 % WER on clean English,
50–150 ms streaming TTFT).

Alternate variants selectable via `vppctl rtp-asr set model-variant`:

- `moonshine-small-streaming` (123 MB, ~7.8 % WER)
- `moonshine-medium-streaming` (245 MB, ~6.7 % WER)

Silero VAD (`silero_vad.onnx`, ~1 MB) is also fetched by this script.

## Usage

```
bash models/fetch.sh              # default: base
bash models/fetch.sh base small   # fetch multiple variants
```

The plugin refuses to load a model whose SHA-256 doesn't match the pinned
digest in `fetch.sh`.
