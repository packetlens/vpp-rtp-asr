# baresip-call lab

End-to-end baresip VoIP call routed through VPP with live transcription.

## What it demonstrates

Two baresip SIP clients make a real G.711 µ-law call. VPP sits in the path
as an L3 router with the `rtp-asr-tap` feature enabled on both interfaces.
Every RTP packet — in both directions — is intercepted by the plugin,
decoded, and fed to Moonshine for transcription. Transcripts appear in
real-time in the collector container log.

```
[alice (caller)] ── SIP/RTP ──► [VPP router] ── SIP/RTP ──► [bob (callee)]
                                 rtp-asr-tap                   auto-answer
                                 Moonshine-tiny
                                      │ JSON-UDP
                                      ▼
                               [collector]  (validates transcript)
```

## Prerequisites

```bash
# From the repo root — build the VPP plugin
docker compose run --rm build

# Fetch models (if not done)
bash models/fetch.sh --tiny-only
```

The baresip image is built automatically by `docker compose` (Dockerfile.baresip
in this directory — just Ubuntu + `baresip-core` package).

## Run

```bash
# From the repo root:
docker compose -f labs/baresip-call/compose.yaml up

# In another terminal, watch the transcript arrive:
docker compose -f labs/baresip-call/compose.yaml logs -f collector
```

Expected output in `collector` logs:
```
[collector] Listening on UDP 0.0.0.0:17879
[collector] [172.20.0.10] transcript: 'yet these thoughts affected hester prynne...'
[collector] hits 4/7: ['thoughts', 'hope', 'hester', 'apprehension']
[collector] PASS: 4/7 expected words found
```

(The bundled test wav is a Hawthorne excerpt; the pangram-based word list from
`e2e-collector.py` won't match — collector exits 0 if ≥5/7 pangram words are
found, which won't happen here. For the baresip lab the collector just shows the
transcripts — see §Validation below for checking manually.)

## Architecture detail

| Container | Network | IP | Role |
|---|---|---|---|
| `vpp` | caller-net | 172.22.0.10 (af_packet) | L3 router, rtp-asr-tap |
| `vpp` | callee-net | 172.23.0.10 (af_packet) | L3 router, rtp-asr-tap |
| `vpp` | mgmt-net | 172.20.0.10 (kernel) | JSON-UDP emitter |
| `caller` | caller-net | 172.22.0.20 | baresip alice |
| `callee` | callee-net | 172.23.0.20 | baresip bob (answermode=auto) |
| `collector` | mgmt-net | 172.20.0.30 | UDP transcript listener |

**Routes added by startup scripts:**
- caller: `172.23.0.0/24 via 172.22.0.10` (reach callee through VPP)
- callee: `172.22.0.0/24 via 172.23.0.10` (reach caller through VPP)

VPP handles ARP on both af_packet interfaces; kernel retains eth0 (mgmt) for
the emitter's own UDP sockets.

## Codec

G.711 µ-law (PCMU, PT=0, 8 kHz) — baresip is configured with `audio_codecs
pcmu/8000/1`. VPP's rtp-asr-tap decodes G.711 with an inline 256-entry LUT
(the lowest-latency path). Audio is streamed from the Moonshine test wav
(`models/.../test_wavs/8k.wav`) looped for the duration of the call.

## Validation

Check VPP stats: `docker compose -f labs/baresip-call/compose.yaml exec vpp vppctl -s /run/vpp/cli.sock show rtp-asr stats`

Expected: `rtp packets:` counter increasing, `sherpa loaded: yes`.

Check transcripts: `docker compose -f labs/baresip-call/compose.yaml logs collector`
