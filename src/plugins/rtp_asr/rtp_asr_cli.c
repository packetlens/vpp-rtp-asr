/*
 * rtp_asr_cli.c - vppctl commands for vpp-rtp-asr.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <rtp_asr/rtp_asr.h>

/* ---------- set interface rtp-asr enable|disable ---------- */

static clib_error_t *
rtp_asr_set_interface_fn (vlib_main_t *vm, unformat_input_t *input,
			  vlib_cli_command_t *cmd)
{
  vnet_main_t *vnm = vnet_get_main ();
  u32 sw_if_index = ~0;
  int enable = 1;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "disable"))
	enable = 0;
      else if (unformat (input, "enable"))
	enable = 1;
      else if (unformat (input, "%U", unformat_vnet_sw_interface, vnm,
			 &sw_if_index))
	;
      else
	return clib_error_return (0, "unknown input '%U'",
				  format_unformat_error, input);
    }

  if (sw_if_index == ~0u)
    return clib_error_return (0, "interface required");

  int rv = rtp_asr_interface_enable_disable (sw_if_index, enable);
  if (rv)
    return clib_error_return (0, "enable/disable rv=%d", rv);
  return 0;
}

VLIB_CLI_COMMAND (rtp_asr_set_interface_cmd, static) = {
  .path = "set interface rtp-asr",
  .short_help = "set interface rtp-asr <interface> [enable|disable]",
  .function = rtp_asr_set_interface_fn,
};

/* ---------- show rtp-asr version ---------- */

static clib_error_t *
show_rtp_asr_version_fn (vlib_main_t *vm, unformat_input_t *input,
			 vlib_cli_command_t *cmd)
{
  vlib_cli_output (vm, "vpp-rtp-asr plugin %s", RTP_ASR_PLUGIN_VERSION);
  return 0;
}

VLIB_CLI_COMMAND (show_rtp_asr_version_cmd, static) = {
  .path = "show rtp-asr version",
  .short_help = "show rtp-asr version",
  .function = show_rtp_asr_version_fn,
};

/* ---------- show rtp-asr stats ---------- */

static clib_error_t *
show_rtp_asr_stats_fn (vlib_main_t *vm, unformat_input_t *input,
		       vlib_cli_command_t *cmd)
{
  rtp_asr_main_t *rm = &rtp_asr_main;
  u64 packets = 0, bytes = 0;
  u64 new_sessions = 0, expired = 0, malformed = 0, not_rtp = 0, not_udp = 0;
  u32 active_sessions = 0;
  u32 workers = vec_len (rm->per_worker);

  for (u32 i = 0; i < workers; i++)
    {
      rtp_asr_worker_t *w = vec_elt_at_index (rm->per_worker, i);
      packets += w->packets;
      bytes += w->bytes;
      new_sessions += w->new_sessions;
      expired += w->expired_sessions;
      malformed += w->malformed_rtp;
      not_rtp += w->not_rtp;
      not_udp += w->not_udp;
      active_sessions += pool_elts (w->sessions);
    }

  vlib_cli_output (vm, "workers:            %u", workers);
  vlib_cli_output (vm, "active sessions:    %u", active_sessions);
  vlib_cli_output (vm, "new sessions:       %llu", new_sessions);
  vlib_cli_output (vm, "expired sessions:   %llu", expired);
  vlib_cli_output (vm, "rtp packets:        %llu", packets);
  vlib_cli_output (vm, "rtp bytes:          %llu", bytes);
  vlib_cli_output (vm, "malformed rtp:      %llu", malformed);
  vlib_cli_output (vm, "udp not rtp:        %llu", not_rtp);
  vlib_cli_output (vm, "not udp:            %llu", not_udp);
  vlib_cli_output (vm, "worker processed:   %llu",
		   rtp_asr_worker_processed_total ());
  vlib_cli_output (vm, "decode errors:      %llu",
		   rtp_asr_worker_decode_errors_total ());
  vlib_cli_output (vm, "segments emitted:   %llu",
		   rtp_asr_worker_segments_total ());
  vlib_cli_output (vm, "sherpa loaded:      %s",
		   rtp_asr_sherpa_is_loaded () ? "yes" : "no");
  return 0;
}

VLIB_CLI_COMMAND (show_rtp_asr_stats_cmd, static) = {
  .path = "show rtp-asr stats",
  .short_help = "show rtp-asr stats",
  .function = show_rtp_asr_stats_fn,
};

/* ---------- show rtp-asr sessions ---------- */

static clib_error_t *
show_rtp_asr_sessions_fn (vlib_main_t *vm, unformat_input_t *input,
			  vlib_cli_command_t *cmd)
{
  rtp_asr_main_t *rm = &rtp_asr_main;
  f64 now = vlib_time_now (vm);
  u32 workers = vec_len (rm->per_worker);

  vlib_cli_output (vm, "%-8s %-16s %-16s %-10s %-3s %-10s %-10s %s",
		   "worker", "src", "dst", "ssrc", "pt", "packets",
		   "bytes", "age");

  for (u32 i = 0; i < workers; i++)
    {
      rtp_asr_worker_t *w = vec_elt_at_index (rm->per_worker, i);
      rtp_asr_session_t *s;
      pool_foreach (s, w->sessions)
	{
	  if (s->is_ip6)
	    vlib_cli_output (
		vm, "%-8u %U:%u -> %U:%u ssrc=0x%08x pt=%u %llu/%llu %.1fs",
		i, format_ip6_address, &s->key6.src, s->src_port,
		format_ip6_address, &s->key6.dst, s->dst_port, s->ssrc,
		s->payload_type, s->packets, s->bytes, now - s->first_seen);
	  else
	    vlib_cli_output (
		vm, "%-8u %U:%u -> %U:%u ssrc=0x%08x pt=%u %llu/%llu %.1fs",
		i, format_ip4_address, &s->key4.src, s->src_port,
		format_ip4_address, &s->key4.dst, s->dst_port, s->ssrc,
		s->payload_type, s->packets, s->bytes, now - s->first_seen);
	}
    }
  return 0;
}

VLIB_CLI_COMMAND (show_rtp_asr_sessions_cmd, static) = {
  .path = "show rtp-asr sessions",
  .short_help = "show rtp-asr sessions",
  .function = show_rtp_asr_sessions_fn,
};

/* ---------- rtp-asr set model <dir>  /  rtp-asr set emitter ... ---------- */

static clib_error_t *
rtp_asr_set_model_fn (vlib_main_t *vm, unformat_input_t *input,
		      vlib_cli_command_t *cmd)
{
  u8 *path = 0;
  if (!unformat (input, "%s", &path))
    return clib_error_return (0, "usage: rtp-asr set model <dir>");
  vec_add1 (path, 0);
  int rv = rtp_asr_sherpa_global_init ((const char *) path);
  if (rv != 0)
    {
      clib_error_t *err =
	  clib_error_return (0, "sherpa load failed rv=%d for '%s'", rv, path);
      vec_free (path);
      return err;
    }
  rtp_asr_main.model_dir = (char *) path; /* ownership transferred */
  vlib_cli_output (vm, "rtp-asr: model loaded from %s", path);
  return 0;
}

VLIB_CLI_COMMAND (rtp_asr_set_model_cmd, static) = {
  .path = "rtp-asr set model",
  .short_help = "rtp-asr set model <dir>",
  .function = rtp_asr_set_model_fn,
};

static clib_error_t *
rtp_asr_set_emitter_fn (vlib_main_t *vm, unformat_input_t *input,
			vlib_cli_command_t *cmd)
{
  u8 *target = 0;
  u8 kind = 255;
  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "syslog"))
	kind = 0;
      else if (unformat (input, "json-udp %s", &target))
	kind = 1;
      else
	return clib_error_return (
	    0, "usage: rtp-asr set emitter syslog | json-udp <host:port>");
    }
  if (kind == 255)
    return clib_error_return (0, "usage: rtp-asr set emitter ...");
  if (target)
    vec_add1 (target, 0);
  int rv = rtp_asr_emitter_init (kind, target ? (const char *) target : NULL);
  if (rv != 0)
    {
      clib_error_t *err =
	  clib_error_return (0, "emitter init failed rv=%d", rv);
      if (target)
	vec_free (target);
      return err;
    }
  rtp_asr_main.emitter_sink = kind;
  if (rtp_asr_main.emitter_target)
    free (rtp_asr_main.emitter_target);
  rtp_asr_main.emitter_target = target ? (char *) target : NULL;
  vlib_cli_output (vm, "rtp-asr: emitter set");
  return 0;
}

VLIB_CLI_COMMAND (rtp_asr_set_emitter_cmd, static) = {
  .path = "rtp-asr set emitter",
  .short_help = "rtp-asr set emitter syslog | json-udp <host:port>",
  .function = rtp_asr_set_emitter_fn,
};
