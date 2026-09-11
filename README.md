# vpp-rtp-asr

VPP out-of-tree plugin for inline RTP stream tapping and streaming speech-to-text
at line rate. Runs [Moonshine v2](https://github.com/moonshine-ai/moonshine)
via [Sherpa-ONNX](https://github.com/k2-fsa/sherpa-onnx) off-path on dedicated
Linux worker threads. Part of the PacketLens plugin family, maintained by
[PacketFlow](https://packetflow.dev).

> **Status: pre-production.** Lab-validated on our own bench, not yet deployed in
> production. Any performance figures below are bench measurements, not production
> telemetry.


## What it does

The plugin hooks into the VPP `ip4-unicast` feature arc. For every RTP/UDP packet
on configured interfaces it:

1. Parses the 12-byte RTP header (V, PT, SEQ, TS, SSRC)
2. Looks up or creates a per-SSRC session in a per-worker bihash table
3. Enqueues the payload to a SPSC ring — the graph node never blocks
4. Returns `NEXT_PASS` so the packet continues through the VPP pipeline unchanged

A pool of Linux worker threads drains the rings and, per session:

- Decodes G.711 µ-law/A-law via inline 256-entry LUT (no libavcodec for the common case)
- Decodes Opus via libopus, G.722/G.729/AMR-WB via libavcodec
- Resamples to 16 kHz f32 with libswresample (Kaiser anti-alias filter)
- Accumulates PCM into per-session chunk buffers
- Runs Moonshine-tiny-int8 (default) via Sherpa-ONNX offline C API
- Emits transcript records via JSON-over-UDP (default) or syslog

## Current status

All integration tests passing. Working pipeline: Piper TTS → 8 kHz G.711 µ-law →
VPP packet-generator → `rtp-asr-tap` feature node → Moonshine-tiny-int8 → JSON-UDP.
Calibrated accuracy: **~86% word accuracy** on the pangram through the full stack.

```
test/test_smoke.py::test_plugin_loads          PASSED
test/test_smoke.py::test_rtp_asr_show_stats    PASSED
test/test_smoke.py::test_rtp_asr_show_version  PASSED
test/test_e2e.py::test_sherpa_loaded           PASSED
test/test_e2e.py::test_replay_transcribes      PASSED
test/test_tts_roundtrip.py::test_tts_roundtrip PASSED
```

## Architecture

```
                      ┌──────────────────────────┐   ┌────────────────────────────────────┐
  ingress NIC ─► VPP  │ VPP worker (hot path)    │   │ Linux worker threads (off-path)    │
                      │                          │   │                                    │
                      │  ip4-unicast feature arc  │   │  per session:                      │
                      │  ┌──────────────────────┐ │   │   1. codec decode                  │
                      │  │ rtp-asr-tap node     │ │   │      G.711: inline LUT (1 ns/samp) │
                      │  │                      │ │   │      Opus: libopus                 │
                      │  │  parse RTP header    │ │SPSC│      G.722+: libavcodec            │
                      │  │  bihash lookup       ├─┼───►  2. libswresample → 16kHz f32      │
                      │  │  enqueue payload     │ │ring│  3. accumulate chunk (2s default)  │
                      │  │  NEXT_PASS           │ │   │  4. Sherpa-ONNX offline recognizer │
                      │  └──────────────────────┘ │   │     (Moonshine-tiny-int8)          │
                      │  < 100 ns/packet           │   │  5. emit transcript JSON           │
                      └──────────────────────────┘   └─────────────────┬──────────────────┘
                                                                        │
                                                                        ▼
                                                         JSON-UDP / syslog / (gRPC, planned)
```

**Design invariant:** the VPP graph node does header parsing + bihash lookup +
ring enqueue only. It never calls into libavcodec, libswresample, or Sherpa-ONNX.
Inference latency is milliseconds; graph node budget is nanoseconds.

**ASR runtime note:** Sherpa-ONNX's online (streaming) recognizer does not expose
Moonshine on the C API in published releases. Moonshine runs on the offline path.
We use a chunk-based loop: accumulate ~2 s of 16 kHz PCM, run offline decode once
per chunk, emit. A future stage can swap Zipformer-streaming behind the same API.

## Build

All build and test work happens inside Docker. The base image bundles:
VPP 24.x (fd.io apt), Sherpa-ONNX v1.12 (built from source), FFmpeg, libopus,
piper-tts, scapy, and pytest.

```bash
# Build the dev base image (~15 min first time — sherpa-onnx compile)
docker build -f Dockerfile.base -t vpp-rtp-asr-dev:base .

# Build the plugin
docker compose run --rm build

# Drop into an interactive dev shell with the plugin built
docker compose run --rm dev
```

The plugin builds as a single `rtp_asr_plugin.so` via CMake out-of-tree,
following the same `add_vpp_plugin()` pattern as vpp-ndpi.

## Test

### Fetch models first

```bash
# Moonshine-tiny + Piper voice (required; CI default)
bash models/fetch.sh --tiny-only

# Also download Moonshine-base (optional, better WER)
bash models/fetch.sh
```

Model layout after fetch:

```
models/
├── sherpa-onnx-moonshine-tiny-en-int8/   # default model (~26 MB)
│   ├── preprocess.onnx
│   ├── encode.int8.onnx
│   ├── uncached_decode.int8.onnx
│   ├── cached_decode.int8.onnx
│   ├── tokens.txt
│   └── test_wavs/8k.wav               # bundled 8kHz wav fixture
├── sherpa-onnx-moonshine-base-en-int8/   # optional (~58 MB)
└── piper/
    ├── en_US-lessac-medium.onnx        # required by TTS roundtrip test
    └── en_US-lessac-medium.onnx.json
```

### Run the suite

```bash
# Full test suite inside Docker (builds + tests)
docker compose run --rm test

# Faster iteration — just run tests against an already-built plugin
docker compose run --rm dev pytest test/ -v
```

### Test descriptions

| Test | Marks | What it checks |
|---|---|---|
| `test_smoke.py::test_plugin_loads` | — | VPP starts, plugin `.so` loads |
| `test_smoke.py::test_rtp_asr_show_stats` | — | CLI `show rtp-asr stats` works |
| `test_smoke.py::test_rtp_asr_show_version` | — | CLI `show rtp-asr version` works |
| `test_e2e.py::test_sherpa_loaded` | integration | Moonshine model loads in-process |
| `test_e2e.py::test_replay_transcribes` | integration, slow | G.711 pcap → VPP → Moonshine → word found |
| `test_tts_roundtrip.py::test_tts_roundtrip` | integration, slow | Piper TTS → G.711 → VPP → Moonshine → ≥5/7 words |

Skip conditions: `test_e2e` and `test_tts_roundtrip` are skipped if models are
not present. `test_tts_roundtrip` additionally requires piper-tts and ffmpeg.

## Configuration

### Environment variables (at VPP startup)

| Variable | Default | Description |
|---|---|---|
| `VPP_RTP_ASR_MODEL_DIR` | — | Path to Moonshine model directory (required) |
| `VPP_RTP_ASR_EMITTER_JSON_UDP` | — | `host:port` for JSON-UDP transcript emission |

### VPP startup.conf block

```
rtp-asr {
  model-dir /path/to/sherpa-onnx-moonshine-tiny-en-int8
  emitter-json-udp 127.0.0.1:7879
  segment-seconds 2.0
  sessions-per-worker 256
  expiry-seconds 30
}
```

### VPP CLI

```
rtp-asr enable  <interface>      # tap RTP on this interface
rtp-asr disable <interface>
show rtp-asr stats               # counters: packets, sessions, drops
show rtp-asr version
```

## Transcript format (JSON-UDP)

Each transcript segment emits a newline-terminated JSON record:

```json
{
  "ts_wall":    "2026-04-24T13:37:42.891Z",
  "ssrc":       "0x7f3a91c2",
  "src":        "10.1.2.3:40000",
  "dst":        "10.1.2.4:5004",
  "pt":         0,
  "codec":      "PCMU",
  "text":       "the quick brown fox jumped over the lazy dog",
  "is_final":   true
}
```

## Codec support

| PT | Codec | Decoder | Notes |
|---|---|---|---|
| 0 | PCMU (G.711 µ-law) | Inline 256-entry LUT | ~1 ns/sample |
| 8 | PCMA (G.711 A-law) | Inline 256-entry LUT | ~1 ns/sample |
| 9 | G.722 | libavcodec | RTP clock 8kHz, audio 16kHz |
| 18 | G.729 | libavcodec | If present in FFmpeg build |
| 111 (typical) | Opus | libopus | Direct, lower overhead than libavcodec wrapper |
| dynamic | Any | libavcodec | Configured PT→codec mapping |

All paths resample to 16 kHz f32 via libswresample before Moonshine inference.

## Known quirks (VPP 24.x)

**packet-generator `enable-stream <name>` is silently ignored.** Use the
no-argument form to enable all streams:
```
vppctl packet-generator enable-stream          # correct
vppctl packet-generator enable-stream stream0  # silently does nothing in 24.x
```

**pcap destination MAC must be broadcast.** VPP's `ethernet-input` drops frames
with a unicast dst MAC that doesn't match the interface MAC. Since
packet-generator interfaces have no real MAC, use broadcast in synthesized pcaps:
```python
Ether(dst="ff:ff:ff:ff:ff:ff") / IP(...) / UDP(...) / rtp_bytes
```

## Roadmap

**v1 (in progress)**
- [x] VPP graph node: RTP parse + bihash session tracking
- [x] Codec decode: G.711 LUT, G.722/G.729 via libavcodec, Opus via libopus
- [x] Resample: libswresample Kaiser filter, 8/16/48 kHz → 16 kHz f32
- [x] ASR: Sherpa-ONNX offline recognizer, Moonshine-tiny-int8
- [x] Emitter: JSON-over-UDP
- [x] Test harness: smoke + e2e + TTS roundtrip (6/6 passing)
- [ ] VAD-gated segment finalization (Silero)
- [ ] Codec matrix test (PT 0/8/9/18/111)
- [ ] Session lifecycle test (concurrent SSRCs, aging)
- [ ] gRPC emitter
- [ ] perfmon < 100 ns/packet assertion in CI

**v2**
- GPU backend via TensorRT / ONNX Runtime CUDA EP
- Riva gRPC backend for central inference farm deployments
- Multilingual / Whisper fallback
- Fine-tuned Moonshine on G.711-degraded telephony data
- Jitter reorder window (20–50 ms) with proper PLC
- IPFIX exporter

## Related

- [vpp-ndpi](https://github.com/packetlens/vpp-ndpi) — application visibility via
  nDPI; this plugin's CMake + fixture + VPP graph node pattern is cloned from here
- [Sherpa-ONNX](https://github.com/k2-fsa/sherpa-onnx) — offline/online ASR runtime
- [Moonshine](https://github.com/moonshine-ai/moonshine) — the ASR model (MIT)
- [Piper](https://github.com/rhasspy/piper) — neural TTS used in the roundtrip test

See [FUNC_SPEC.md](FUNC_SPEC.md) for the full functional specification including
architecture details, codec handling, session lifecycle, and the transcription stack
decision rationale.

## License

Apache 2.0. Dynamically links Sherpa-ONNX (Apache 2.0), FFmpeg libavcodec /
libavutil / libswresample (LGPL 2.1+), and libopus (BSD-3).
