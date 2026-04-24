/*
 * rtp_session.c - Per-SSRC session table (bihash + pool) and aging sweep.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mirrors the pattern from vpp-ndpi/src/plugins/ndpi/ndpi_flow.c.
 * Separate bihash per AF: clib_bihash_16_8_t for IPv4, clib_bihash_40_8_t
 * for IPv6.
 *
 * Upstream candidate: this file plus node_rtp_tap.c (with codec/ASR
 * removed) are the "RTP session-tracking feature arc" proposed to fd.io.
 */

#include <rtp_asr/rtp_asr.h>

/* TODO(v1):
 *   - rtp_asr_session_lookup_or_create():
 *       key = {src.as_u64[0], src.as_u64[1], dst.as_u64[0], dst.as_u64[1], ssrc}
 *       per-AF bihash lookup; on miss pool-alloc, insert, set fields
 *   - rtp_asr_session_aging_sweep(): walk pool, free entries where
 *     vpp_time_now() - last_packet_vpp_time > session_expiry_seconds
 *   - emit "session closing" notification to the Linux worker over the
 *     per-worker ring so codec + ASR contexts get torn down cleanly
 */
