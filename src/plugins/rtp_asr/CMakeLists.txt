# Copyright (c) 2026 PacketFlow (packetflow.dev)
# SPDX-License-Identifier: Apache-2.0

add_vpp_plugin(rtp_asr
  SOURCES
  rtp_asr.c
  rtp_asr_api.c
  rtp_asr_cli.c
  rtp_session.c
  node_rtp_tap.c
  codec_decode.c
  resample.c
  vad.c
  asr_worker.c
  sherpa_runtime.c
  emitter.c

  API_FILES
  rtp_asr.api

  INSTALL_HEADERS
  rtp_asr.h

  LINK_LIBRARIES
  ${SHERPA_ONNX_LIBRARIES}
  ${AV_LIBRARIES}
  ${OPUS_LIBRARIES}
)
