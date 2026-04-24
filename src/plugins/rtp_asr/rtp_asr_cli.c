/*
 * rtp_asr_cli.c - vppctl CLI command surface.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Commands per FUNC_SPEC.md §9.2:
 *
 *   rtp-asr enable   <interface>
 *   rtp-asr disable  <interface>
 *   rtp-asr set      model <path>
 *   rtp-asr set      model-variant base | small | medium
 *   rtp-asr set      vad silero | webrtc | off
 *   rtp-asr set      emitter syslog | json-udp <host:port> | grpc <endpoint>
 *   rtp-asr show     sessions
 *   rtp-asr show     stats
 *   rtp-asr show     transcripts tail [N]
 */

#include <rtp_asr/rtp_asr.h>

/* TODO(v1):
 *   - VLIB_CLI_COMMAND (rtp_asr_enable_cmd, ...): parse sw_if_index,
 *     call rtp_asr_enable_disable().
 *   - VLIB_CLI_COMMAND (rtp_asr_show_sessions_cmd, ...): walk per-worker
 *     session pools, format with clib_format style.
 *   - VLIB_CLI_COMMAND (rtp_asr_show_stats_cmd, ...): aggregate counters
 *     across workers.
 *   - VLIB_CLI_COMMAND (rtp_asr_set_cmd_*): parse token by token, update
 *     rtp_asr_main fields; trigger re-init paths where needed.
 */

void
rtp_asr_cli_init (vlib_main_t *vm)
{
  /* TODO(v1) */
  (void) vm;
}
