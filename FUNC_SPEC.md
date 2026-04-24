# vpp-rtp-asr — Functional Specification (v1)

## 0. Context

Three signals converged on this project:

- **Telephony transcription in PacketFlow customer pipelines** needs a production-quality
  line-rate datapath. whisper.cpp is not the right engine for 8 kHz telephony —
  confirmed by research (acoustic bottleneck + streaming degradation) and by the
  MediaBridge reference project's observed quality gap.
- **PacketLens needs a VPP-plugin artifact** that demonstrates PacketFlow's "inline
  media processing at line rate" story. The existing `vpp-ndpi` plugin is the
  template; an RTP-ASR plugin is the natural next demo.
- **Moonshine v2** (UsefulSensors, MIT) is the right default model: 50–150 ms
  streaming TTFT, 26–245 MB on disk, CPU-viable, designed for the "many concurrent
  edge streams" shape that telephony traffic produces. Parakeet / Canary / Whisper
  remain optional backends for accuracy upsell and multilingual / offline paths.

Transcription-stack reasoning is summarized in Appendix A.

## 1. Purpose and scope

`vpp-rtp-asr` is a VPP out-of-tree plugin (`packetlens/vpp-rtp-asr`) that:

- Taps RTP/RTCP streams on configured interfaces at line rate
- Tracks per-SSRC sessions with codec state
- Decodes G.711 µ-law/A-law, G.722, G.729, Opus, AMR-WB and FFmpeg-discoverable payloads
- Resamples to 16 kHz f32 for ASR
- Runs Voice Activity Detection (Silero via Sherpa-ONNX by default)
- Streams audio into Sherpa-ONNX with a Moonshine v2 Base model (default) or configurable variant
- Emits transcripts via syslog, JSON-over-UDP, or gRPC

v1 non-goals:

- No media mixing, TTS, or call synthesis
- No SRTP decrypt (defer to fd.io's upstream `srtp` plugin; pipe its cleartext-egress into this plugin's feature arc)
- No SIP signaling integration — sessions are created heuristically on first valid RTP packet for an unseen SSRC
- No jitter reordering or advanced PLC — v1 relies on VPP RSS pinning + zero-fill for dropped samples
- No text-side fraud scoring — transcripts are consumed downstream

## 2. Top-level architecture

```
                      ┌────────────────────────────────┐   ┌──────────────────────────────────┐
  ingress NIC ─► VPP  │ VPP worker thread (hot)        │   │ Linux worker threads (N, cold)   │
                      │                                │   │                                  │
                      │  ip4/6-unicast ▶ rtp-asr-tap   │   │  per session:                    │
                      │                                │   │    1. codec decode (G.711 LUT    │
                      │    parse RTP header (12B)      │   │        inline; Opus/G.722/... via│
                      │    bihash lookup ({5t, ssrc})  │   │        libavcodec + libopus)     │
                      │    miss ⇒ pool alloc + insert  │SPSC│    2. libswresample → 16 kHz f32 │
                      │    enqueue payload ▶ ring ─────┼───►    3. Silero VAD                 │
                      │    counters++                  │ring│    4. sherpa_onnx_stream_accept_ │
                      │    return NEXT_PASS            │   │        waveform                  │
                      │                                │   │    5. poll result, emit segment  │
                      └────────────────────────────────┘   └──────────────────────────────────┘
                            ~< 100 ns/packet                         ~ms/chunk (off-path)
                                                                              │
                                                                              ▼
                                                              ┌──────────────────────────────┐
                                                              │ Transcript emitter (async)   │
                                                              │   syslog / JSON-UDP / gRPC   │
                                                              └──────────────────────────────┘
```

**Design invariant:** the VPP graph node does only parsing + bihash lookup + ring
enqueue. Codec decode, resampling, VAD, and inference all happen off-path on
dedicated Linux threads. The graph node must never block on an ASR call;
inference latency is measured in milliseconds-to-seconds, VPP graph batches in
nanoseconds.

## 3. VPP graph node: `rtp-asr-tap`

### 3.1 Placement

- Feature arc on `ip4-unicast` and `ip6-unicast`, inserted before `ip-lookup`,
  gated per-interface via `vnet_feature_enable_disable()`.
- Follows the exact layout of `vpp-ndpi/src/plugins/ndpi/node_observe.c`.
- Next arcs:
  - `NEXT_PASS` — always in passive-tap mode (default)
  - `NEXT_DROP` — malformed RTP only (version mismatch, truncated header); counted separately
  - `NEXT_PUNT` — reserved for future RTCP-only path

### 3.2 Per-packet work (hot path)

1. Framework hands us pre-parsed IP/UDP.
2. UDP port filter — configurable, default `{5004, 16384..32768}` (RFC 3551
   dynamic range + well-known). Off-range packets return `NEXT_PASS` immediately
   with no state touch.
3. Parse RTP header (12 bytes): `V==2`, `PT`, `SEQ`, `TS`, `SSRC`. If V != 2 or
   CC padding indicates truncation, increment `malformed_rtp` and `NEXT_PASS`.
4. Bihash lookup — key is `{src_ip, dst_ip, src_port, dst_port, ssrc}`.
   Separate bihash per AF (`clib_bihash_16_8_t` for IPv4, `clib_bihash_40_8_t`
   for IPv6 — same sizing as vpp-ndpi).
5. Miss path: validate PT is in the configured accept-list, pool-alloc a new
   `rtp_asr_session_t`, insert into bihash, increment `new_sessions`.
6. Enqueue `{session_idx, payload_offset, payload_len, rtp_ts, vpp_time}` to
   this worker's SPSC ring. On ring-full, drop the payload (not the packet),
   increment `ring_full_drops`, and still return `NEXT_PASS`.
7. Update session counters (`packets`, `bytes`, `last_seq`, `last_ts`,
   `last_packet_vpp_time`).
8. Return `NEXT_PASS`.

Budget: steady-state (bihash hit) must be **< 100 ns/packet**. Hard
requirement; verified via `perfmon` in CI.

### 3.3 Per-worker state

```c
typedef struct rtp_asr_worker_ {
  clib_bihash_16_8_t  v4_sessions;
  clib_bihash_40_8_t  v6_sessions;
  rtp_asr_session_t  *session_pool;

  svm_fifo_t         *payload_ring;     /* SPSC, fixed-size slots */
  u32                 owned_linux_worker_id;

  /* counters */
  u64 packets, bytes;
  u64 new_sessions, expired_sessions;
  u64 malformed_rtp, ring_full_drops;
  u64 ring_enqueue_retries;
} rtp_asr_worker_t;
```

Each VPP worker is statically bound at init to exactly one Linux worker
thread — keeps session ownership stable, no cross-thread migration in v1.

### 3.4 Session struct

```c
typedef struct rtp_asr_session_ {
  /* identity */
  ip46_address_t src, dst;
  u16  src_port, dst_port;
  u32  ssrc;

  /* RTP */
  u8   payload_type;
  u16  last_seq;
  u32  last_rtp_ts;
  u32  clock_rate;           /* from PT table */
  f64  last_packet_vpp_time;
  f64  first_packet_vpp_time;

  /* codec + ASR — opaque to VPP, owned by the Linux worker */
  u32  owned_linux_worker_id;
  void *codec_ctx;           /* AVCodecContext* or G711 LUT pointer */
  void *swr_ctx;             /* SwrContext* */
  void *asr_stream;          /* SherpaOnnxOnlineStream* */
  void *vad_ctx;             /* SileroVadDetector* */

  /* pcm staging */
  f32 *pcm_window;           /* sized = clock_rate_out * window_seconds */
  u32  pcm_samples;

  /* aging */
  u32  inactivity_ticks;
} rtp_asr_session_t;
```

Aging: per-worker periodic process walks the pool every 1 s; sessions idle
> `expiry_seconds` (default 30) are finalized (flush ASR stream, emit tail
transcript) and freed on both the Linux side and the VPP-bihash side.

## 4. Linux worker thread pool

### 4.1 Sizing and binding

- Default pool size: `min(online_cpus − vpp_workers, 4)`, overridable.
- Each VPP worker → exactly one Linux worker (round-robin at plugin init).
  1:N mapping (one Linux worker can serve multiple VPP workers).
- Each Linux worker owns:
  - Its share of codec + ASR contexts (session struct points to these by worker id)
  - One **shared-across-sessions** `SherpaOnnxOnlineRecognizer` (Moonshine model,
    loaded once per worker — not per session)
  - Per-session `SherpaOnnxOnlineStream` (lightweight wrapper, ~KB)
  - One libswresample context per active stream
  - One transcript-emitter client (syslog handle / UDP socket / gRPC channel)

### 4.2 Main loop

```
while (!shutdown) {
  /* drain all assigned rings with a bounded batch */
  for each ring in my_rings {
    for (int i = 0; i < BATCH; i++) {
      entry = ring_dequeue(ring);
      if (!entry) break;
      handle_payload(entry);
    }
  }
  drive_async_io();      /* any gRPC / Sherpa background progress */
  run_aging_sweep();     /* every ~1s wall clock */
  short_sleep_if_idle();
}
```

### 4.3 `handle_payload()` per-session work

1. Look up session via `worker.session_refs[entry.session_idx]`.
2. Codec decode: PCM (s16 or f32 depending on codec).
3. Resample to 16 kHz f32 via libswresample.
4. VAD: accept the chunk unconditionally into the ASR stream, but note
   speech/non-speech from Silero. (Sherpa-ONNX handles silence internally; VAD is
   used to gate *segment finalization*, not sample admission.)
5. `sherpa_onnx_online_stream_accept_waveform(stream, 16000, pcm, n)`.
6. Drain decoding: `while (sherpa_onnx_is_online_stream_ready(...)) sherpa_onnx_decode_online_stream(...)`.
7. Read current result; if a segment finalized (VAD silence edge OR configured
   max-segment duration hit), emit and reset the stream.
8. Update session counters.

## 5. RTP codec decode (the transcoding layer)

Design anchor: MediaBridge's `AudioDecoder`/`PcmEncoder` pipeline already solves
the codec + resample problem for the same codec set. Port that shape; do not
re-invent.

### 5.1 Payload-type → codec table

| PT | Codec | Clock rate (Hz) | Channels | Decoder path |
|----|---|---|---|---|
| 0  | PCMU (µ-law) | 8 000 | 1 | **Inline LUT** (µ-law → s16, 256-entry table) — no FFmpeg for G.711 |
| 8  | PCMA (A-law) | 8 000 | 1 | **Inline LUT** (A-law → s16) |
| 9  | G.722 | RTP 8000 / audio 16000 | 1 | libavcodec (AV_CODEC_ID_ADPCM_G722) |
| 18 | G.729 | 8 000 | 1 | libavcodec (AV_CODEC_ID_G729) if present in FFmpeg build; else disabled |
| 111 (typical) | Opus | 48 000 | 1–2 | **libopus directly** (`opus_decoder_create`/`opus_decode_float`) |
| 120–127 (typical) | AMR-WB / EVS / iLBC | per codec | 1 | libavcodec (if built in) |
| other dynamic | from config | from config | from config | libavcodec `avcodec_find_decoder_by_name()` |

**G.711 specifically** gets the inline LUT path because (a) it's by far the most
common telephony codec, (b) LUT decode is one memory read per sample (~1 ns),
and (c) dragging libavcodec in for G.711 would multiply per-sample cost by 50×+.

### 5.2 Decoder lifecycle

Create on first packet for a given SSRC:

```c
switch (pt_kind) {
  case RTP_ASR_CODEC_G711_MU:
  case RTP_ASR_CODEC_G711_A:
    session->codec_ctx = rtp_asr_g711_ctx_new(pt_kind);  /* cheap, ~0 alloc */
    break;
  case RTP_ASR_CODEC_OPUS:
    session->codec_ctx = opus_decoder_create(48000, 1, &err);
    break;
  default:  /* FFmpeg path */
    AVCodec *c = avcodec_find_decoder(av_codec_id_for_pt(pt_kind));
    AVCodecContext *cc = avcodec_alloc_context3(c);
    cc->sample_rate = clock_rate;
    cc->channels = 1;
    avcodec_open2(cc, c, NULL);
    session->codec_ctx = cc;
}
```

Destroy on session aging.

### 5.3 Decode per packet

```c
switch (codec_kind) {
  case RTP_ASR_CODEC_G711_MU:
    for (i = 0; i < payload_len; i++) pcm_s16[i] = ulaw_to_s16_lut[payload[i]];
    n_samples = payload_len;
    break;
  case RTP_ASR_CODEC_OPUS:
    n_samples = opus_decode_float(opus_dec, payload, payload_len, pcm_f32, max_samples, 0);
    break;
  default: {
    AVPacket pkt = {...};
    avcodec_send_packet(cc, &pkt);
    avcodec_receive_frame(cc, frame);
    n_samples = frame->nb_samples;
  }
}
```

### 5.4 Resample to 16 kHz f32

Moonshine expects 16 kHz mono f32. Resample table:

| Input | Output | Implementation |
|---|---|---|
| 8 kHz s16 (G.711 / G.729) | 16 kHz f32 | libswresample, sinc interpolation (`SWR_FILTER_TYPE_KAISER`), format convert |
| 16 kHz s16 (G.722 audio) | 16 kHz f32 | libswresample format convert only |
| 48 kHz f32 (Opus) | 16 kHz f32 | libswresample downsample with low-pass anti-alias |

SwrContext is per-session, created once at session start, destroyed at session
end — same as MediaBridge's `AudioDecoder::mSwr` pattern.

### 5.5 Loss / reorder handling

- VPP RSS pinning of a 5-tuple keeps packets in order at the plugin layer.
- Track `last_seq`; on gap of 1–10 sequence numbers, insert
  `gap_samples_at_clock_rate` zeros into the PCM stream before feeding ASR
  (zero-fill PLC — crude, v1 only).
- Gap > 10: count as `stream_reset`, flush Sherpa stream, reset ASR state,
  start fresh segment.
- Late packet (`seq < last_seq`, modulo wrap, distance < 10): drop silently;
  increment `late_drops`.
- No jitter buffer. Explicit v1 scope — v2 may add a 20–50 ms reorder window.

## 6. VAD

Default: **Silero VAD** via Sherpa-ONNX built-in. ~1 MB model, runs in the same
thread as ASR, cheap relative to Moonshine.

Purpose: **gate segment finalization, not sample admission.**

- All decoded PCM goes to `sherpa_onnx_online_stream_accept_waveform()` regardless of VAD.
- Silero runs in parallel on the same frame.
- On `speech → silence` transition (tunable min-silence-ms), emit the current
  transcript segment and reset the Sherpa stream.
- On long silence (> 5 s), additionally pause the sherpa decode loop to save CPU.

Alternate: WebRTC VAD (smaller, BSD-style), selectable via config.

## 7. ASR runtime

### 7.1 Model — Moonshine v2 (default Base)

- Default: `moonshine-base.onnx` (58 MB).
- Optional: `moonshine-small-streaming.onnx` (123 MB, better WER) or
  `moonshine-medium-streaming.onnx` (245 MB, best WER).
- Model files not committed to git. Download script `models/fetch.sh` pulls from
  a checksum-verified mirror; plugin refuses to load on checksum mismatch.

### 7.2 Runtime — Sherpa-ONNX C API

Shared per Linux worker:

```c
SherpaOnnxOnlineRecognizerConfig cfg = {
  .model_config.moonshine = {
    .preprocessor = "...", .encoder = "...",
    .uncached_decoder = "...", .cached_decoder = "..."
  },
  .sample_rate = 16000,
  .feat_config = { .sample_rate = 16000, .feature_dim = 80 },
  .decoding_method = "greedy_search",   /* or "modified_beam_search" at higher cost */
  .enable_endpoint = 1,
  .rule1_min_trailing_silence = 2.4f,
  .rule2_min_trailing_silence = 1.2f,
  .rule3_min_utterance_length = 20.0f,
};
worker->recognizer = sherpa_onnx_create_online_recognizer(&cfg);
```

Per session:

```c
session->asr_stream = sherpa_onnx_create_online_stream(worker->recognizer);
```

Per payload:

```c
sherpa_onnx_online_stream_accept_waveform(session->asr_stream, 16000, pcm_f32, n);
while (sherpa_onnx_is_online_stream_ready(worker->recognizer, session->asr_stream))
    sherpa_onnx_decode_online_stream(worker->recognizer, session->asr_stream);
const SherpaOnnxOnlineRecognizerResult *r =
    sherpa_onnx_get_online_stream_result(worker->recognizer, session->asr_stream);
```

### 7.3 Segment finalization and emit

A segment finalizes when **any** of:
- `sherpa_onnx_online_stream_is_endpoint()` returns true
- Silero VAD reports speech→silence with min-silence-ms exceeded
- Wall-clock max-segment-duration exceeded (default 30 s, safety net)

On finalize: build a transcript record, push to emitter, then `sherpa_onnx_online_stream_reset(stream)`.

Record fields:

```json
{
  "ts_wall": "2026-04-24T13:37:42.891Z",
  "ts_rtp_first": 3123456789,
  "ts_rtp_last":  3123458901,
  "ssrc": "0x7f3a91c2",
  "src": "10.1.2.3:40000",
  "dst": "10.1.2.4:5004",
  "pt":  0,
  "codec": "PCMU",
  "text": "hello this is a test call",
  "is_final": true,
  "vad_conf": 0.91
}
```

## 8. Transcript emitter

Stackable sinks; config-selectable:

- **syslog** — human-readable one-liner per final segment. Dev / debug default.
- **JSON over UDP** — newline-delimited JSON to `host:port`. Default `127.0.0.1:7879`. Integrates with any log pipeline.
- **gRPC** — streaming RPC (`TranscriptService.Emit(stream TranscriptRecord) returns (google.protobuf.Empty)`) to a configurable endpoint.

Emitter runs on the Linux worker; backpressure is bounded (emitter buffer drops
oldest on overflow with a counter).

## 9. Configuration surface

### 9.1 Binary API (`rtp_asr.api`)

- `rtp_asr_interface_enable_disable { sw_if_index, enable_disable }` — bind plugin to interface
- `rtp_asr_config_set { model_path, model_variant, vad_kind, emitter_sink, emitter_target, worker_count }`
- `rtp_asr_session_dump` — streaming reply listing active sessions
- `rtp_asr_stats_get` — counters reply
- `rtp_asr_transcripts_subscribe` — optional streaming subscription for live transcripts

### 9.2 CLI

```
rtp-asr enable <interface>
rtp-asr disable <interface>
rtp-asr set model <path>
rtp-asr set model-variant base | small | medium
rtp-asr set vad silero | webrtc | off
rtp-asr set emitter syslog | json-udp <host:port> | grpc <endpoint>
rtp-asr show sessions
rtp-asr show stats
rtp-asr show transcripts tail [N]
```

## 10. Build system

CMake out-of-tree, cloned shape from `vpp-ndpi/CMakeLists.txt`:

- `find_package(VPP REQUIRED)` via installed `/usr/lib/cmake/VPP/VPPConfig.cmake`
- `pkg_check_modules(SHERPA_ONNX REQUIRED sherpa-onnx)`
- `pkg_check_modules(AV REQUIRED libavcodec libavutil libswresample)`
- `pkg_check_modules(OPUS REQUIRED opus)`
- Optional: `-DWITH_CUDA=on` enables ONNX Runtime CUDA EP; `-DWITH_TENSORRT=on`
  swaps Sherpa for a TRT Moonshine/Parakeet backend (v2 scope)
- `add_vpp_plugin(rtp-asr ...)` macro, single `rtp_asr_plugin.so` under
  `/usr/lib/vpp_plugins/`

License: Apache 2.0 (plugin code) + Apache 2.0 (Sherpa-ONNX dynamic link) + LGPL
2.1+ (FFmpeg dynamic link) + BSD (libopus dynamic link).

## 11. Test harness

Port vpp-ndpi's pytest structure:

- `test/test_codec_matrix.py` — for each PT in `{0, 8, 9, 18, 111}`, synthesize
  a pcap from a known wav (LibriSpeech-telephony reference), replay through VPP,
  assert transcript matches ground truth within target WER.
- `test/test_session_lifecycle.py` — concurrent SSRCs, mid-call SSRC rotation,
  session aging, session rebinding, ring-full backpressure.
- `test/test_perf_hot_path.py` — `perfmon` harness asserting ≤ 100 ns
  steady-state per packet in the VPP graph node.
- `test/test_emit_sinks.py` — syslog / JSON-UDP / gRPC end-to-end roundtrip.
- `labs/` — interactive tmux demo that replays a real-call pcap and tails
  transcripts.
- `models/` — fetch script + checksums; test assets (wav + pcap fixtures) kept
  small and committed, models downloaded at test setup.

## 12. Reuse map

From `~/projects/vpp-ndpi`:
- CMake scaffolding + `add_vpp_plugin` wiring
- Per-worker struct pattern (`ndpi_per_worker_t` → `rtp_asr_worker_t`)
- Feature arc registration (`VNET_FEATURE_INIT` on `ip4-unicast`/`ip6-unicast`)
- Bihash flow table + aging sweep (`ndpi_flow.c` → `rtp_session.c`)
- Binary API definition + stub-gen (`ndpi.api` → `rtp_asr.api`)
- CLI command registration (`ndpi_cli.c` pattern)
- Stats counter segment integration
- pytest + labs harness shape

From `~/projects/mediabridge`:
- Codec decode pattern (`AudioDecoder` wrapper over libavcodec → `codec_decode.c`)
- libswresample 8→16 kHz f32 flow
- Per-session struct pattern (`PortInfo_t` → `rtp_asr_session_t`)
- RX-thread vs worker-thread split with SPSC ring hand-off
- PT → (clock rate, codec id) table
- Sherpa-ONNX integration hooks — though we switch from
  WebSocket-to-remote-sherpa-server to **in-process Sherpa-ONNX C API**

**Intentional departures from MediaBridge:**

1. **In-process Sherpa-ONNX**, not WebSocket to remote server. Runs the
   recognizer inside the plugin's Linux worker thread. Eliminates network hop
   and a whole separate service to operate.
2. **Moonshine v2 by default**, not Whisper-tiny. Smaller, faster,
   streaming-native, MIT.
3. **Structured transcript emission** (JSON / gRPC), not stdout logging.
4. **RTP timestamp + wall-clock preserved per segment.**
5. **No SIP signaling.** Sessions created on first valid RTP packet for a new
   (5-tuple, SSRC). Keeps the plugin Kamailio-free and deployable as a pure
   passive tap.
6. **Inline G.711 LUT decode** instead of routing G.711 through libavcodec.

## 13. Non-functional targets (v1)

| Target | v1 number | How measured |
|---|---|---|
| Per-packet graph-node cost (bihash hit) | < 100 ns | `perfmon` in CI, per-node cycles counter |
| Per-packet graph-node cost (session create) | < 5 μs | same, at first packet of new SSRC |
| Concurrent sessions per Linux worker (Moonshine-base, CPU) | 50–100 target; 30 conservative | synthetic RTP load test |
| TTFT (speech onset → first emit) | < 500 ms with Moonshine-base | timed pcap replay |
| End-to-end segment latency (final emit after last sample) | < 1000 ms | same |
| WER, G.711 µ-law English telephony, cold model | baseline ~15–20 % on LibriSpeech-telephony | CI asserts against fixed fixture |
| Ring-full drop rate under 2× design load | < 0.1 % | stress test |
| Zero packet drops in the VPP graph (not ring) | 0 under any load | assertion in test |

## 14. Deferred to v2+

- GPU backend: `-DWITH_TENSORRT=on` swaps Sherpa-ONNX for an in-process
  TensorRT Moonshine/Parakeet engine
- Riva gRPC backend for customers who operate a central inference farm
- Multilingual / Whisper fallback for non-English traffic
- Fine-tuned Moonshine on G.711-degraded domain data — closes the 34 % WER
  "acoustic bottleneck" documented in Appendix A
- IPFIX exporter with per-session transcript summaries
- SIP/SDP signaling tap for dynamic-PT learning
- Jitter reorder window (20–50 ms) with proper PLC (libspeexdsp or libavcodec PLC)
- Session migration across Linux workers (for long-lived streams crossing worker rebalance)

## 15. Upstream-to-fd.io split

**Keep in packetlens repo:** `codec_decode.c`, `resample.c`, `vad.c`,
`asr_worker.c`, `sherpa_runtime.c`, `emitter.c`, binary API, CLI. These are
product, not framework — fd.io maintainers will not take ML inference or media
codec into the tree.

**Propose for upstream (narrow patches, one at a time):**

1. **RTP session-tracking feature arc** (`node_rtp_tap.c` + `rtp_session.c`,
   minus codec/ASR plumbing). Pure packet-processing primitive. Complements the
   existing in-tree `srtp` plugin. Usable by anyone writing RTP-aware plugins.
2. **Async worker helper in `src/vppinfra/`.** Generic cross-thread SPSC ring +
   off-path-Linux-thread dispatcher. Useful for crypto offload, ML scoring,
   heavy regex — not specific to ASR. Likely accepted as a shared utility if the
   API is clean.

Expected outcome: narrow plumbing accepted over 1–2 release cycles; ASR-specific
plugin stays out-of-tree, which is correct.

---

## Appendix A — Transcription stack decisions

- **Whisper is the wrong primary for telephony.** Acoustic bottleneck at 8 kHz
  caps accuracy near 34 % WER for acoustic-only adaptation; streaming degradation
  adds another ~5 % WER on top. Use only as offline/multilingual fallback.
- **Moonshine v2** is the right default model for the plugin: streaming-native
  (50–150 ms TTFT), 26–245 MB footprint, MIT license, designed for CPU-first
  edge deployment. 10.07 % WER (Base) on clean English — not leaderboard-topping
  but sufficient for telephony and well within fine-tuning range.
- **Parakeet-TDT / Canary via Riva** — accuracy upsell and multilingual path;
  reachable via same Sherpa-ONNX abstraction (Parakeet-ONNX) for CPU, or
  TensorRT / Riva gRPC for GPU customers.
- **The accuracy lift on real telephony comes from fine-tuning**, not from
  bigger models. Deferred to v2.
