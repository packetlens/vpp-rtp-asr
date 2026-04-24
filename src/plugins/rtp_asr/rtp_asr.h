/*
 * rtp_asr.h - VPP RTP-ASR plugin: main types and per-worker state.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __included_vpp_rtp_asr_h__
#define __included_vpp_rtp_asr_h__

#include <vnet/vnet.h>
#include <vnet/ip/ip.h>
#include <vppinfra/bitmap.h>
#include <vppinfra/bihash_16_8.h>
#include <vppinfra/bihash_48_8.h>

#define RTP_ASR_PLUGIN_VERSION "0.1.0"

#define RTP_ASR_SESSIONS_PER_WORKER_DFLT (1u << 16)  /* 64K */
#define RTP_ASR_SESSION_EXPIRY_DFLT      30.0f       /* seconds */

/* RTP session keys. IPv4 key fits in the 16-byte bihash slot, IPv6 in 48. */

typedef struct
{
  ip4_address_t src;
  ip4_address_t dst;
  u16 src_port;
  u16 dst_port;
  u32 ssrc;
} rtp_asr_session_key4_t;
STATIC_ASSERT_SIZEOF (rtp_asr_session_key4_t, 16);

typedef struct
{
  ip6_address_t src;
  ip6_address_t dst;
  u16 src_port;
  u16 dst_port;
  u32 ssrc;
  u8  pad[8];
} rtp_asr_session_key6_t;
STATIC_ASSERT_SIZEOF (rtp_asr_session_key6_t, 48);

/* Per-session state. Stored in a per-worker pool; bihash value is pool idx.
 * The codec + ASR fields are void* because later passes (codec_decode.c,
 * sherpa_runtime.c, asr_worker.c) own them and don't need to be visible to
 * the graph node. */
typedef struct
{
  /* identity */
  u8  is_ip6;
  u8  pad0[3];
  u16 src_port;
  u16 dst_port;
  u32 ssrc;
  union
  {
    rtp_asr_session_key4_t key4;
    rtp_asr_session_key6_t key6;
  };

  /* RTP state */
  u8  payload_type;
  u8  pad1[3];
  u16 last_seq;
  u16 pad2;
  u32 last_rtp_ts;
  u32 clock_rate;

  /* counters */
  u64 packets;
  u64 bytes;
  u64 late_drops;
  u64 stream_resets;

  f64 first_seen;
  f64 last_seen;
  u32 sw_if_index;

  /* codec + ASR — opaque; NULL in minimum-compile scope */
  void *codec_ctx;
  void *swr_ctx;
  void *asr_stream;
  void *vad_ctx;
  f32  *pcm_window;
  u32   pcm_samples;
  u32   owned_linux_worker_id;
} rtp_asr_session_t;

/* Per-VPP-worker state. One struct per VPP worker thread. */
typedef struct
{
  rtp_asr_session_t *sessions;    /* pool */
  clib_bihash_16_8_t v4_ht;
  clib_bihash_48_8_t v6_ht;

  /* counters */
  u64 packets;
  u64 bytes;
  u64 new_sessions;
  u64 expired_sessions;
  u64 malformed_rtp;
  u64 ring_full_drops;
  u64 not_rtp;           /* packets that didn't match the RTP heuristic */
  u64 not_udp;           /* non-UDP packets passing through the node */

  int initialized;
} rtp_asr_worker_t;

/* Plugin main */
typedef struct
{
  vlib_main_t *vlib_main;
  vnet_main_t *vnet_main;

  rtp_asr_worker_t *per_worker;  /* vec, one per worker */
  uword            *enabled_interfaces;  /* bitmap */

  /* config */
  u32 sessions_per_worker;
  f32 session_expiry_seconds;
  u16 rtp_port_min;
  u16 rtp_port_max;
  u16 rtp_port_well_known;

  u16 msg_id_base;
  vlib_log_class_t log_class;
} rtp_asr_main_t;

extern rtp_asr_main_t rtp_asr_main;

extern vlib_node_registration_t rtp_asr_tap_node;

typedef enum
{
  RTP_ASR_NEXT_PASS,
  RTP_ASR_N_NEXT,
} rtp_asr_next_t;

/* rtp_asr.c */
int rtp_asr_interface_enable_disable (u32 sw_if_index, int enable);

/* rtp_session.c */
int rtp_asr_session_table_init (rtp_asr_worker_t *w, u32 capacity);
rtp_asr_session_t *rtp_asr_session_lookup_or_create4 (
    rtp_asr_worker_t *w, const rtp_asr_session_key4_t *key, int *created);
rtp_asr_session_t *rtp_asr_session_lookup_or_create6 (
    rtp_asr_worker_t *w, const rtp_asr_session_key6_t *key, int *created);

#endif /* __included_vpp_rtp_asr_h__ */
