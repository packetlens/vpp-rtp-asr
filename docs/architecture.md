# Architecture

Canonical architecture lives in [FUNC_SPEC.md §2](../FUNC_SPEC.md#2-top-level-architecture).
This file collects implementation-facing notes that don't belong in the spec.

## Why split VPP and Linux workers at all

VPP's graph runs on pinned worker threads with a per-packet budget measured in
nanoseconds. Sherpa-ONNX inference with Moonshine-base measures in tens to
hundreds of milliseconds per chunk. Running the recognizer inline in the graph
would starve the forwarding loop across a range of 5–6 orders of magnitude —
not a tunable, a structural mismatch. Every PacketFlow plugin that needs heavy
off-path work (crypto offload, ML scoring, DB lookups) ends up with this same
shape; the "async worker helper" upstream candidate generalizes it.

## Ring sizing

Entry size: `{session_idx: u32, payload_offset: u16, payload_len: u16, rtp_ts: u32, vpp_time: f64}`
≈ 24 B. With 16 K entries per worker: ~384 KB per VPP worker, negligible L2
footprint. Sized to hold ~1 s of worst-case burst even if the Linux worker
stalls briefly.

## Why static VPP-to-Linux binding

Dynamic migration (move a session between Linux workers as load shifts)
requires serialized Sherpa stream state — the Sherpa ONNX runtime doesn't
expose that today. Static binding keeps the design honest; rebalance is
explicitly deferred to v2.

## Sherpa thread-safety

Sherpa-ONNX's `OnlineRecognizer` is shared across Linux worker threads (one
per plugin load). Each worker owns the streams it creates; streams are
**not** shared across threads. This matches the Sherpa docs' stated
thread-safety contract.

## Copy of the payload, not the mbuf

The graph node copies the RTP payload bytes into the ring, not a pointer into
the VPP buffer. This sounds wasteful but is not: (a) typical telephony
payloads are 20–160 B per packet, (b) buffer lifetime is undefined once we
return `NEXT_PASS`, and (c) copy-free would require buffer cloning, which is
more expensive than the 160-B copy.
