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

  /* codec + ASR — opaque pointers; allocated on first decode. */
  void *codec_ctx;
  void *swr_ctx;
  void *asr_stream;
  void *vad_ctx;

  /* Per-session PCM accumulator @ 16 kHz f32 for chunk-based offline ASR. */
  f32 *pcm_window;
  u32  pcm_samples;
  u32  pcm_window_cap;
  u32  segment_rtp_ts_first;
  u32  segment_rtp_ts_last;
  u64  segments_emitted;
  u32  owned_linux_worker_id;
} rtp_asr_session_t;

/* Per-VPP-worker state. One struct per VPP worker thread. */
typedef struct
{
  rtp_asr_session_t *sessions;    /* pool */
  clib_bihash_16_8_t v4_ht;
  clib_bihash_48_8_t v6_ht;

  /* SPSC ring to this worker's assigned Linux thread. Opaque in this
   * header; rtp_asr_ring_t defined in asr_worker.c. */
  void *payload_ring;

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
  f32 segment_seconds;         /* how much audio to accumulate per sherpa call */

  /* model + emitter */
  char *model_dir;             /* NULL = ASR disabled; plugin still taps RTP */
  u8    emitter_sink;          /* 0=syslog, 1=json-udp */
  char *emitter_target;

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

/* Supported codec kinds — v1 */
typedef enum
{
  RTP_ASR_CODEC_UNKNOWN = 0,
  RTP_ASR_CODEC_G711_MU,
  RTP_ASR_CODEC_G711_A,
  RTP_ASR_CODEC_G722,
  RTP_ASR_CODEC_G729,
  RTP_ASR_CODEC_OPUS,
  RTP_ASR_CODEC_AMR_WB,
  RTP_ASR_CODEC_DYNAMIC,
} rtp_asr_codec_t;

/* codec_decode.c */
void rtp_asr_codec_init (void);
int  rtp_asr_codec_setup_for_session (rtp_asr_session_t *s);
int  rtp_asr_codec_decode (rtp_asr_session_t *s,
			   const u8 *payload, u32 payload_len,
			   f32 *pcm_f32, u32 *n_samples);
void rtp_asr_codec_teardown (rtp_asr_session_t *s);

/* resample.c */
int  rtp_asr_resample_to_16k (rtp_asr_session_t *s,
			      const f32 *in, u32 in_samples,
			      f32 *out, u32 *out_samples);
void rtp_asr_resample_teardown (rtp_asr_session_t *s);

/* asr_worker.c */
int  rtp_asr_worker_pool_start (u32 vpp_worker_count);
void rtp_asr_worker_pool_stop (void);
int  rtp_asr_ring_enqueue (rtp_asr_worker_t *w, u32 session_idx,
			   const u8 *payload, u32 payload_len,
			   u32 rtp_ts, f64 vpp_time);
u64  rtp_asr_worker_processed_total (void);
u64  rtp_asr_worker_decode_errors_total (void);
u64  rtp_asr_worker_segments_total (void);

/* sherpa_runtime.c */
int  rtp_asr_sherpa_global_init (const char *model_dir);
int  rtp_asr_sherpa_is_loaded (void);
/* Decode a single chunk of 16 kHz mono f32; writes the transcript (nul-term)
 * into text_buf (size text_buf_cap). Returns number of chars written, 0 if
 * empty, negative on error. Thread-safe: each call allocates its own stream. */
int  rtp_asr_sherpa_decode_chunk (const f32 *pcm_16k, u32 n,
				  char *text_buf, u32 text_buf_cap);
void rtp_asr_sherpa_global_shutdown (void);

/* emitter.c */
int  rtp_asr_emitter_init (u8 sink, const char *target);
void rtp_asr_emitter_publish (const rtp_asr_session_t *s,
			      const char *text, u8 is_final,
			      u32 rtp_ts_first, u32 rtp_ts_last);
void rtp_asr_emitter_shutdown (void);

#endif /* __included_vpp_rtp_asr_h__ */
