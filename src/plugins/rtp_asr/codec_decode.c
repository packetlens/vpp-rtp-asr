/*
 * codec_decode.c - RTP payload → PCM decode.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runs on the Linux worker thread, NOT the VPP graph node.
 * See FUNC_SPEC.md §5 for the payload-type → codec table.
 *
 * G.711 µ-law / A-law: inline 256-entry LUT decode (no FFmpeg roundtrip).
 * Opus: libopus directly (opus_decoder_create / opus_decode_float).
 * Everything else: libavcodec (AVCodecContext per session).
 */

/* FFmpeg headers MUST come before VPP's clib.h: VPP redefines `always_inline`
 * as a whole-declaration macro which, when it precedes FFmpeg's
 * `av_always_inline` expansion, produces bogus `static static inline …`
 * decl syntax that GCC rejects. Keep this order. */
#include <opus/opus.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

#include <rtp_asr/rtp_asr.h>

/* ---- G.711 LUTs, populated once at plugin init ---- */
static i16 g711_ulaw_to_s16[256];
static i16 g711_alaw_to_s16[256];

static i16
ulaw_decode_one (u8 u)
{
  /* ITU-T G.711 µ-law decode — standard LUT derivation */
  u = ~u;
  int sign = (u & 0x80);
  int exponent = (u >> 4) & 0x07;
  int mantissa = u & 0x0f;
  int magnitude = ((mantissa << 3) + 0x84) << exponent;
  magnitude -= 0x84;
  return (i16) (sign ? -magnitude : magnitude);
}

static i16
alaw_decode_one (u8 a)
{
  /* ITU-T G.711 A-law decode */
  a ^= 0x55;
  int sign = (a & 0x80);
  int exponent = (a >> 4) & 0x07;
  int mantissa = a & 0x0f;
  int magnitude;
  if (exponent == 0)
    magnitude = (mantissa << 4) + 8;
  else
    magnitude = ((mantissa << 4) + 0x108) << (exponent - 1);
  return (i16) (sign ? -magnitude : magnitude);
}

static void
g711_lut_init_once (void)
{
  static int inited = 0;
  if (inited)
    return;
  for (int i = 0; i < 256; i++)
    {
      g711_ulaw_to_s16[i] = ulaw_decode_one ((u8) i);
      g711_alaw_to_s16[i] = alaw_decode_one ((u8) i);
    }
  inited = 1;
}

/* ---- PT → codec kind lookup ---- */

typedef struct
{
  rtp_asr_codec_t kind;
  u32             clock_rate;
  u8              channels;
  int             avcodec_id; /* -1 if not ffmpeg path */
} codec_info_t;

static codec_info_t
codec_info_for_pt (u8 pt)
{
  codec_info_t info = { .kind = RTP_ASR_CODEC_UNKNOWN, .clock_rate = 0,
			.channels = 1, .avcodec_id = -1 };
  switch (pt)
    {
    case 0: /* PCMU */
      info.kind = RTP_ASR_CODEC_G711_MU;
      info.clock_rate = 8000;
      break;
    case 8: /* PCMA */
      info.kind = RTP_ASR_CODEC_G711_A;
      info.clock_rate = 8000;
      break;
    case 9: /* G.722 (RTP 8kHz, audio 16kHz) */
      info.kind = RTP_ASR_CODEC_G722;
      info.clock_rate = 16000;
      info.avcodec_id = AV_CODEC_ID_ADPCM_G722;
      break;
    case 18: /* G.729 */
      info.kind = RTP_ASR_CODEC_G729;
      info.clock_rate = 8000;
      info.avcodec_id = AV_CODEC_ID_G729;
      break;
    default:
      /* Dynamic PTs — assume Opus by default (the common case). A real
       * deployment should learn this from SDP; minimum-viable assumes 111
       * -> Opus. */
      if (pt >= 96 && pt <= 127)
	{
	  info.kind = RTP_ASR_CODEC_OPUS;
	  info.clock_rate = 48000;
	  info.channels = 1;
	}
      break;
    }
  return info;
}

/* ---- Per-session codec context ---- */

typedef struct
{
  rtp_asr_codec_t kind;
  u32             clock_rate;
  u8              channels;

  OpusDecoder *opus;        /* NULL for non-Opus */
  AVCodecContext *av;       /* NULL for non-FFmpeg paths */
  AVPacket *av_pkt;
  AVFrame  *av_frame;
} codec_ctx_t;

void
rtp_asr_codec_init (void)
{
  g711_lut_init_once ();
}

int
rtp_asr_codec_setup_for_session (rtp_asr_session_t *s)
{
  if (s->codec_ctx)
    return 0; /* already set up */

  codec_info_t info = codec_info_for_pt (s->payload_type);
  if (info.kind == RTP_ASR_CODEC_UNKNOWN)
    return -1;

  codec_ctx_t *cc = clib_mem_alloc (sizeof (*cc));
  clib_memset (cc, 0, sizeof (*cc));
  cc->kind = info.kind;
  cc->clock_rate = info.clock_rate;
  cc->channels = info.channels;

  switch (info.kind)
    {
    case RTP_ASR_CODEC_G711_MU:
    case RTP_ASR_CODEC_G711_A:
      /* LUT path — nothing to allocate. */
      break;

    case RTP_ASR_CODEC_OPUS: {
      int err = 0;
      cc->opus = opus_decoder_create (48000, info.channels, &err);
      if (err != OPUS_OK || !cc->opus)
	{
	  clib_mem_free (cc);
	  return -2;
	}
      break;
    }

    default: {
      if (info.avcodec_id < 0)
	{
	  clib_mem_free (cc);
	  return -3;
	}
      const AVCodec *codec = avcodec_find_decoder ((enum AVCodecID) info.avcodec_id);
      if (!codec)
	{
	  clib_mem_free (cc);
	  return -4;
	}
      cc->av = avcodec_alloc_context3 (codec);
      if (!cc->av)
	{
	  clib_mem_free (cc);
	  return -5;
	}
      cc->av->sample_rate = info.clock_rate;
#if LIBAVCODEC_VERSION_MAJOR >= 60
      av_channel_layout_default (&cc->av->ch_layout, info.channels);
#else
      cc->av->channels = info.channels;
      cc->av->channel_layout = AV_CH_LAYOUT_MONO;
#endif
      if (avcodec_open2 (cc->av, codec, NULL) < 0)
	{
	  avcodec_free_context (&cc->av);
	  clib_mem_free (cc);
	  return -6;
	}
      cc->av_pkt = av_packet_alloc ();
      cc->av_frame = av_frame_alloc ();
      if (!cc->av_pkt || !cc->av_frame)
	{
	  if (cc->av_pkt)   av_packet_free (&cc->av_pkt);
	  if (cc->av_frame) av_frame_free (&cc->av_frame);
	  avcodec_free_context (&cc->av);
	  clib_mem_free (cc);
	  return -7;
	}
      break;
    }
    }

  s->codec_ctx = cc;
  s->clock_rate = info.clock_rate;
  return 0;
}

void
rtp_asr_codec_teardown (rtp_asr_session_t *s)
{
  if (!s->codec_ctx)
    return;
  codec_ctx_t *cc = s->codec_ctx;
  if (cc->opus)
    opus_decoder_destroy (cc->opus);
  if (cc->av_pkt)
    av_packet_free (&cc->av_pkt);
  if (cc->av_frame)
    av_frame_free (&cc->av_frame);
  if (cc->av)
    avcodec_free_context (&cc->av);
  clib_mem_free (cc);
  s->codec_ctx = NULL;
}

/* Decode one RTP payload into mono f32 PCM at the codec's native clock
 * rate. The caller passes a pcm_f32 buffer sized for the worst case and an
 * in/out n_samples. Returns 0 on success, negative on error. */
int
rtp_asr_codec_decode (rtp_asr_session_t *s,
		      const u8 *payload, u32 payload_len,
		      f32 *pcm_f32, u32 *n_samples)
{
  if (!s->codec_ctx)
    return -1;
  codec_ctx_t *cc = s->codec_ctx;

  switch (cc->kind)
    {
    case RTP_ASR_CODEC_G711_MU: {
      u32 n = payload_len;
      if (n > *n_samples) n = *n_samples;
      for (u32 i = 0; i < n; i++)
	pcm_f32[i] = (f32) g711_ulaw_to_s16[payload[i]] / 32768.0f;
      *n_samples = n;
      return 0;
    }
    case RTP_ASR_CODEC_G711_A: {
      u32 n = payload_len;
      if (n > *n_samples) n = *n_samples;
      for (u32 i = 0; i < n; i++)
	pcm_f32[i] = (f32) g711_alaw_to_s16[payload[i]] / 32768.0f;
      *n_samples = n;
      return 0;
    }
    case RTP_ASR_CODEC_OPUS: {
      int max = (int) *n_samples;
      int out = opus_decode_float (cc->opus, payload, (opus_int32) payload_len,
				   pcm_f32, max, 0);
      if (out < 0)
	return -2;
      *n_samples = (u32) out;
      return 0;
    }
    default: {
      /* FFmpeg path */
      AVPacket *pkt = cc->av_pkt;
      AVFrame  *fr  = cc->av_frame;
      pkt->data = (u8 *) payload;
      pkt->size = (int) payload_len;
      if (avcodec_send_packet (cc->av, pkt) < 0)
	return -3;
      int r = avcodec_receive_frame (cc->av, fr);
      if (r == AVERROR (EAGAIN))
	{
	  *n_samples = 0;
	  return 0;
	}
      if (r < 0)
	return -4;

      u32 out = (u32) fr->nb_samples;
      if (out > *n_samples)
	out = *n_samples;

      /* Convert whatever AV sample format to mono f32 — one-channel assumed. */
      switch (fr->format)
	{
	case AV_SAMPLE_FMT_S16: {
	  const i16 *in = (const i16 *) fr->data[0];
	  for (u32 i = 0; i < out; i++)
	    pcm_f32[i] = (f32) in[i] / 32768.0f;
	  break;
	}
	case AV_SAMPLE_FMT_FLT: {
	  clib_memcpy (pcm_f32, fr->data[0], out * sizeof (f32));
	  break;
	}
	case AV_SAMPLE_FMT_S16P: {
	  const i16 *in = (const i16 *) fr->data[0];
	  for (u32 i = 0; i < out; i++)
	    pcm_f32[i] = (f32) in[i] / 32768.0f;
	  break;
	}
	case AV_SAMPLE_FMT_FLTP: {
	  clib_memcpy (pcm_f32, fr->data[0], out * sizeof (f32));
	  break;
	}
	default:
	  av_frame_unref (fr);
	  return -5;
	}
      av_frame_unref (fr);
      *n_samples = out;
      return 0;
    }
    }
}
