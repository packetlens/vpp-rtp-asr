#!/usr/bin/env python3
"""Count RTP packets arriving at the sink — diagnostic for VPP forwarding."""
import socket
import sys
import time

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 16400))
s.settimeout(90)

print("[sink] Counting RTP packets on UDP :16400", flush=True)

count = 0
start = time.time()
try:
    while True:
        data, addr = s.recvfrom(4096)
        count += 1
        if count % 50 == 0:
            elapsed = time.time() - start
            print(
                f"[sink] {count} pkts  {count/elapsed:.1f} pps  {len(data)}b last",
                flush=True,
            )
except socket.timeout:
    pass

elapsed = time.time() - start
print(
    f"[sink] TOTAL: {count} packets in {elapsed:.1f}s  ({count/elapsed:.1f} pps avg)",
    flush=True,
)
sys.exit(0)
