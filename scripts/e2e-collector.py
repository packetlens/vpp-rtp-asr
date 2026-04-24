#!/usr/bin/env python3
"""
Collect JSON-UDP transcripts from vpp-rtp-asr, assert ≥5/7 pangram content
words found, exit 0 on pass / 1 on fail.

Runs inside the collector service container in compose-e2e.yaml.
"""

import json
import os
import socket
import sys
import time

_default_words = ["quick", "brown", "fox", "jump", "lazy", "dog", "over"]
EXPECTED_WORDS = os.environ.get("EXPECTED_WORDS_CSV", ",".join(_default_words)).split(",")
MIN_WORD_HITS  = int(os.environ.get("MIN_WORD_HITS", "5"))
LISTEN_HOST    = "0.0.0.0"
LISTEN_PORT    = 17879
# Total collection window: caller streams ~24s (6 loops) + 2s Moonshine latency
TIMEOUT_S      = 120.0
# Once we get at least one transcript, wait this long for more before stopping.
DRAIN_S        = 8.0

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind((LISTEN_HOST, LISTEN_PORT))
sock.settimeout(2.0)

print(f"[collector] Listening on UDP {LISTEN_HOST}:{LISTEN_PORT}", flush=True)
print(f"[collector] Expecting ≥{MIN_WORD_HITS}/{len(EXPECTED_WORDS)} words: {EXPECTED_WORDS}", flush=True)

transcripts = []
deadline       = time.time() + TIMEOUT_S
drain_deadline = None

while time.time() < deadline:
    try:
        data, addr = sock.recvfrom(4096)
    except socket.timeout:
        if drain_deadline and time.time() >= drain_deadline:
            break
        continue

    try:
        record = json.loads(data.decode("utf-8"))
    except json.JSONDecodeError as exc:
        print(f"[collector] bad JSON from {addr}: {exc}: {data!r}", flush=True)
        continue

    text = record.get("text", "")
    print(f"[collector] [{addr[0]}] transcript: {text!r}", flush=True)
    transcripts.append(record)

    # Start a drain window after first transcript
    if drain_deadline is None:
        drain_deadline = time.time() + DRAIN_S
    else:
        drain_deadline = max(drain_deadline, time.time() + DRAIN_S)

sock.close()

if not transcripts:
    print("[collector] FAIL: no transcripts received within timeout", flush=True)
    sys.exit(1)

full_text = " ".join(r.get("text", "") for r in transcripts).lower()
hits      = [w for w in EXPECTED_WORDS if w in full_text]

print(f"[collector] full transcript: {full_text!r}", flush=True)
print(f"[collector] hits {len(hits)}/{len(EXPECTED_WORDS)}: {hits}", flush=True)
missing = [w for w in EXPECTED_WORDS if w not in full_text]
if missing:
    print(f"[collector] missing: {missing}", flush=True)

if len(hits) < MIN_WORD_HITS:
    print(
        f"[collector] FAIL: need {MIN_WORD_HITS}/{len(EXPECTED_WORDS)}, "
        f"got {len(hits)}",
        flush=True,
    )
    sys.exit(1)

print(
    f"[collector] PASS: {len(hits)}/{len(EXPECTED_WORDS)} expected words found",
    flush=True,
)
sys.exit(0)
