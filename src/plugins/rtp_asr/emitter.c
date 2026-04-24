/*
 * emitter.c - Transcript emitter: syslog / JSON-UDP / gRPC.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * See FUNC_SPEC.md §8 for record schema. Emitter runs on the Linux worker
 * thread; bounded backpressure with oldest-drop policy on overflow.
 */

#include <rtp_asr/rtp_asr.h>

/* TODO(v1):
 *   - rtp_asr_emitter_init(sink, target):
 *       sink=0  → openlog("vpp-rtp-asr", LOG_PID, LOG_DAEMON)
 *       sink=1  → parse target "host:port", socket(AF_INET, SOCK_DGRAM)
 *       sink=2  → gRPC channel via generated stubs (v1 can ship sink=0/1,
 *                 gRPC behind feature flag)
 *   - rtp_asr_emitter_publish(session, text, is_final):
 *       format JSON with fields per FUNC_SPEC.md §7.3, dispatch to the
 *       configured sink.
 *   - rtp_asr_emitter_shutdown().
 */
