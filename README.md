# vpp-rtp-asr

VPP out-of-tree plugin for inline RTP stream tapping and streaming speech-to-text
at line rate. Default model: [Moonshine v2](https://github.com/moonshine-ai/moonshine)
via [Sherpa-ONNX](https://github.com/k2-fsa/sherpa-onnx). Part of the PacketLens
plugin family, maintained by [PacketFlow](https://packetflow.dev).

> **Status:** early scaffolding. The functional specification is frozen
> ([FUNC_SPEC.md](FUNC_SPEC.md)); source files are stubs. Do not expect
> a working build from `main` yet.

## What it does (when it works)

Taps RTP/RTCP traffic on configured VPP interfaces, tracks per-SSRC sessions,
decodes G.711 µ-law/A-law / G.722 / G.729 / Opus / AMR-WB, resamples to 16 kHz,
runs VAD, and streams audio into a Moonshine-based recognizer. Transcripts
emit via syslog, JSON-over-UDP, or streaming gRPC.

Key design decisions (see [FUNC_SPEC.md](FUNC_SPEC.md) §2):

- The VPP graph node does **only** header parse + bihash lookup + ring enqueue.
  Budget: < 100 ns per packet (steady state, bihash hit).
- Codec decode, resampling, VAD, and inference all run **off-path** on dedicated
  Linux worker threads. The graph node never blocks on an ASR call.
- G.711 gets an inline 256-entry LUT decode — no libavcodec roundtrip for the
  common-case telephony codec.
- In-process Sherpa-ONNX C API, not a remote inference server.
  Moonshine-base by default (58 MB, MIT); Parakeet / Canary / Whisper available
  as optional backends.

## Build and test

All build + test work happens inside the Docker dev container. The base
image bundles VPP (fd.io apt), Sherpa-ONNX (built from source), FFmpeg,
libopus, and the Python test harness.

```bash
# Build the dev base image (~15 min first time — sherpa-onnx compile)
docker build -f Dockerfile.base -t vpp-rtp-asr-dev:base .

# Build the plugin out-of-tree
docker compose run --rm build

# Build + run the full test suite
docker compose run --rm test

# Interactive dev shell
docker compose run --rm dev
```

## Roadmap

- **v1** — functional spec in [FUNC_SPEC.md](FUNC_SPEC.md). G.711 + Opus + G.722
  path, Moonshine via Sherpa-ONNX, JSON-UDP emitter, CI on synthetic pcap.
- **v2** — TensorRT backend, multilingual fallback, fine-tuned telephony model,
  IPFIX exporter, jitter reorder window.

See [docs/upstream-split.md](docs/upstream-split.md) for the plan on which
pieces we propose to contribute back to fd.io/vpp.

## Related PacketLens plugins

- [vpp-ndpi](https://github.com/packetlens/vpp-ndpi) — application visibility
  via nDPI (the template this plugin's structure is cloned from)
- [ndpi-observe](https://github.com/packetlens/ndpi-observe) — eBPF variant

## License

Apache 2.0. Dynamically links Sherpa-ONNX (Apache 2.0), FFmpeg libavcodec /
libavutil / libswresample (LGPL 2.1+), and libopus (BSD-3).
