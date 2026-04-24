/*
 * resample.c - libswresample wrapper: codec-native rate → 16 kHz mono f32.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Moonshine expects 16 kHz mono f32. See FUNC_SPEC.md §5.4 for the rate table.
 * SwrContext is per-session, allocated at first packet, freed at session aging.
 *
 * Design anchor: AudioDecoder::mSwr in mediabridge/transcode.cpp.
 */

#include <rtp_asr/rtp_asr.h>

/* TODO(v1):
 *   - rtp_asr_resample_to_16k():
 *       on first call per session: swr_alloc_set_opts2() with
 *         input  = (codec_clock_rate, 1 channel, AV_SAMPLE_FMT_S16 or _FLT)
 *         output = (16000, 1 channel, AV_SAMPLE_FMT_FLT)
 *         Kaiser filter
 *       swr_convert() each call.
 *   - Teardown: swr_free() on session aging.
 */
