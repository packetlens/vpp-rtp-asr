/*
 * emitter.c - Transcript emitter: syslog + JSON-UDP.
 *
 * Copyright (c) 2026 PacketFlow (packetflow.dev)
 * SPDX-License-Identifier: Apache-2.0
 *
 * See FUNC_SPEC.md §8 for record schema. Runs on the Linux worker thread.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <syslog.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>

#include <rtp_asr/rtp_asr.h>

typedef struct
{
  u8  sink;              /* 0=syslog, 1=json-udp */
  int udp_fd;
  struct sockaddr_in udp_addr;
  pthread_mutex_t mu;
  int initialized;
} emitter_state_t;

static emitter_state_t g_emit;

static int
parse_host_port (const char *s, struct sockaddr_in *out)
{
  if (!s || !*s)
    return -1;
  char host[128] = { 0 };
  int  port = 0;
  const char *colon = strrchr (s, ':');
  if (!colon)
    return -1;
  size_t hl = (size_t) (colon - s);
  if (hl >= sizeof host)
    return -1;
  memcpy (host, s, hl);
  host[hl] = '\0';
  port = atoi (colon + 1);
  if (port <= 0 || port > 65535)
    return -1;

  memset (out, 0, sizeof (*out));
  out->sin_family = AF_INET;
  out->sin_port = htons ((u_int16_t) port);
  if (inet_pton (AF_INET, host, &out->sin_addr) != 1)
    return -1;
  return 0;
}

int
rtp_asr_emitter_init (u8 sink, const char *target)
{
  pthread_mutex_init (&g_emit.mu, NULL);
  g_emit.sink = sink;
  g_emit.udp_fd = -1;

  if (sink == 0)
    {
      openlog ("vpp-rtp-asr", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    }
  else if (sink == 1)
    {
      if (parse_host_port (target ? target : "127.0.0.1:7879",
			   &g_emit.udp_addr) != 0)
	return -1;
      g_emit.udp_fd = socket (AF_INET, SOCK_DGRAM, 0);
      if (g_emit.udp_fd < 0)
	return -2;
    }
  g_emit.initialized = 1;
  return 0;
}

static void
iso8601_now (char *buf, size_t cap)
{
  struct timespec ts;
  clock_gettime (CLOCK_REALTIME, &ts);
  struct tm tm;
  gmtime_r (&ts.tv_sec, &tm);
  int ms = (int) (ts.tv_nsec / 1000000);
  snprintf (buf, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
	    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	    tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

static void
format_ip (const rtp_asr_session_t *s, char *buf, size_t cap, int is_src)
{
  u32 v4 = 0;
  if (!s->is_ip6)
    {
      const u8 *p = is_src ? (const u8 *) &s->key4.src
			   : (const u8 *) &s->key4.dst;
      snprintf (buf, cap, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
      (void) v4;
    }
  else
    {
      snprintf (buf, cap, "%s", is_src ? "::src-v6::" : "::dst-v6::");
    }
}

static const char *
codec_name_for_pt (u8 pt)
{
  switch (pt)
    {
    case 0:  return "PCMU";
    case 8:  return "PCMA";
    case 9:  return "G722";
    case 18: return "G729";
    case 111: return "OPUS";
    default:
      if (pt >= 96 && pt <= 127) return "DYN";
      return "UNK";
    }
}

static void
escape_json_string (const char *in, char *out, size_t cap)
{
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 2 < cap; i++)
    {
      unsigned char c = (unsigned char) in[i];
      if (c == '"' || c == '\\')
	{
	  if (o + 2 >= cap) break;
	  out[o++] = '\\';
	  out[o++] = (char) c;
	}
      else if (c == '\n')
	{
	  if (o + 2 >= cap) break;
	  out[o++] = '\\';
	  out[o++] = 'n';
	}
      else if (c == '\r')
	{
	  if (o + 2 >= cap) break;
	  out[o++] = '\\';
	  out[o++] = 'r';
	}
      else if (c < 0x20)
	{
	  if (o + 6 >= cap) break;
	  o += snprintf (out + o, cap - o, "\\u%04x", c);
	}
      else
	{
	  out[o++] = (char) c;
	}
    }
  out[o] = '\0';
}

void
rtp_asr_emitter_publish (const rtp_asr_session_t *s,
			 const char *text, u8 is_final,
			 u32 rtp_ts_first, u32 rtp_ts_last)
{
  if (!g_emit.initialized || !text || !*text)
    return;

  char ts_wall[64];
  iso8601_now (ts_wall, sizeof ts_wall);

  char src[64], dst[64];
  format_ip (s, src, sizeof src, 1);
  format_ip (s, dst, sizeof dst, 0);

  char text_esc[1024];
  escape_json_string (text, text_esc, sizeof text_esc);

  char line[2048];
  int n = snprintf (line, sizeof line,
		    "{\"ts_wall\":\"%s\",\"ts_rtp_first\":%u,"
		    "\"ts_rtp_last\":%u,\"ssrc\":\"0x%08x\","
		    "\"src\":\"%s:%u\",\"dst\":\"%s:%u\",\"pt\":%u,"
		    "\"codec\":\"%s\",\"text\":\"%s\",\"is_final\":%s}",
		    ts_wall, rtp_ts_first, rtp_ts_last, s->ssrc,
		    src, s->src_port, dst, s->dst_port, s->payload_type,
		    codec_name_for_pt (s->payload_type), text_esc,
		    is_final ? "true" : "false");
  if (n <= 0)
    return;
  if ((size_t) n >= sizeof line)
    n = sizeof line - 1;

  pthread_mutex_lock (&g_emit.mu);
  if (g_emit.sink == 0)
    {
      syslog (LOG_INFO, "%s", line);
    }
  else if (g_emit.sink == 1 && g_emit.udp_fd >= 0)
    {
      (void) sendto (g_emit.udp_fd, line, (size_t) n, 0,
		     (struct sockaddr *) &g_emit.udp_addr,
		     sizeof g_emit.udp_addr);
    }
  pthread_mutex_unlock (&g_emit.mu);
}

void
rtp_asr_emitter_shutdown (void)
{
  if (!g_emit.initialized)
    return;
  if (g_emit.sink == 0)
    closelog ();
  if (g_emit.udp_fd >= 0)
    close (g_emit.udp_fd);
  g_emit.udp_fd = -1;
  pthread_mutex_destroy (&g_emit.mu);
  g_emit.initialized = 0;
}
