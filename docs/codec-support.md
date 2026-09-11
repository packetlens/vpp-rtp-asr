# Codec support

| PT | Codec | Clock | Decoder | v1 status |
|----|---|---|---|---|
| 0  | PCMU (µ-law) | 8 kHz | inline LUT | planned (priority 1) |
| 8  | PCMA (A-law) | 8 kHz | inline LUT | planned (priority 1) |
| 9  | G.722 | 16 kHz | libavcodec | planned |
| 18 | G.729 | 8 kHz  | libavcodec (if built in) | best-effort |
| 111* | Opus | 48 kHz | libopus | planned |
| dynamic | AMR-WB | 16 kHz | libavcodec | v2 |
| dynamic | EVS | 16 kHz | libavcodec | v2 (license-dependent) |
| dynamic | iLBC | 8 kHz | libavcodec | v2 |

\* Opus dynamic PT varies; default 111 from RFC 7587 is a common convention
but never guaranteed. Learn from SDP in v2; for v1 operators configure an
accept-list via `vppctl rtp-asr set codec-pt <PT> opus`.

## G.711: why inline LUT

G.711 is ~80 % of domestic telephony traffic. µ-law → s16 is one table read
per sample. Routing this through libavcodec adds (a) per-packet
`avcodec_send_packet` / `avcodec_receive_frame` overhead, (b) AVFrame alloc
churn, (c) format-conversion passes. At 8 kHz × 160 samples/packet × 50
packets/s/session × 100 concurrent sessions, the overhead compounds fast.
The LUT is 1 KB of static data and one `for` loop of L1-hot reads.

## Opus: why libopus, not libavcodec

libavcodec's Opus decoder is a wrapper over libopus. The wrapper adds
AVFrame plumbing we don't need — the Sherpa pipeline wants raw f32 samples.
Calling `opus_decode_float()` directly is shorter, faster, and eliminates
one allocation per packet.

## Resampling

All decoded PCM is resampled to 16 kHz mono f32 via libswresample. Moonshine
(and Parakeet / Zipformer / Whisper) expect this format. Resampling to 16 kHz
from 8 kHz does **not** recover frequency content above 4 kHz — see
[moonshine-tuning.md](moonshine-tuning.md) for the quality implications.
