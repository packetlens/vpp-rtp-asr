/*
 * codec_decode.c - RTP payload → PCM decode.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runs on the Linux worker thread, NOT the VPP graph node. See FUNC_SPEC.md
 * §5 for the payload-type → codec table.
 *
 * G.711 µ-law / A-law: inline 256-entry LUT decode (no FFmpeg roundtrip).
 * Opus: libopus directly (opus_decoder_create / opus_decode_float).
 * Everything else: libavcodec (AVCodecContext per session).
 *
 * Design anchor: ~/projects/mediabridge/src/mediabridge/transcode.cpp
 */

#include <rtp_asr/rtp_asr.h>

/* TODO(v1):
 *   - rtp_asr_codec_init(): populate ulaw_to_s16_lut[256] and
 *     alaw_to_s16_lut[256] at startup.
 *   - rtp_asr_codec_setup_for_session(): dispatch on codec_kind:
 *       G711_MU / G711_A  → no alloc, codec_ctx = NULL (use LUT path directly)
 *       OPUS              → opus_decoder_create(48000, 1, &err)
 *       default           → avcodec_find_decoder() + avcodec_alloc_context3()
 *                            + avcodec_open2()
 *   - rtp_asr_codec_decode(): switch on codec_kind; produce mono PCM f32
 *     in the session's scratch buffer. Return n_samples.
 *   - rtp_asr_codec_teardown(): opus_decoder_destroy / avcodec_free_context.
 */
