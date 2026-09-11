/*
 * vad.c - Voice Activity Detection wrapper (Silero default, WebRTC fallback).
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * VAD purpose in this plugin: gate segment finalization, NOT sample admission.
 * Every decoded PCM chunk goes to sherpa_onnx_online_stream_accept_waveform()
 * regardless of VAD. VAD's speech→silence edge is what closes a transcript
 * segment and triggers emitter publish. See FUNC_SPEC.md §6.
 *
 * Default: Silero via Sherpa-ONNX built-in (SherpaOnnxVoiceActivityDetector).
 * Alt:     WebRTC VAD (smaller, BSD-style).
 */

#include <rtp_asr/rtp_asr.h>

/* TODO(v1):
 *   - rtp_asr_vad_init_session(): for silero kind,
 *     sherpa_onnx_create_voice_activity_detector() with config pointing at
 *     the bundled silero_vad.onnx.
 *   - rtp_asr_vad_is_speech_edge(): feed the chunk, return 1 on
 *     speech→silence edge (segment closes), 0 otherwise.
 *   - rtp_asr_vad_teardown_session().
 */
