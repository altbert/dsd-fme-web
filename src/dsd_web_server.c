/*-------------------------------------------------------------------------------
 * dsd_web_server.c
 * Embedded HTTP / WebSocket server for the DSD-FME web interface
 *
 * One thread, poll() based, non blocking sockets. Messages to clients are
 * pre-framed and reference counted so a broadcast is a single allocation.
 * Each client queue applies a drop policy per message kind so a slow browser
 * can never stall the decoder (FFT, audio and console frames get dropped,
 * control messages do not).
 *-----------------------------------------------------------------------------*/

#include "dsd_web.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#define WEB_MAX_CLIENTS   32
#define WEB_MAX_HEADER    16384
#define WEB_MAX_MESSAGE   (8 * 1024 * 1024)
#define WEB_QUEUE_LEN     1024
#define WEB_CMD_QUEUE_LEN 1024
#define WEB_MAX_AREAS     8

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct web_msg
{
  int refs;
  int kind;
  size_t len;
  uint8_t data[];
} web_msg;

typedef struct
{
  web_msg * m;
  size_t off;
} web_qitem;

typedef struct
{
  int used;
  int fd;
  int id;
  int is_ws;
  int authed;
  int close_after;
  uint32_t subs;
  char ip[64];
  uint8_t * in;
  size_t in_len;
  size_t in_cap;
  uint8_t * frag;
  size_t frag_len;
  int frag_op;
  web_qitem q[WEB_QUEUE_LEN];
  int qh;
  int qn;
  size_t q_bytes;
  int pend[5];          //queued messages per kind
  size_t pend_bytes[5]; //queued bytes per kind
  FILE * dl;            //file being streamed (download)
  long long dl_left;
} web_client;

typedef struct
{
  int client;
  char * json;
} web_cmd;

static struct
{
  int running;
  int stop;
  int listen_fd;
  int wake_r;
  int wake_w;
  pthread_t thread;
  pthread_mutex_t mtx;
  web_client clients[WEB_MAX_CLIENTS];
  int next_id;
  char root[1024];
  char data[1024];
  char password[256];
  char token[33];
  char areas[WEB_MAX_AREAS][2][1024];
  pthread_mutex_t cmd_mtx;
  web_cmd cmds[WEB_CMD_QUEUE_LEN];
  int cmd_head;
  int cmd_count;
} g;

//----------------------------------------------------------------------------
// SHA-1 and base64 (websocket handshake)
//----------------------------------------------------------------------------

typedef struct
{
  uint32_t h[5];
  uint64_t len;
  uint8_t buf[64];
  size_t blen;
} sha1_ctx;

static uint32_t rol32 (uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block (sha1_ctx * c, const uint8_t * p)
{
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
  for (int i = 16; i < 80; i++)
    w[i] = rol32 (w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
  uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
  for (int i = 0; i < 80; i++)
  {
    uint32_t f, k;
    if (i < 20)      { f = (b & cc) | (~b & d);            k = 0x5A827999; }
    else if (i < 40) { f = b ^ cc ^ d;                     k = 0x6ED9EBA1; }
    else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);  k = 0x8F1BBCDC; }
    else             { f = b ^ cc ^ d;                     k = 0xCA62C1D6; }
    uint32_t t = rol32 (a, 5) + f + e + k + w[i];
    e = d; d = cc; cc = rol32 (b, 30); b = a; a = t;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1 (const uint8_t * data, size_t len, uint8_t out[20])
{
  sha1_ctx c;
  c.h[0] = 0x67452301; c.h[1] = 0xEFCDAB89; c.h[2] = 0x98BADCFE; c.h[3] = 0x10325476; c.h[4] = 0xC3D2E1F0;
  c.len = 0; c.blen = 0;
  for (size_t i = 0; i < len; i++)
  {
    c.buf[c.blen++] = data[i];
    if (c.blen == 64) { sha1_block (&c, c.buf); c.blen = 0; }
  }
  c.len = (uint64_t)len * 8;
  c.buf[c.blen++] = 0x80;
  if (c.blen > 56)
  {
    while (c.blen < 64) c.buf[c.blen++] = 0;
    sha1_block (&c, c.buf);
    c.blen = 0;
  }
  while (c.blen < 56) c.buf[c.blen++] = 0;
  for (int i = 7; i >= 0; i--) c.buf[c.blen++] = (uint8_t)(c.len >> (i * 8));
  sha1_block (&c, c.buf);
  for (int i = 0; i < 5; i++)
  {
    out[i*4+0] = (uint8_t)(c.h[i] >> 24);
    out[i*4+1] = (uint8_t)(c.h[i] >> 16);
    out[i*4+2] = (uint8_t)(c.h[i] >> 8);
    out[i*4+3] = (uint8_t)(c.h[i]);
  }
}

static void base64 (const uint8_t * in, size_t len, char * out)
{
  static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < len; i += 3)
  {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < len) v |= (uint32_t)in[i+1] << 8;
    if (i + 2 < len) v |= in[i+2];
    out[o++] = t[(v >> 18) & 63];
    out[o++] = t[(v >> 12) & 63];
    out[o++] = (i + 1 < len) ? t[(v >> 6) & 63] : '=';
    out[o++] = (i + 2 < len) ? t[v & 63] : '=';
  }
  out[o] = 0;
}

//----------------------------------------------------------------------------
// Messages and queues
//----------------------------------------------------------------------------

static web_msg * msg_new (int kind, size_t len)
{
  web_msg * m = malloc (sizeof(web_msg) + len);
  if (!m) return NULL;
  m->refs = 0;
  m->kind = kind;
  m->len = len;
  return m;
}

static void msg_unref (web_msg * m)
{
  if (m && --m->refs <= 0) free (m);
}

//build a server to client websocket frame (never masked)
static web_msg * ws_frame (int kind, int opcode, const void * data, size_t len)
{
  size_t hl = 2;
  if (len >= 126 && len <= 0xFFFF) hl = 4;
  else if (len > 0xFFFF) hl = 10;
  web_msg * m = msg_new (kind, hl + len);
  if (!m) return NULL;
  uint8_t * p = m->data;
  p[0] = (uint8_t)(0x80 | (opcode & 0x0F));
  if (hl == 2) p[1] = (uint8_t)len;
  else if (hl == 4)
  {
    p[1] = 126;
    p[2] = (uint8_t)(len >> 8);
    p[3] = (uint8_t)len;
  }
  else
  {
    p[1] = 127;
    for (int i = 0; i < 8; i++) p[2+i] = (uint8_t)((uint64_t)len >> (56 - 8*i));
  }
  if (len) memcpy (p + hl, data, len);
  return m;
}

static void wake (void)
{
  char c = 1;
  if (g.wake_w > 0)
  {
    ssize_t r = write (g.wake_w, &c, 1);
    UNUSED(r);
  }
}

//must hold g.mtx; returns 0 if the message was dropped
static int client_enqueue (web_client * c, web_msg * m)
{
  int k = m->kind;
  if (c->close_after) return 0;
  //drop policy per kind
  if (k == WEB_KIND_FFT && c->pend[k] >= 2) return 0;
  if (k == WEB_KIND_STATUS && c->pend[k] >= 2) return 0;
  if (k == WEB_KIND_AUDIO && c->pend_bytes[k] > 96000) return 0;
  if (k == WEB_KIND_CONSOLE && c->pend_bytes[k] > 2 * 1024 * 1024) return 0;
  if (c->qn >= WEB_QUEUE_LEN || c->q_bytes > 64 * 1024 * 1024)
  {
    //control messages can not be dropped, give up on this client
    if (k == WEB_KIND_CTRL) c->close_after = 2;
    return 0;
  }
  int idx = (c->qh + c->qn) % WEB_QUEUE_LEN;
  c->q[idx].m = m;
  c->q[idx].off = 0;
  c->qn++;
  c->q_bytes += m->len;
  c->pend[k]++;
  c->pend_bytes[k] += m->len;
  m->refs++;
  return 1;
}

static void client_pop (web_client * c)
{
  web_qitem * it = &c->q[c->qh];
  int k = it->m->kind;
  c->q_bytes -= it->m->len;
  c->pend[k]--;
  c->pend_bytes[k] -= it->m->len;
  msg_unref (it->m);
  it->m = NULL;
  c->qh = (c->qh + 1) % WEB_QUEUE_LEN;
  c->qn--;
}

static web_client * client_by_id (int id)
{
  for (int i = 0; i < WEB_MAX_CLIENTS; i++)
    if (g.clients[i].used && g.clients[i].id == id) return &g.clients[i];
  return NULL;
}

static void push_command (int client, const char * json, size_t len)
{
  pthread_mutex_lock (&g.cmd_mtx);
  if (g.cmd_count < WEB_CMD_QUEUE_LEN)
  {
    char * s = malloc (len + 1);
    if (s)
    {
      memcpy (s, json, len);
      s[len] = 0;
      int idx = (g.cmd_head + g.cmd_count) % WEB_CMD_QUEUE_LEN;
      g.cmds[idx].client = client;
      g.cmds[idx].json = s;
      g.cmd_count++;
    }
  }
  pthread_mutex_unlock (&g.cmd_mtx);
}

int web_pop_command (int * client_id, char ** json)
{
  int ok = 0;
  if (!g.running) return 0;
  pthread_mutex_lock (&g.cmd_mtx);
  if (g.cmd_count > 0)
  {
    *client_id = g.cmds[g.cmd_head].client;
    *json = g.cmds[g.cmd_head].json;
    g.cmds[g.cmd_head].json = NULL;
    g.cmd_head = (g.cmd_head + 1) % WEB_CMD_QUEUE_LEN;
    g.cmd_count--;
    ok = 1;
  }
  pthread_mutex_unlock (&g.cmd_mtx);
  return ok;
}

void web_broadcast (int kind, int opcode, const void * data, size_t len, uint32_t need_sub)
{
  if (!g.running) return;
  web_msg * m = NULL;
  int queued = 0;
  pthread_mutex_lock (&g.mtx);
  for (int i = 0; i < WEB_MAX_CLIENTS; i++)
  {
    web_client * c = &g.clients[i];
    if (!c->used || !c->is_ws || !c->authed) continue;
    if (need_sub && !(c->subs & need_sub)) continue;
    if (!m)
    {
      m = ws_frame (kind, opcode, data, len);
      if (!m) break;
    }
    queued |= client_enqueue (c, m);
  }
  if (m && m->refs == 0) free (m);
  pthread_mutex_unlock (&g.mtx);
  if (queued) wake ();
}

void web_send (int client_id, int kind, int opcode, const void * data, size_t len)
{
  if (!g.running) return;
  int queued = 0;
  pthread_mutex_lock (&g.mtx);
  web_client * c = client_by_id (client_id);
  if (c && c->is_ws)
  {
    web_msg * m = ws_frame (kind, opcode, data, len);
    if (m)
    {
      queued = client_enqueue (c, m);
      if (m->refs == 0) free (m);
    }
  }
  pthread_mutex_unlock (&g.mtx);
  if (queued) wake ();
}

int web_server_clients (void)
{
  int n = 0;
  if (!g.running) return 0;
  pthread_mutex_lock (&g.mtx);
  for (int i = 0; i < WEB_MAX_CLIENTS; i++)
    if (g.clients[i].used && g.clients[i].is_ws && g.clients[i].authed) n++;
  pthread_mutex_unlock (&g.mtx);
  return n;
}

uint32_t web_server_subs (void)
{
  uint32_t s = 0;
  if (!g.running) return 0;
  pthread_mutex_lock (&g.mtx);
  for (int i = 0; i < WEB_MAX_CLIENTS; i++)
    if (g.clients[i].used && g.clients[i].is_ws && g.clients[i].authed) s |= g.clients[i].subs;
  pthread_mutex_unlock (&g.mtx);
  return s;
}

void web_set_subs (int client_id, uint32_t subs)
{
  pthread_mutex_lock (&g.mtx);
  web_client * c = client_by_id (client_id);
  if (c) c->subs = subs;
  pthread_mutex_unlock (&g.mtx);
}

int web_client_authed (int client_id)
{
  int a = 0;
  pthread_mutex_lock (&g.mtx);
  web_client * c = client_by_id (client_id);
  if (c) a = c->authed;
  pthread_mutex_unlock (&g.mtx);
  return a;
}

void web_set_authed (int client_id, int authed)
{
  pthread_mutex_lock (&g.mtx);
  web_client * c = client_by_id (client_id);
  if (c) c->authed = authed;
  pthread_mutex_unlock (&g.mtx);
}

const char * web_session_token (void)
{
  return g.token;
}

void web_server_set_area (const char * name, const char * path)
{
  pthread_mutex_lock (&g.mtx);
  int slot = -1;
  for (int i = 0; i < WEB_MAX_AREAS; i++)
  {
    if (g.areas[i][0][0] && strcmp (g.areas[i][0], name) == 0) { slot = i; break; }
    if (slot < 0 && g.areas[i][0][0] == 0) slot = i;
  }
  if (slot >= 0)
  {
    snprintf (g.areas[slot][0], sizeof(g.areas[slot][0]), "%s", name);
    snprintf (g.areas[slot][1], sizeof(g.areas[slot][1]), "%s", path);
  }
  pthread_mutex_unlock (&g.mtx);
}

//only allow simple file names: letters, digits, '.', '_', '-', ' ', '+', ','
//no leading dot and no path separators, so nothing can escape its directory
int web_path_component_ok (const char * s)
{
  if (!s || !s[0] || s[0] == '.') return 0;
  size_t n = strlen (s);
  if (n > 200) return 0;
  for (size_t i = 0; i < n; i++)
  {
    unsigned char c = (unsigned char)s[i];
    if (isalnum (c) || c == '.' || c == '_' || c == '-' || c == ' ' || c == '+' || c == ',') continue;
    return 0;
  }
  return 1;
}

//----------------------------------------------------------------------------
// HTTP
//----------------------------------------------------------------------------

static const char * mime_for (const char * path)
{
  const char * ext = strrchr (path, '.');
  if (!ext) return "application/octet-stream";
  if (strcasecmp (ext, ".html") == 0) return "text/html; charset=utf-8";
  if (strcasecmp (ext, ".js") == 0) return "text/javascript; charset=utf-8";
  if (strcasecmp (ext, ".css") == 0) return "text/css; charset=utf-8";
  if (strcasecmp (ext, ".json") == 0) return "application/json";
  if (strcasecmp (ext, ".svg") == 0) return "image/svg+xml";
  if (strcasecmp (ext, ".png") == 0) return "image/png";
  if (strcasecmp (ext, ".ico") == 0) return "image/x-icon";
  if (strcasecmp (ext, ".wav") == 0) return "audio/wav";
  if (strcasecmp (ext, ".csv") == 0) return "text/csv";
  if (strcasecmp (ext, ".txt") == 0 || strcasecmp (ext, ".log") == 0) return "text/plain; charset=utf-8";
  if (strcasecmp (ext, ".webmanifest") == 0) return "application/manifest+json";
  return "application/octet-stream";
}

static void http_send (web_client * c, int code, const char * status, const char * ctype, const void * body, size_t blen, const char * extra)
{
  char hdr[1024];
  int hl = snprintf (hdr, sizeof(hdr),
    "HTTP/1.1 %d %s\r\n"
    "Content-Type: %s\r\n"
    "Content-Length: %zu\r\n"
    "Connection: close\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "%s"
    "\r\n", code, status, ctype, blen, extra ? extra : "");
  if (hl < 0) return;
  web_msg * m = msg_new (WEB_KIND_CTRL, (size_t)hl + blen);
  if (!m) return;
  memcpy (m->data, hdr, (size_t)hl);
  if (blen) memcpy (m->data + hl, body, blen);
  pthread_mutex_lock (&g.mtx);
  client_enqueue (c, m);
  if (m->refs == 0) free (m);
  c->close_after = 1;
  pthread_mutex_unlock (&g.mtx);
}

static void http_error (web_client * c, int code, const char * status)
{
  char body[256];
  snprintf (body, sizeof(body), "%d %s\n", code, status);
  http_send (c, code, status, "text/plain; charset=utf-8", body, strlen (body), NULL);
}

static const char * header_value (const char * req, const char * name, char * out, size_t outlen)
{
  size_t nl = strlen (name);
  const char * p = strstr (req, "\r\n");
  while (p && p[2] && !(p[2] == '\r' && p[3] == '\n'))
  {
    p += 2;
    if (strncasecmp (p, name, nl) == 0 && p[nl] == ':')
    {
      const char * v = p + nl + 1;
      while (*v == ' ' || *v == '\t') v++;
      const char * e = strstr (v, "\r\n");
      if (!e) e = v + strlen (v);
      size_t n = (size_t)(e - v);
      if (n >= outlen) n = outlen - 1;
      memcpy (out, v, n);
      out[n] = 0;
      return out;
    }
    p = strstr (p, "\r\n");
  }
  return NULL;
}

static int url_decode (const char * in, char * out, size_t outlen)
{
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < outlen; i++)
  {
    if (in[i] == '%' && isxdigit ((unsigned char)in[i+1]) && isxdigit ((unsigned char)in[i+2]))
    {
      char hx[3] = { in[i+1], in[i+2], 0 };
      int v = (int)strtol (hx, NULL, 16);
      if (v == 0) return 0;
      out[o++] = (char)v;
      i += 2;
    }
    else if (in[i] == '+') out[o++] = ' ';
    else out[o++] = in[i];
  }
  out[o] = 0;
  return 1;
}

static int read_file (const char * path, uint8_t ** data, size_t * len)
{
  FILE * f = fopen (path, "rb");
  if (!f) return 0;
  struct stat st;
  if (fstat (fileno (f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 32 * 1024 * 1024)
  {
    fclose (f);
    return 0;
  }
  *len = (size_t)st.st_size;
  *data = malloc (*len + 1);
  if (!*data) { fclose (f); return 0; }
  if (*len && fread (*data, 1, *len, f) != *len)
  {
    free (*data);
    fclose (f);
    return 0;
  }
  fclose (f);
  return 1;
}

//true when the browser Origin header points at the same host we were reached on
static int origin_ok (const char * req)
{
  char origin[512], host[512];
  if (!header_value (req, "Origin", origin, sizeof(origin))) return 1; //non browser client
  if (!header_value (req, "Host", host, sizeof(host))) return 0;
  const char * o = strstr (origin, "://");
  if (!o) return 0;
  o += 3;
  size_t ol = strcspn (o, "/");
  return (ol == strlen (host) && strncasecmp (o, host, ol) == 0);
}

//DNS rebinding guard: a hostile page can point a name it owns at 127.0.0.1 (or at this host)
//and the browser then treats this server as part of that page's site, Origin and Host agree.
//Without a password only answer requests addressed to an IP literal, localhost or this
//machine's own name (with or without .local); with one, nothing works before the login anyway
static int host_ok (const char * req)
{
  char host[512], self[256];
  if (g.password[0]) return 1;
  if (!header_value (req, "Host", host, sizeof(host))) return 1; //not a browser
  char * h = host;
  if (h[0] == '[') //IPv6 literal, [addr]:port
  {
    char * rb = strchr (h, ']');
    if (!rb) return 0;
    *rb = 0;
    struct in6_addr a6;
    return inet_pton (AF_INET6, h + 1, &a6) == 1;
  }
  char * colon = strchr (h, ':');
  if (colon) *colon = 0;
  struct in_addr a4;
  if (inet_pton (AF_INET, h, &a4) == 1) return 1;
  size_t n = strlen (h);
  if (n && h[n-1] == '.') h[--n] = 0; //fully qualified form
  if (!strcasecmp (h, "localhost") || (n > 10 && !strcasecmp (h + n - 10, ".localhost"))) return 1;
  if (gethostname (self, sizeof(self)) != 0) return 0;
  self[sizeof(self)-1] = 0;
  size_t sl = strlen (self);
  if (!sl) return 0;
  if (!strcasecmp (h, self)) return 1;
  return n == sl + 6 && !strncasecmp (h, self, sl) && !strcasecmp (h + sl, ".local");
}

static void serve_download (web_client * c, const char * path, const char * query, const char * req)
{
  //path is "/dl/<area>/<file>"
  char area[64], file[512], decoded[512], full[2048];
  const char * p = path + 4;
  const char * slash = strchr (p, '/');
  if (!slash || (size_t)(slash - p) >= sizeof(area)) { http_error (c, 404, "Not Found"); return; }
  memcpy (area, p, (size_t)(slash - p));
  area[slash - p] = 0;
  snprintf (file, sizeof(file), "%s", slash + 1);
  if (!url_decode (file, decoded, sizeof(decoded)) || !web_path_component_ok (decoded))
  {
    http_error (c, 400, "Bad Request");
    return;
  }

  if (g.password[0])
  {
    const char * t = query ? strstr (query, "t=") : NULL;
    if (!t || strncmp (t + 2, g.token, 32) != 0)
    {
      http_error (c, 403, "Forbidden");
      return;
    }
  }

  char dir[1024] = {0};
  pthread_mutex_lock (&g.mtx);
  for (int i = 0; i < WEB_MAX_AREAS; i++)
    if (g.areas[i][0][0] && strcmp (g.areas[i][0], area) == 0)
      snprintf (dir, sizeof(dir), "%s", g.areas[i][1]);
  pthread_mutex_unlock (&g.mtx);
  if (!dir[0]) { http_error (c, 404, "Not Found"); return; }

  snprintf (full, sizeof(full), "%s/%s", dir, decoded);
  FILE * f = fopen (full, "rb");
  struct stat st;
  if (!f || fstat (fileno (f), &st) != 0 || !S_ISREG(st.st_mode))
  {
    if (f) fclose (f);
    http_error (c, 404, "Not Found");
    return;
  }

  long long size = (long long)st.st_size, start = 0, end = size - 1;
  int partial = 0;
  char range[128];
  if (header_value (req, "Range", range, sizeof(range)) && strncmp (range, "bytes=", 6) == 0)
  {
    char * dash = strchr (range + 6, '-');
    if (dash)
    {
      *dash = 0;
      if (range[6]) start = atoll (range + 6);
      if (dash[1]) end = atoll (dash + 1);
      if (!range[6] && dash[1]) { start = size - atoll (dash + 1); end = size - 1; }
      if (start < 0) start = 0;
      if (end >= size) end = size - 1;
      if (start <= end) partial = 1;
      else start = 0, end = size - 1;
    }
  }
  if (size == 0) { start = 0; end = -1; partial = 0; }
  fseek (f, (long)start, SEEK_SET);

  char hdr[1400];
  int dl = (query && strstr (query, "dl=1"));
  int hl;
  if (partial)
    hl = snprintf (hdr, sizeof(hdr),
      "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
      "Content-Range: bytes %lld-%lld/%lld\r\nAccept-Ranges: bytes\r\nConnection: close\r\n"
      "%s%s%s\r\n",
      mime_for (decoded), end - start + 1, start, end, size,
      dl ? "Content-Disposition: attachment; filename=\"" : "", dl ? decoded : "", dl ? "\"\r\n" : "");
  else
    hl = snprintf (hdr, sizeof(hdr),
      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\nAccept-Ranges: bytes\r\nConnection: close\r\n"
      "%s%s%s\r\n",
      mime_for (decoded), size,
      dl ? "Content-Disposition: attachment; filename=\"" : "", dl ? decoded : "", dl ? "\"\r\n" : "");
  web_msg * m = msg_new (WEB_KIND_CTRL, (size_t)hl);
  if (!m) { fclose (f); return; }
  memcpy (m->data, hdr, (size_t)hl);
  pthread_mutex_lock (&g.mtx);
  client_enqueue (c, m);
  if (m->refs == 0) free (m);
  c->dl = f;
  c->dl_left = end - start + 1;
  pthread_mutex_unlock (&g.mtx);
}

static void ws_accept (web_client * c, const char * req)
{
  char key[256];
  if (!header_value (req, "Sec-WebSocket-Key", key, sizeof(key)))
  {
    http_error (c, 400, "Bad Request");
    return;
  }
  if (!origin_ok (req))
  {
    http_error (c, 403, "Forbidden");
    return;
  }
  char cat[512];
  snprintf (cat, sizeof(cat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
  uint8_t dig[20];
  sha1 ((const uint8_t *)cat, strlen (cat), dig);
  char acc[64];
  base64 (dig, 20, acc);
  char resp[512];
  int n = snprintf (resp, sizeof(resp),
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: %s\r\n\r\n", acc);
  web_msg * m = msg_new (WEB_KIND_CTRL, (size_t)n);
  if (!m) return;
  memcpy (m->data, resp, (size_t)n);
  pthread_mutex_lock (&g.mtx);
  c->is_ws = 1;
  c->authed = g.password[0] ? 0 : 1;
  c->subs = WEB_SUB_FFT | WEB_SUB_CONSOLE;
  client_enqueue (c, m);
  if (m->refs == 0) free (m);
  pthread_mutex_unlock (&g.mtx);
  char cmd[64];
  int cl = snprintf (cmd, sizeof(cmd), "{\"cmd\":\"_connect\"}");
  push_command (c->id, cmd, (size_t)cl);
}

static void handle_http (web_client * c, char * req)
{
  char method[16], target[2048];
  if (sscanf (req, "%15s %2047s", method, target) != 2)
  {
    http_error (c, 400, "Bad Request");
    return;
  }
  if (strcmp (method, "GET") != 0 && strcmp (method, "HEAD") != 0)
  {
    http_error (c, 405, "Method Not Allowed");
    return;
  }
  if (!host_ok (req))
  {
    const char * msg = "403 Forbidden\nUnknown host name. Open the web interface by IP address or localhost, "
                       "or start dsd-fme with --web-pass to allow any name.\n";
    http_send (c, 403, "Forbidden", "text/plain; charset=utf-8", msg, strlen (msg), NULL);
    return;
  }

  char * query = strchr (target, '?');
  if (query) *query++ = 0;

  char upgrade[64];
  if (strcmp (target, "/ws") == 0)
  {
    if (header_value (req, "Upgrade", upgrade, sizeof(upgrade)) && strcasecmp (upgrade, "websocket") == 0)
      ws_accept (c, req);
    else http_error (c, 400, "Bad Request");
    return;
  }

  if (strncmp (target, "/dl/", 4) == 0)
  {
    serve_download (c, target, query, req);
    return;
  }

  char path[2048];
  if (!url_decode (target, path, sizeof(path)) || strstr (path, "..") || path[0] != '/')
  {
    http_error (c, 400, "Bad Request");
    return;
  }
  if (strcmp (path, "/") == 0) snprintf (path, sizeof(path), "/index.html");

  const char * extra = "Cache-Control: no-cache\r\n";

  //development mode: serve from a directory on disk
  if (g.root[0])
  {
    char full[3072];
    uint8_t * data = NULL;
    size_t len = 0;
    snprintf (full, sizeof(full), "%s%s", g.root, path);
    if (read_file (full, &data, &len))
    {
      http_send (c, 200, "OK", mime_for (path), data, len, extra);
      free (data);
      return;
    }
  }

  for (unsigned int i = 0; i < web_assets_count; i++)
  {
    if (strcmp (web_assets[i].path, path) == 0)
    {
      http_send (c, 200, "OK", web_assets[i].mime, web_assets[i].data, web_assets[i].len, extra);
      return;
    }
  }
  http_error (c, 404, "Not Found");
}

//----------------------------------------------------------------------------
// WebSocket input
//----------------------------------------------------------------------------

static void ws_send_raw (web_client * c, int opcode, const void * data, size_t len)
{
  web_msg * m = ws_frame (WEB_KIND_CTRL, opcode, data, len);
  if (!m) return;
  pthread_mutex_lock (&g.mtx);
  client_enqueue (c, m);
  if (m->refs == 0) free (m);
  pthread_mutex_unlock (&g.mtx);
}

static void ws_message (web_client * c, int opcode, const uint8_t * data, size_t len)
{
  if (opcode == WS_OP_TEXT)
    push_command (c->id, (const char *)data, len);
  //binary messages from clients are not used
}

//returns -1 on protocol error, otherwise number of bytes consumed (0 = need more)
static long ws_parse (web_client * c, const uint8_t * p, size_t n)
{
  if (n < 2) return 0;
  int fin = (p[0] & 0x80) != 0;
  int op = p[0] & 0x0F;
  int masked = (p[1] & 0x80) != 0;
  uint64_t len = p[1] & 0x7F;
  size_t pos = 2;
  if (!masked) return -1; //clients must mask
  if (len == 126)
  {
    if (n < 4) return 0;
    len = ((uint64_t)p[2] << 8) | p[3];
    pos = 4;
  }
  else if (len == 127)
  {
    if (n < 10) return 0;
    len = 0;
    for (int i = 0; i < 8; i++) len = (len << 8) | p[2+i];
    pos = 10;
  }
  if (len > WEB_MAX_MESSAGE) return -1;
  if (n < pos + 4 + len) return 0;
  const uint8_t * mask = p + pos;
  pos += 4;
  uint8_t * payload = malloc ((size_t)len + 1);
  if (!payload) return -1;
  for (uint64_t i = 0; i < len; i++) payload[i] = p[pos + i] ^ mask[i & 3];
  payload[len] = 0;

  if (op == 0x8) //close
  {
    ws_send_raw (c, 0x8, payload, len >= 2 ? 2 : 0);
    pthread_mutex_lock (&g.mtx);
    c->close_after = 1;
    pthread_mutex_unlock (&g.mtx);
  }
  else if (op == 0x9) //ping
    ws_send_raw (c, 0xA, payload, (size_t)len);
  else if (op == 0xA) ; //pong
  else if (op == 0x1 || op == 0x2 || op == 0x0)
  {
    if (op == 0x0 && c->frag_op == 0) { free (payload); return -1; } //continuation without a message to continue
    if (op != 0x0 && fin && c->frag_op == 0)
      ws_message (c, op, payload, (size_t)len);
    else
    {
      if (op != 0x0) { c->frag_op = op; c->frag_len = 0; }
      if (c->frag_len + len > WEB_MAX_MESSAGE) { free (payload); return -1; }
      uint8_t * t = realloc (c->frag, c->frag_len + (size_t)len + 1);
      if (!t) { free (payload); return -1; }
      c->frag = t;
      memcpy (c->frag + c->frag_len, payload, (size_t)len);
      c->frag_len += (size_t)len;
      c->frag[c->frag_len] = 0;
      if (fin)
      {
        ws_message (c, c->frag_op, c->frag, c->frag_len);
        c->frag_len = 0;
        c->frag_op = 0;
      }
    }
  }
  free (payload);
  return (long)(pos + len);
}

//----------------------------------------------------------------------------
// Client lifecycle and socket IO
//----------------------------------------------------------------------------

static void client_close (web_client * c)
{
  int was_ws = c->is_ws;
  int id = c->id;
  pthread_mutex_lock (&g.mtx);
  while (c->qn > 0) client_pop (c);
  if (c->dl) fclose (c->dl);
  c->dl = NULL;
  close (c->fd);
  free (c->in);
  free (c->frag);
  memset (c, 0, sizeof(web_client));
  pthread_mutex_unlock (&g.mtx);
  if (was_ws)
  {
    char cmd[64];
    int cl = snprintf (cmd, sizeof(cmd), "{\"cmd\":\"_disconnect\"}");
    push_command (id, cmd, (size_t)cl);
  }
}

//returns 0 if the client should be closed
static int client_read (web_client * c)
{
  uint8_t buf[65536];
  for (;;)
  {
    ssize_t r = recv (c->fd, buf, sizeof(buf), 0);
    if (r == 0) return 0;
    if (r < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      return 0;
    }
    size_t limit = c->is_ws ? (size_t)WEB_MAX_MESSAGE + 64 : (size_t)WEB_MAX_HEADER;
    if (c->in_len + (size_t)r > limit) return 0;
    if (c->in_len + (size_t)r + 1 > c->in_cap)
    {
      size_t ncap = c->in_cap ? c->in_cap : 8192;
      while (ncap < c->in_len + (size_t)r + 1) ncap *= 2;
      uint8_t * t = realloc (c->in, ncap);
      if (!t) return 0;
      c->in = t;
      c->in_cap = ncap;
    }
    memcpy (c->in + c->in_len, buf, (size_t)r);
    c->in_len += (size_t)r;
    c->in[c->in_len] = 0;
  }

  if (!c->is_ws)
  {
    //one request per connection (every response closes it): a pipelined second request
    //would queue a second response and replace the open download
    if (c->close_after || c->dl) { c->in_len = 0; return 1; }
    if (!c->in) return 1; //woken without data
    char * end = strstr ((char *)c->in, "\r\n\r\n");
    if (!end) return 1;
    size_t hlen = (size_t)(end - (char *)c->in) + 4;
    char save = c->in[hlen];
    c->in[hlen] = 0;
    handle_http (c, (char *)c->in);
    c->in[hlen] = (uint8_t)save;
    memmove (c->in, c->in + hlen, c->in_len - hlen);
    c->in_len -= hlen;
    if (!c->is_ws) return 1; //plain http, response queued with close_after
  }

  size_t off = 0;
  while (off < c->in_len)
  {
    long used = ws_parse (c, c->in + off, c->in_len - off);
    if (used < 0) return 0;
    if (used == 0) break;
    off += (size_t)used;
  }
  if (off)
  {
    memmove (c->in, c->in + off, c->in_len - off);
    c->in_len -= off;
  }
  return 1;
}

//returns 0 if the client should be closed
static int client_flush (web_client * c)
{
  int keep = 1;
  pthread_mutex_lock (&g.mtx);
  for (;;)
  {
    if (c->qn == 0 && c->dl)
    {
      //stream next chunk of a download
      size_t want = 65536;
      if (c->dl_left < (long long)want) want = (size_t)c->dl_left;
      web_msg * m = msg_new (WEB_KIND_CTRL, want);
      size_t got = (m && want) ? fread (m->data, 1, want, c->dl) : 0;
      if (got == 0)
      {
        free (m);
        fclose (c->dl);
        c->dl = NULL;
        c->close_after = 1;
      }
      else
      {
        m->len = got;
        c->dl_left -= (long long)got;
        if (!client_enqueue (c, m)) { free (m); c->dl_left = 0; } //closing: drop the rest
        if (c->dl_left <= 0)
        {
          fclose (c->dl);
          c->dl = NULL;
          c->close_after = 1;
        }
      }
    }
    if (c->qn == 0) break;
    web_qitem * it = &c->q[c->qh];
    ssize_t w = send (c->fd, it->m->data + it->off, it->m->len - it->off, MSG_NOSIGNAL);
    if (w < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      keep = 0;
      break;
    }
    it->off += (size_t)w;
    if (it->off >= it->m->len) client_pop (c);
  }
  if (c->qn == 0 && !c->dl && c->close_after) keep = 0;
  if (c->close_after == 2) keep = 0;
  pthread_mutex_unlock (&g.mtx);
  return keep;
}

static void set_nonblock (int fd)
{
  int fl = fcntl (fd, F_GETFL, 0);
  if (fl >= 0) fcntl (fd, F_SETFL, fl | O_NONBLOCK);
}

static void accept_clients (void)
{
  for (;;)
  {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    int fd = accept (g.listen_fd, (struct sockaddr *)&ss, &sl);
    if (fd < 0) return;
    set_nonblock (fd);
    int one = 1;
    setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    #ifdef SO_NOSIGPIPE
    setsockopt (fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    #endif
    int slot = -1;
    pthread_mutex_lock (&g.mtx);
    for (int i = 0; i < WEB_MAX_CLIENTS; i++)
      if (!g.clients[i].used) { slot = i; break; }
    if (slot >= 0)
    {
      web_client * c = &g.clients[slot];
      memset (c, 0, sizeof(web_client));
      c->used = 1;
      c->fd = fd;
      c->id = ++g.next_id;
      if (ss.ss_family == AF_INET)
        inet_ntop (AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, c->ip, sizeof(c->ip));
      else if (ss.ss_family == AF_INET6)
        inet_ntop (AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, c->ip, sizeof(c->ip));
    }
    pthread_mutex_unlock (&g.mtx);
    if (slot < 0)
    {
      const char * busy = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      ssize_t w = send (fd, busy, strlen (busy), MSG_NOSIGNAL);
      UNUSED(w);
      close (fd);
    }
  }
}

static void * web_thread (void * arg)
{
  UNUSED(arg);
  struct pollfd fds[WEB_MAX_CLIENTS + 2];
  int map[WEB_MAX_CLIENTS + 2];

  while (!g.stop)
  {
    int n = 0;
    fds[n].fd = g.listen_fd; fds[n].events = POLLIN; fds[n].revents = 0; map[n++] = -1;
    fds[n].fd = g.wake_r;    fds[n].events = POLLIN; fds[n].revents = 0; map[n++] = -1;
    pthread_mutex_lock (&g.mtx);
    for (int i = 0; i < WEB_MAX_CLIENTS; i++)
    {
      web_client * c = &g.clients[i];
      if (!c->used) continue;
      fds[n].fd = c->fd;
      fds[n].events = POLLIN;
      if (c->qn > 0 || c->dl || c->close_after) fds[n].events |= POLLOUT;
      fds[n].revents = 0;
      map[n++] = i;
    }
    pthread_mutex_unlock (&g.mtx);

    int r = poll (fds, (nfds_t)n, 500);
    if (r < 0)
    {
      if (errno == EINTR) continue;
      break;
    }

    if (fds[1].revents & POLLIN)
    {
      char tmp[256];
      while (read (g.wake_r, tmp, sizeof(tmp)) > 0) ;
    }
    if (fds[0].revents & POLLIN) accept_clients ();

    for (int k = 2; k < n; k++)
    {
      web_client * c = &g.clients[map[k]];
      if (!c->used || c->fd != fds[k].fd) continue;
      short re = fds[k].revents;
      int keep = 1;
      if (re & (POLLERR | POLLNVAL)) keep = 0;
      if (keep && (re & (POLLIN | POLLHUP))) keep = client_read (c);
      if (keep) keep = client_flush (c);
      if (!keep) client_close (c);
    }
  }
  return NULL;
}

//----------------------------------------------------------------------------
// Start / stop
//----------------------------------------------------------------------------

int web_server_start (const char * bind_addr, int port, const char * root_dir, const char * data_dir, const char * password)
{
  if (g.running) return 0;
  memset (&g, 0, sizeof(g));
  pthread_mutex_init (&g.mtx, NULL);
  pthread_mutex_init (&g.cmd_mtx, NULL);
  if (root_dir) snprintf (g.root, sizeof(g.root), "%s", root_dir);
  if (data_dir) snprintf (g.data, sizeof(g.data), "%s", data_dir);
  if (password) snprintf (g.password, sizeof(g.password), "%s", password);

  //session token for download links when a password is set
  FILE * ur = fopen ("/dev/urandom", "rb");
  uint8_t rnd[16];
  size_t got = ur ? fread (rnd, 1, sizeof(rnd), ur) : 0;
  if (ur) fclose (ur);
  if (got != sizeof(rnd))
  {
    srand ((unsigned int)time (NULL) ^ (unsigned int)getpid ());
    for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)(rand () & 0xFF);
  }
  for (int i = 0; i < 16; i++) snprintf (g.token + i*2, 3, "%02x", rnd[i]);

  //a closed browser tab must never kill the decoder
  signal (SIGPIPE, SIG_IGN);

  struct addrinfo hints, * res = NULL;
  memset (&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  char portstr[16];
  snprintf (portstr, sizeof(portstr), "%d", port);
  const char * host = (bind_addr && bind_addr[0] && strcmp (bind_addr, "*") != 0) ? bind_addr : NULL;
  int gai = getaddrinfo (host, portstr, &hints, &res);
  if (gai != 0 || !res)
  {
    fprintf (stderr, "Web: unable to resolve bind address %s: %s\n", bind_addr ? bind_addr : "*", gai_strerror (gai));
    return -1;
  }
  g.listen_fd = -1;
  for (struct addrinfo * ai = res; ai; ai = ai->ai_next)
  {
    int fd = socket (ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    int one = 1;
    setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind (fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen (fd, 16) == 0)
    {
      g.listen_fd = fd;
      break;
    }
    close (fd);
  }
  freeaddrinfo (res);
  if (g.listen_fd < 0)
  {
    fprintf (stderr, "Web: unable to listen on %s:%d (%s)\n", bind_addr ? bind_addr : "*", port, strerror (errno));
    return -1;
  }
  set_nonblock (g.listen_fd);

  int p[2];
  if (pipe (p) != 0)
  {
    close (g.listen_fd);
    return -1;
  }
  g.wake_r = p[0];
  g.wake_w = p[1];
  set_nonblock (g.wake_r);
  set_nonblock (g.wake_w);

  g.running = 1;
  if (pthread_create (&g.thread, NULL, web_thread, NULL) != 0)
  {
    g.running = 0;
    close (g.listen_fd);
    return -1;
  }
  return 0;
}

void web_server_stop (void)
{
  if (!g.running) return;
  g.stop = 1;
  wake ();
  pthread_join (g.thread, NULL);
  for (int i = 0; i < WEB_MAX_CLIENTS; i++)
    if (g.clients[i].used) client_close (&g.clients[i]);
  close (g.listen_fd);
  close (g.wake_r);
  close (g.wake_w);
  g.wake_w = 0;
  pthread_mutex_lock (&g.cmd_mtx);
  while (g.cmd_count > 0)
  {
    free (g.cmds[g.cmd_head].json);
    g.cmd_head = (g.cmd_head + 1) % WEB_CMD_QUEUE_LEN;
    g.cmd_count--;
  }
  pthread_mutex_unlock (&g.cmd_mtx);
  g.running = 0;
}
