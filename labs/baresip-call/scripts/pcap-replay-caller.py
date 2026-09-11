#!/usr/bin/env python3
"""Replay a G.711 µ-law RTP stream from the Moonshine test WAV over UDP.

Builds RTP packets using rtp_synth.wav_to_ulaw(), then sends them via a
plain UDP socket to VPP's host-interface IP at 50 pps (160 samples / 20 ms
per packet).  Real UDP packets flow through the Docker bridge → VPP
host-interface → rtp-asr tap → Moonshine ASR.

Environment:
    VPP_IP    destination IP (default 10.66.22.10)
    VPP_PORT  destination RTP port (default 16400)
    REPEATS   how many times to loop the audio (default 3)
    WAV       path to input WAV (default Moonshine test 8k.wav)
"""

import os
import socket
import struct
import sys
import time

sys.path.insert(0, "/src/test")
from rtp_synth import wav_to_ulaw  # noqa: E402

VPP_IP   = os.environ.get("VPP_IP",   "10.66.22.10")
VPP_PORT = int(os.environ.get("VPP_PORT", "16400"))
REPEATS  = int(os.environ.get("REPEATS",  "3"))
WAV      = os.environ.get("WAV", "/models/sherpa-onnx-moonshine-tiny-en-int8/test_wavs/8k.wav")

SAMPLES_PER_PKT = 160       # 20 ms @ 8 kHz
PTIME_S         = SAMPLES_PER_PKT / 8000.0   # 0.020 s
SSRC            = 0xDEAD1234
PT              = 0         # G.711 µ-law

def make_rtp(seq: int, ts: int, payload: bytes) -> bytes:
    hdr = struct.pack("!BBHII", 0x80, PT, seq & 0xFFFF, ts & 0xFFFFFFFF, SSRC)
    return hdr + payload

_, ulaw = wav_to_ulaw(WAV)
pkts_per_loop = (len(ulaw) + SAMPLES_PER_PKT - 1) // SAMPLES_PER_PKT
total_pkts = pkts_per_loop * REPEATS
total_dur  = PTIME_S * total_pkts

print(f"[pcap-replay] WAV={WAV}  {pkts_per_loop} pkt/loop × {REPEATS} = {total_pkts} pkts  ({total_dur:.1f}s)", flush=True)
print(f"[pcap-replay] → {VPP_IP}:{VPP_PORT}", flush=True)

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

seq = 0
ts  = 0
t0  = time.monotonic()

for rep in range(REPEATS):
    for off in range(0, len(ulaw), SAMPLES_PER_PKT):
        frame = ulaw[off : off + SAMPLES_PER_PKT]
        if not frame:
            break
        pkt = make_rtp(seq, ts, frame)
        sock.sendto(pkt, (VPP_IP, VPP_PORT))

        seq += 1
        ts  += SAMPLES_PER_PKT

        # pace at 50 pps — sleep until next scheduled send time
        deadline = t0 + seq * PTIME_S
        drift = deadline - time.monotonic()
        if drift > 0:
            time.sleep(drift)

    print(f"[pcap-replay] loop {rep + 1}/{REPEATS} done  (seq={seq})", flush=True)

sock.close()
print(f"[pcap-replay] sent {total_pkts} RTP packets — holding 30s for VPP to finish transcribing", flush=True)
time.sleep(30)
