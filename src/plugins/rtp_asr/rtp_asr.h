/*
 * rtp_asr.h - VPP RTP-ASR plugin: main types and per-worker state.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __included_rtp_asr_h__
#define __included_rtp_asr_h__

#include <vnet/vnet.h>
#include <vnet/ip/ip.h>
#include <vppinfra/bihash_16_8.h>
#include <vppinfra/bihash_40_8.h>

#define RTP_ASR_PLUGIN_VERSION "0.1.0"

/* Supported codec kinds — v1 */
typedef enum
{
  RTP_ASR_CODEC_UNKNOWN = 0,
  RTP_ASR_CODEC_G711_MU,        /* PT 0,  8 kHz, inline LUT */
  RTP_ASR_CODEC_G711_A,         /* PT 8,  8 kHz, inline LUT */
  RTP_ASR_CODEC_G722,           /* PT 9,  16 kHz audio, libavcodec */
  RTP_ASR_CODEC_G729,           /* PT 18, 8 kHz, libavcodec */
  RTP_ASR_CODEC_OPUS,           /* dynamic PT, 48 kHz, libopus */
  RTP_ASR_CODEC_AMR_WB,         /* dynamic PT, 16 kHz, libavcodec */
  RTP_ASR_CODEC_DYNAMIC,        /* config-driven, libavcodec by name */
} rtp_asr_codec_t;

/* Per-session state. Some fields live on the VPP side (read-only for Linux
 * workers); codec_ctx / swr_ctx / asr_stream / vad_ctx are allocated and owned
 * by the assigned Linux worker. */
typedef struct
{
  /* identity */
  ip46_address_t src, dst;
  u16 src_port, dst_port;
  u32 ssrc;

  /* RTP state */
  u8  payload_type;
  u16 last_seq;
  u32 last_rtp_ts;
  u32 clock_rate;
  f64 last_packet_vpp_time;
  f64 first_packet_vpp_time;

  /* codec + ASR — opaque to VPP; owned by Linux worker */
  u32   owned_linux_worker_id;
  void *codec_ctx;
  void *swr_ctx;
  void *asr_stream;
  void *vad_ctx;

  /* pcm staging */
  f32 *pcm_window;
  u32  pcm_samples;

  /* aging */
  u32 inactivity_ticks;

  /* counters */
  u64 packets;
  u64 bytes;
  u64 late_drops;
  u64 stream_resets;

  rtp_asr_codec_t codec_kind;
} rtp_asr_session_t;

/* Per-VPP-worker state. One struct per VPP worker thread; each worker is
 * statically bound to exactly one Linux worker thread at plugin init. */
typedef struct
{
  clib_bihash_16_8_t  v4_sessions;
  clib_bihash_40_8_t  v6_sessions;
  rtp_asr_session_t  *session_pool;

  /* SPSC ring to the Linux worker assigned to this VPP worker. Slots carry
   * {session_idx, payload_offset, payload_len, rtp_ts, vpp_time}. */
  void *payload_ring;
  u32   owned_linux_worker_id;

  /* counters */
  u64 packets;
  u64 bytes;
  u64 new_sessions;
  u64 expired_sessions;
  u64 malformed_rtp;
  u64 ring_full_drops;
  u64 ring_enqueue_retries;
} rtp_asr_worker_t;

/* Global plugin main */
typedef struct
{
  u16   msg_id_base;

  /* runtime config */
  u32   sessions_per_worker;     /* default 1<<16 */
  f32   session_expiry_seconds;  /* default 30 */
  u32   pcm_window_ms;           /* default 2000 */
  u8    vad_kind;                /* 0=silero, 1=webrtc, 2=off */
  u8    emitter_sink;            /* 0=syslog, 1=json-udp, 2=grpc */
  char *model_path;
  u8    model_variant;           /* 0=base, 1=small-streaming, 2=medium-streaming */
  char *emitter_target;
  u32   linux_worker_count;

  /* per-VPP-worker state */
  rtp_asr_worker_t *per_worker;

  /* Linux worker threads are managed in asr_worker.c */
  void *linux_workers;

  /* Sherpa-ONNX recognizer handle (shared across Linux workers); pointer
   * type intentionally opaque at this header level. */
  void *recognizer;

  /* interface enable bitmap */
  u8 *enabled_by_sw_if_index;
} rtp_asr_main_t;

extern rtp_asr_main_t rtp_asr_main;

/* -----  public API between source files  ----- */

/* rtp_session.c */
int rtp_asr_session_lookup_or_create (rtp_asr_worker_t *w,
                                      const ip46_address_t *src,
                                      const ip46_address_t *dst,
                                      u16 src_port, u16 dst_port,
                                      u32 ssrc, u8 payload_type,
                                      u32 *out_session_idx);
void rtp_asr_session_aging_sweep (u32 worker_idx);

/* node_rtp_tap.c */
clib_error_t *rtp_asr_enable_disable (u32 sw_if_index, int enable_disable);

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

/* vad.c */
int  rtp_asr_vad_init_session (rtp_asr_session_t *s, u8 vad_kind);
int  rtp_asr_vad_is_speech_edge (rtp_asr_session_t *s,
                                 const f32 *pcm_16k, u32 n_samples);
void rtp_asr_vad_teardown_session (rtp_asr_session_t *s);

/* asr_worker.c */
int  rtp_asr_worker_pool_start (u32 linux_worker_count);
void rtp_asr_worker_pool_stop (void);

/* sherpa_runtime.c */
int  rtp_asr_sherpa_global_init (const char *model_path, u8 variant);
int  rtp_asr_sherpa_attach_session (rtp_asr_session_t *s);
void rtp_asr_sherpa_detach_session (rtp_asr_session_t *s);
void rtp_asr_sherpa_global_shutdown (void);

/* emitter.c */
int  rtp_asr_emitter_init (u8 sink, const char *target);
void rtp_asr_emitter_publish (const rtp_asr_session_t *s,
                              const char *text, u8 is_final);
void rtp_asr_emitter_shutdown (void);

/* rtp_asr_cli.c */
void rtp_asr_cli_init (vlib_main_t *vm);

/* rtp_asr_api.c */
clib_error_t *rtp_asr_api_init (vlib_main_t *vm);

#endif /* __included_rtp_asr_h__ */
