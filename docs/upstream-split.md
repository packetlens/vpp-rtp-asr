# Upstream-to-fd.io split

The plugin as a whole is not a candidate for fd.io/vpp upstream — fd.io
maintainers consistently keep ML inference, media codecs, and
deployment-specific glue out of the tree. Two narrower pieces are plausible
patches on their own.

## Candidate 1 — generic RTP session-tracking feature arc

Files involved: `node_rtp_tap.c` + `rtp_session.c`, stripped of any
codec/ASR/emitter plumbing.

What it would be in-tree: a small plugin (`src/plugins/rtp_observe/` or
similar) that provides

- Feature-arc graph node on ip4-unicast / ip6-unicast
- Per-SSRC session table with aging
- Counters and CLI (`show rtp-observe sessions`)
- A registration API other plugins can use to hook into RTP session
  events (create / tear-down / packet-arrival) — the "hook" pattern that
  lets the ASR plugin, a future RTCP analyzer, or a telemetry exporter
  all consume the same session state.

Precedent: `src/plugins/srtp/` already lives in-tree. RTP as a separate
plugin that SRTP can layer on top is a clean evolution.

Expected resistance: "does anyone else need this?" — answered by pointing at
the SRTP plugin and at downstream consumers (packetlens/vpp-rtp-asr, future
RTCP analysis tools).

## Candidate 2 — async off-path worker helper

Files involved: the SPSC-ring + Linux-thread dispatcher parts of
`asr_worker.c`.

What it would be in-tree: a small utility in `src/vppinfra/async_offload.h`
+ matching `.c` that gives plugins

- A pool of Linux threads managed by VPP (pinning, lifecycle tied to
  `vlib_main_t`)
- Per-VPP-worker SPSC rings to dispatch opaque work items
- A return-path mechanism (either a second ring or a message into the VPP
  input node) so results arrive on the right VPP worker

Use cases beyond ASR: crypto offload (TLS handshake, SRTP key derivation),
heavy regex (Hyperscan batches), ML scoring, database lookups.

Expected resistance: "VPP has vlib_main_t_thread_main for this" — answer:
VPP's existing thread model is pinned-worker + main thread, not arbitrary
Linux worker pool. Off-path inference / crypto doesn't fit the pinned
worker model (it'd steal cycles from forwarding). The helper is specifically
for work that must not run on a VPP worker.

## What stays here, forever

- `codec_decode.c` — FFmpeg + libopus glue and the G.711 LUT
- `resample.c` — libswresample wrapper
- `vad.c` — Silero / WebRTC VAD
- `sherpa_runtime.c` — Sherpa-ONNX lifecycle
- `emitter.c` — syslog / JSON / gRPC sinks
- `rtp_asr_api.c`, `rtp_asr_cli.c`, `rtp_asr.api` — plugin-specific API surface

These are product. fd.io doesn't take products.

## Process

1. Finish v1 of the plugin in this repo; make sure the codec/ASR separation
   in the source tree is clean enough that extracting "just the datapath"
   is a mechanical split, not a rewrite.
2. Draft RFC on vpp-dev@lists.fd.io for candidate #1 (RTP observe plugin).
   Keep it under 200 lines of prose; include a small code sample of the
   hook API.
3. If RFC gets nods, prepare a gerrit change with the plugin + tests; do
   NOT bundle the async helper in the same patch — separate review threads
   have a better chance.
4. Candidate #2 (async worker helper) follows only after #1 lands, and only
   if the in-tree API is clearly the right shape (the one we use in this
   out-of-tree plugin is a proof of usefulness, not a finished API).
