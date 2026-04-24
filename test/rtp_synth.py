"""Synthesize a G.711 µ-law RTP pcap from a mono PCM wav.

Each RTP packet carries 20 ms of audio = 160 samples = 160 bytes of µ-law.
Source IP / dst IP / ports / SSRC are configurable. Written for test harness
use only — not production-grade.
"""

import argparse
import struct
import wave

from scapy.all import Ether, IP, UDP, Raw, wrpcap


def s16_to_ulaw(sample: int) -> int:
    """Convert one int16 sample to 8-bit µ-law (ITU-T G.711)."""
    BIAS = 0x84
    CLIP = 32635
    sign = 0x00
    if sample < 0:
        sample = -sample
        sign = 0x80
    if sample > CLIP:
        sample = CLIP
    sample += BIAS
    exponent = 7
    mask = 0x4000
    while not (sample & mask):
        exponent -= 1
        mask >>= 1
        if exponent == 0:
            break
    mantissa = (sample >> (exponent + 3)) & 0x0f
    ulaw = ~(sign | (exponent << 4) | mantissa) & 0xff
    return ulaw


def wav_to_ulaw(wav_path: str) -> tuple[int, bytes]:
    """Return (sample_rate, µ-law-encoded 8 kHz mono bytes)."""
    with wave.open(wav_path, "rb") as w:
        if w.getnchannels() != 1:
            raise ValueError("wav must be mono")
        if w.getsampwidth() != 2:
            raise ValueError("wav must be 16-bit PCM")
        rate = w.getframerate()
        frames = w.readframes(w.getnframes())

    samples = struct.unpack(f"<{len(frames)//2}h", frames)

    if rate != 8000:
        # Simple linear downsample to 8 kHz.
        step = rate / 8000.0
        new_len = int(len(samples) / step)
        samples = [samples[int(i * step)] for i in range(new_len)]
        rate = 8000

    ulaw = bytes(s16_to_ulaw(s) for s in samples)
    return rate, ulaw


def build_rtp_pcap(
    ulaw_bytes: bytes,
    pcap_path: str,
    *,
    src_ip: str = "10.0.0.2",
    dst_ip: str = "10.0.0.1",
    src_port: int = 16400,
    dst_port: int = 16400,
    ssrc: int = 0x7F3A91C2,
    pt: int = 0,
    samples_per_packet: int = 160,
    dst_mac: str = "ff:ff:ff:ff:ff:ff",
):
    pkts = []
    seq = 0
    ts = 0
    total = len(ulaw_bytes)
    for off in range(0, total, samples_per_packet):
        frame = ulaw_bytes[off : off + samples_per_packet]
        # 12-byte RTP header: V=2, P=0, X=0, CC=0, M=0, PT=pt, seq, ts, ssrc.
        vpxcc = (2 << 6) | 0  # V=2, P=0, X=0, CC=0
        mpt = pt & 0x7F
        rtp = struct.pack("!BBHII", vpxcc, mpt, seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc)
        pkt = (
            Ether(dst=dst_mac)
            / IP(src=src_ip, dst=dst_ip)
            / UDP(sport=src_port, dport=dst_port)
            / Raw(load=rtp + frame)
        )
        pkts.append(pkt)
        seq += 1
        ts += samples_per_packet

    wrpcap(pcap_path, pkts)
    return len(pkts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("pcap")
    args = ap.parse_args()
    _, ulaw = wav_to_ulaw(args.wav)
    n = build_rtp_pcap(ulaw, args.pcap)
    print(f"wrote {args.pcap}: {n} RTP packets, {len(ulaw)} audio samples")


if __name__ == "__main__":
    main()
