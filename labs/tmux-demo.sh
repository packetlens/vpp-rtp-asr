#!/usr/bin/env bash
# Copyright (c) 2026 PacketFlow (packetflow.dev)
# SPDX-License-Identifier: Apache-2.0
#
# Interactive demo: replay a pre-recorded RTP pcap through VPP with the
# rtp-asr plugin loaded, tail transcripts in a split pane.
#
# Requires: tmux, tcpreplay, a pcap fixture under test/fixtures/, and the
# plugin built (scripts/build-plugin.sh).

set -euo pipefail

SESSION="rtp-asr-demo"
tmux new-session -d -s "$SESSION" -n main

# pane 0: VPP CLI
tmux send-keys -t "$SESSION:main" \
  'echo "pane 0: vppctl  (try:  show rtp-asr stats | sessions | transcripts tail 20)"' Enter

# pane 1: transcript tail
tmux split-window -v -t "$SESSION:main"
tmux send-keys -t "$SESSION:main.1" \
  'echo "pane 1: socat -u UDP-RECVFROM:7879,fork - | jq ."' Enter

# pane 2: pcap replay
tmux split-window -h -t "$SESSION:main.0"
tmux send-keys -t "$SESSION:main.2" \
  'echo "pane 2: sudo tcpreplay -i <veth> test/fixtures/sample-call.pcap"' Enter

tmux attach -t "$SESSION"
