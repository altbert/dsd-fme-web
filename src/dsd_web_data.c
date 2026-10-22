/*-------------------------------------------------------------------------------
 * dsd_web_data.c
 * DSD-FME web interface: activity log (voice calls, decoded packet data and
 * trunking signalling), the decoders behind it, and the system picture
 *
 * Runs on the decoder thread only. The decoders hand completed DMR packet data
 * and P25 TSBKs over through read-only taps; they are decoded again here into
 * labelled fields for the browser, so console output and decoder state stay as
 * they were. Each activity item is rendered to JSON once, kept in a ring for
 * browsers that connect later, and broadcast when it is added or changes (a
 * call is one item that is updated until it ends).
 *-----------------------------------------------------------------------------*/

#include "dsd_web.h"
#include "synctype.h"
#include "bp.h"      //BPK[256] Basic Privacy lookup table
#include <sys/time.h>
#include <ctype.h>

#define ACT_MAX    600  //items kept for browsers that connect later
#define ACT_PEND   128  //items added or changed since the last broadcast
#define CALL_HANG  1.5  //seconds without voice before a call counts as over
#define GRANT_HANG 4.0  //seconds without a grant update before a trunked call counts as over
#define GRANT_MAX  16
#define P25_NB_MAX 24
#define RECENT_MAX 8    //packet items kept to fold in repeats and delivery reports
#define REPEAT_WIN 20.0 //seconds in which an identical packet counts as a retry

//one activity item while it is built (calls and grants keep theirs until they end)
typedef struct
{
  uint32_t id;             //0 until first published
  const char * kind;       //"call", "data", "sig", "sys"
  char proto[16];
  int slot;                //1 or 2, 0 when the channel has no slots
  long long src, dst;      //radio and talkgroup ids, -1 = none
  char srcs[64], dsts[64]; //callsign style ids (YSF, D-STAR, M17, dPMR)
  int gi;                  //0 group, 1 individual, -1 not known
  char type[48];           //short label, e.g. "ARS" or "Group call"
  char sum[400];           //one line summary
  char enc[64];            //encryption, empty when clear
  int has_pos;
  double lat, lon;
  double t, t_end;         //start and last activity (wall clock)
  int live;                //call still in progress
  long long freq;          //channel frequency in Hz, 0 = not known
  int crc;                 //1 checked ok, 0 failed, -1 not checked
  int rep;                 //times the same packet was heard (confirmed delivery retries)
  char key[48];            //identity of the message when a resend is not byte identical (not sent)
  char ack[96];            //delivery report from the receiving end
  jb f;                    //labelled fields, rendered JSON array members
  char hex[1600];          //payload as hex
} act_item;

//what is on a slot right now, as far as a voice call goes
typedef struct
{
  int active;
  char proto[16];
  long long tg, src;
  char tgs[64], srcs[64];
  int gi;
  char enc[64];
  char alias[200];
  char gps[200];
  int emerg;
  char sys[64];
} call_snap;

typedef struct
{
  int on;
  act_item a;
  call_snap s;
  char gps0[200]; //the decoder's GPS string when the call began (it keeps the last one heard on the slot)
} call_track;

//a trunked call seen on a control channel: the first grant opens it, updates keep it going
typedef struct
{
  int on;
  int kind;          //0 group, 1 unit to unit, 2 data, 3 telephone
  int ch;
  long long id;      //talkgroup or target
  int svc;
  act_item a;
} grant_track;

typedef struct
{
  int rfss, site, sysid, ch, cfva, ssc;
  double t;
} p25_neighbor;

//a recent packet item, kept so retries and delivery reports update it instead of adding rows
typedef struct
{
  int on;
  int slot;
  uint32_t hash;  //of the packet as received
  act_item a;
} recent_item;

static struct
{
  int inited;
  dsd_opts * opts;
  dsd_state * state;

  uint32_t ids[ACT_MAX];
  char * json[ACT_MAX];
  int head, count;
  uint32_t next_id;
  uint32_t pend[ACT_PEND];
  int npend;

  call_track call[2];
  grant_track grant[GRANT_MAX];

  //DMR data headers of the packet being assembled, per slot
  struct
  {
    int dpf, sap, gi, a, ddfmt, bp;
    int nprop;
    uint8_t prop[3][10]; //proprietary (vendor) headers chained behind it
  } hdr[2];

  recent_item recent[RECENT_MAX];
  int recent_next;

  //the packet as received (before any decryption) and its CRC result, per slot
  uint8_t raw[2][24 * 128];
  int raw_len[2], raw_crc[2], raw_valid[2];

  //a detailed data item already covers what the decoder logs next on this slot: claim 1 adds
  //the decoder's line to it, 2 drops it (a retry); claims end with the frame (next poll)
  int claim[2];
  act_item * claimed[2];

  //P25 site picture from the control channel broadcasts
  long long p25_wacn;
  int p25_sysid, p25_rfss, p25_site, p25_cc_ch, p25_ssc;
  double p25_t;
  p25_neighbor nb[P25_NB_MAX];
  int nnb;
  int sccb[4], nsccb;
  unsigned iden_seen;  //channel identifiers already logged (bit per id)
  char p25_time[64];
  long long p25_logged_wacn;
  int p25_logged_sys, p25_logged_rfss, p25_logged_site, p25_logged_cc;

  char * sys_json;     //last system picture sent
  double sys_t;
  double dmr_seen, nxdn_seen; //last DMR / NXDN sync (the picture outlasts gaps between transmissions)
  int dmr_ms;                 //that DMR sync was direct mode
} A;

//----------------------------------------------------------------------------
// Small helpers
//----------------------------------------------------------------------------

static double wall_now (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

static const char * sync_name (int st)
{
  if (st >= 0 && st < 44 && SyncTypes[st]) return SyncTypes[st];
  return "Digital";
}

//talkgroup / radio name from the group CSV import
static const char * id_name (long long id)
{
  dsd_state * state = A.state;
  if (id <= 0 || !state || !state->group_array) return NULL;
  int n = state->group_tally;
  if (n > 16778000) n = 16778000; //entries dsd_main allocates
  for (int i = 0; i < n; i++)
    if ((long long)state->group_array[i].groupNumber == id && state->group_array[i].groupName[0])
      return state->group_array[i].groupName;
  return NULL;
}

//printable text from 8-bit characters (non ASCII bytes pass through, jb_escape maps them to UTF-8)
static void text8 (char * out, size_t n, const uint8_t * p, int len)
{
  size_t k = 0;
  for (int i = 0; i < len && k + 1 < n; i++)
  {
    uint8_t c = p[i];
    if (c == 0) continue;
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    else if (c < 0x20 || c == 0x7F) c = '.';
    out[k++] = (char)c;
  }
  out[k] = 0;
}

//UTF-16 (le: little endian) to UTF-8
static void text16 (char * out, size_t n, const uint8_t * p, int len, int le)
{
  size_t k = 0;
  for (int i = 0; i + 1 < len && k + 4 < n; i += 2)
  {
    unsigned c = le ? (unsigned)(p[i] | (p[i+1] << 8)) : (unsigned)((p[i] << 8) | p[i+1]);
    if (c == 0) continue;
    if (c == '\r' || c == '\n' || c == '\t' || c == 0x2028 || c == 0x2029) c = ' ';
    else if (c < 0x20 || c == 0x7F) c = '.';
    else if (c >= 0xD800 && c < 0xE000) c = '?'; //surrogate halves
    if (c < 0x80) out[k++] = (char)c;
    else if (c < 0x800) { out[k++] = (char)(0xC0 | (c >> 6)); out[k++] = (char)(0x80 | (c & 0x3F)); }
    else
    {
      out[k++] = (char)(0xE0 | (c >> 12));
      out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[k++] = (char)(0x80 | (c & 0x3F));
    }
  }
  out[k] = 0;
}

//share of printable ASCII, in percent
static int printable_pct (const uint8_t * p, int len)
{
  if (len <= 0) return 0;
  int n = 0;
  for (int i = 0; i < len; i++)
    if ((p[i] >= 0x20 && p[i] < 0x7F) || p[i] == '\r' || p[i] == '\n') n++;
  return n * 100 / len;
}

//share of UTF-16 code units that read as text (Latin, Greek, Cyrillic), in percent
static int text16_pct (const uint8_t * p, int len, int le)
{
  int n = 0, good = 0;
  for (int i = 0; i + 1 < len; i += 2)
  {
    unsigned c = le ? (unsigned)(p[i] | (p[i+1] << 8)) : (unsigned)((p[i] << 8) | p[i+1]);
    if (c == 0) continue;
    n++;
    if ((c >= 0x20 && c < 0x7F) || c == '\r' || c == '\n' || (c >= 0xA0 && c < 0x250) || (c >= 0x370 && c < 0x530)) good++;
  }
  return n ? good * 100 / n : 0;
}

//callsign style field without padding or control characters
static void trim_id (char * out, size_t n, const char * in, size_t max)
{
  size_t k = 0, last = 0;
  for (size_t i = 0; i < max && in[i] && k + 1 < n; i++)
  {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x20 || c == 0x7F) c = ' ';
    if (c == ' ' && k == 0) continue;
    out[k++] = (char)c;
    if (c != ' ') last = k;
  }
  out[last] = 0;
}

static unsigned be16 (const uint8_t * p) { return (unsigned)((p[0] << 8) | p[1]); }
static uint32_t be24 (const uint8_t * p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static uint32_t be32 (const uint8_t * p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t le32 (const uint8_t * p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

//----------------------------------------------------------------------------
// Activity items: build, render, keep, broadcast
//----------------------------------------------------------------------------

static void act_new (act_item * a, const char * kind, const char * proto, int slot)
{
  memset (a, 0, sizeof(*a));
  a->kind = kind;
  snprintf (a->proto, sizeof(a->proto), "%s", proto ? proto : "");
  a->slot = slot;
  a->src = a->dst = -1;
  a->gi = -1;
  a->crc = -1;
  a->t = a->t_end = wall_now ();
  jb_init (&a->f);
}

static void act_free (act_item * a)
{
  jb_free (&a->f);
}

static void act_kv (act_item * a, const char * k, const char * fmt, ...) __attribute__((format(printf, 3, 4)));
static void act_kv (act_item * a, const char * k, const char * fmt, ...)
{
  char v[1200];
  va_list ap;
  va_start (ap, fmt);
  vsnprintf (v, sizeof(v), fmt, ap);
  va_end (ap);
  if (a->f.len) jb_rawn (&a->f, ",", 1);
  jb_rawn (&a->f, "[\"", 2);
  jb_escape (&a->f, k, strlen (k));
  jb_rawn (&a->f, "\",\"", 3);
  jb_escape (&a->f, v, strlen (v));
  jb_rawn (&a->f, "\"]", 2);
}

static void act_sum (act_item * a, const char * fmt, ...) __attribute__((format(printf, 2, 3)));
static void act_sum (act_item * a, const char * fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  vsnprintf (a->sum, sizeof(a->sum), fmt, ap);
  va_end (ap);
}

static void act_type (act_item * a, const char * t)
{
  snprintf (a->type, sizeof(a->type), "%s", t);
}

static void act_hex (act_item * a, const uint8_t * p, int n)
{
  static const char hx[] = "0123456789ABCDEF";
  int max = (int)(sizeof(a->hex) - 1) / 2;
  if (n < 0) n = 0;
  if (n > max) n = max;
  for (int i = 0; i < n; i++)
  {
    a->hex[2 * i] = hx[p[i] >> 4];
    a->hex[2 * i + 1] = hx[p[i] & 15];
  }
  a->hex[2 * n] = 0;
}

static void act_pos (act_item * a, double lat, double lon)
{
  if (!(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)) return;
  if (lat == 0 && lon == 0) return;
  a->has_pos = 1;
  a->lat = lat;
  a->lon = lon;
}

static char * act_render (const act_item * a)
{
  jb b;
  jb_init (&b);
  jb_obj (&b, NULL);
  jb_int (&b, "id", a->id);
  jb_dbl (&b, "t", a->t, 3);
  jb_str (&b, "k", a->kind);
  jb_str (&b, "p", a->proto);
  if (a->slot) jb_int (&b, "s", a->slot);
  if (a->src >= 0) jb_int (&b, "src", a->src);
  if (a->dst >= 0) jb_int (&b, "dst", a->dst);
  if (a->srcs[0]) jb_str (&b, "srcs", a->srcs);
  if (a->dsts[0]) jb_str (&b, "dsts", a->dsts);
  const char * n;
  if ((n = id_name (a->src)) != NULL) jb_str (&b, "sn", n);
  if ((n = id_name (a->dst)) != NULL) jb_str (&b, "dn", n);
  if (a->gi >= 0) jb_int (&b, "g", a->gi);
  jb_str (&b, "ty", a->type);
  jb_str (&b, "sum", a->sum);
  if (a->enc[0]) jb_str (&b, "enc", a->enc);
  if (a->has_pos)
  {
    jb_dbl (&b, "lat", a->lat, 6);
    jb_dbl (&b, "lon", a->lon, 6);
  }
  if (!strcmp (a->kind, "call"))
  {
    jb_bool (&b, "live", a->live);
    jb_dbl (&b, "dur", a->t_end - a->t, 1);
  }
  if (a->freq > 0) jb_int (&b, "freq", a->freq);
  if (a->crc >= 0) jb_int (&b, "crc", a->crc);
  if (a->rep > 1) jb_int (&b, "rep", a->rep);
  if (a->ack[0]) jb_str (&b, "ack", a->ack);
  jb_key (&b, "f");
  jb_rawn (&b, "[", 1);
  if (a->f.len) jb_rawn (&b, a->f.buf, a->f.len);
  jb_rawn (&b, "]", 1);
  if (a->hex[0]) jb_str (&b, "hex", a->hex);
  jb_obj_end (&b);
  char * s = b.buf ? b.buf : strdup ("{}");
  b.buf = NULL;
  jb_free (&b);
  return s;
}

static const char * act_find (uint32_t id)
{
  for (int k = 0; k < A.count; k++)
  {
    int i = (A.head - 1 - k + ACT_MAX) % ACT_MAX;
    if (A.ids[i] == id) return A.json[i];
  }
  return NULL;
}

static void act_flush (void)
{
  if (!A.npend) return;
  if (web_server_clients () > 0)
  {
    jb b;
    jb_init (&b);
    jb_raw (&b, "{\"type\":\"activity\",\"items\":[");
    int first = 1;
    for (int k = 0; k < A.npend; k++)
    {
      const char * j = act_find (A.pend[k]);
      if (!j) continue;
      if (!first) jb_rawn (&b, ",", 1);
      jb_raw (&b, j);
      first = 0;
    }
    jb_raw (&b, "]}");
    //control kind: never dropped for a slow browser, unlike the status snapshots
    if (b.buf) web_broadcast (WEB_KIND_CTRL, WS_OP_TEXT, b.buf, b.len, 0);
    jb_free (&b);
  }
  A.npend = 0;
}

//render and keep (an update replaces the earlier version of the same item)
static void act_publish (act_item * a)
{
  if (!A.inited) return;
  if (!a->id) a->id = A.next_id++;
  char * json = act_render (a);
  int found = 0;
  for (int k = 0; k < A.count && !found; k++)
  {
    int i = (A.head - 1 - k + ACT_MAX) % ACT_MAX;
    if (A.ids[i] == a->id)
    {
      free (A.json[i]);
      A.json[i] = json;
      found = 1;
    }
  }
  if (!found)
  {
    if (A.count == ACT_MAX) free (A.json[A.head]);
    else A.count++;
    A.ids[A.head] = a->id;
    A.json[A.head] = json;
    A.head = (A.head + 1) % ACT_MAX;
  }
  for (int k = 0; k < A.npend; k++)
    if (A.pend[k] == a->id) return;
  if (A.npend == ACT_PEND) act_flush ();
  A.pend[A.npend++] = a->id;
}

//publish a one-shot item and release it
static void act_emit (act_item * a)
{
  act_publish (a);
  act_free (a);
}

//----------------------------------------------------------------------------
// Encryption names (shared with the slot cards in dsd_web.c)
//----------------------------------------------------------------------------

//DMR / P25 algorithm ids by name (DMR vendors reuse the low range, P25 sits at 0x80+)
static const char * alg_name (int alg)
{
  switch (alg)
  {
    case 0x01: case 0x21: case 0xAA: return "RC4";
    case 0x02: return "Hytera Enhanced";
    case 0x05: case 0x25: case 0x84: return "AES-256";
    case 0x22: case 0x81: return "DES";
    case 0x24: case 0x85: case 0x89: return "AES-128";
    case 0x26: return "Caltta BP";
    case 0x82: return "2-key 3DES";
    case 0x83: return "3DES";
    case 0x88: return "AES-CBC";
    case 0x9F: return "DES-XL";
    case 0xA0: return "DVI-XL";
    case 0xA1: return "DVP-XL";
  }
  if (alg >= 0x31 && alg <= 0x3A) return "Kirisun";
  return NULL;
}

//encryption on a slot's call, by name: the ALG id where the protocol sends one, otherwise
//what its privacy flags imply (DMR Basic Privacy carries no ALG, only the service options bit).
//Empty when the call is clear or nothing is decoding.
void web_enc_name (dsd_state * state, int slot, char * out, size_t n)
{
  int st = state->lastsynctype;
  out[0] = 0;
  if (!state->carrier) return;
  if ((st >= 10 && st <= 13) || (st >= 32 && st <= 34))
  {
    int alg = slot ? state->payload_algidR : state->payload_algid;
    int so = slot ? state->dmr_soR : state->dmr_so;
    int fid = slot ? state->dmr_fidR : state->dmr_fid;
    int burst = slot ? state->dmrburstR : state->dmrburstL;
    if (so == 0x100 || !(so & 0x40) || !(burst == 16 || burst == 0 || burst == 1)) return; //data, clear, or no call
    const char * name = alg_name (alg);
    if (alg == 0 && fid == 0x68) snprintf (out, n, "Hytera Basic Privacy");
    else if (alg == 0 && fid == 0x10) snprintf (out, n, "Basic Privacy");
    else if (alg == 0) snprintf (out, n, "Privacy (FID %02X)", fid);
    else if (name) snprintf (out, n, "%s", name);
    else snprintf (out, n, "ALG %02X", alg);
  }
  else if (st == 0 || st == 1 || st == 35 || st == 36)
  {
    if (slot && st < 35) return; //P25p1 has one voice channel, slot 2 would repeat slot 1
    int p2r = slot && st >= 35;
    int alg = p2r ? state->payload_algidR : state->payload_algid;
    int burst = p2r ? state->dmrburstR : state->dmrburstL;
    if (!(burst == 20 || burst == 21 || (burst >= 25 && burst <= 27))) return; //the ALG outlives the call
    if (alg == 0 || alg == 0x80) return;
    const char * name = alg_name (alg);
    if (name) snprintf (out, n, "%s", name);
    else snprintf (out, n, "ALG %02X", alg);
  }
  else if (st == 28 || st == 29)
  {
    static const char * t[] = { "", "Scrambler", "DES", "AES-256" };
    unsigned c = state->nxdn_cipher_type;
    if (slot || c == 0) return;
    if (c < 4) snprintf (out, n, "%s", t[c]);
    else snprintf (out, n, "Cipher %u", c);
  }
  else if (st == 8 || st == 9 || st == 16 || st == 17 || st == 86 || st == 87 || st == 98 || st == 99)
  {
    if (slot || state->m17_enc == 0) return;
    if (state->m17_enc == 1) snprintf (out, n, "Scrambler %d-bit", (state->m17_enc_st + 1) * 8);
    else if (state->m17_enc == 2) snprintf (out, n, "AES-%d", 128 + 64 * state->m17_enc_st);
    else snprintf (out, n, "Encrypted (type %d)", state->m17_enc_st);
  }
}

//----------------------------------------------------------------------------
// Voice calls: one item per call, opened when ids show up on a slot and
// closed once the slot has been quiet for a moment
//----------------------------------------------------------------------------

//lat / lon out of one of the decoders' GPS strings ("12.34567°S 98.76543°W", "(12.3, -98.7)", ...)
static int parse_pos (const char * s, double * lat, double * lon)
{
  double v[2];
  int n = 0;
  const char * p = s;
  while (*p && n < 2)
  {
    if ((*p >= '0' && *p <= '9') || ((*p == '-' || *p == '+') && p[1] >= '0' && p[1] <= '9'))
    {
      char * e;
      double d = strtod (p, &e);
      if (e == p) { p++; continue; }
      int has_dot = 0;
      for (const char * q = p; q < e; q++) if (*q == '.') has_dot = 1;
      if (has_dot)
      {
        //hemisphere letter within a few characters (past the degree glyph)
        for (const char * q = e; *q && q < e + 6; q++)
        {
          if (*q == 'S' || *q == 'W') { d = -fabs (d); break; }
          if (*q == 'N' || *q == 'E' || *q == ',' || *q == ';') break;
        }
        v[n++] = d;
      }
      p = e;
    }
    else p++;
  }
  if (n < 2) return 0;
  if (fabs (v[0]) > 90 || fabs (v[1]) > 180 || (v[0] == 0 && v[1] == 0)) return 0;
  *lat = v[0];
  *lon = v[1];
  return 1;
}

static void call_snapshot (dsd_opts * opts, dsd_state * state, int slot, call_snap * c)
{
  UNUSED(opts);
  memset (c, 0, sizeof(*c));
  c->gi = -1;
  int st = state->lastsynctype;
  if (!state->carrier || st < 0) return;
  snprintf (c->proto, sizeof(c->proto), "%s", sync_name (st));

  if ((st >= 10 && st <= 13) || (st >= 32 && st <= 34))
  {
    if (slot && st >= 32) return; //MS / direct mode is mono
    int burst = slot ? state->dmrburstR : state->dmrburstL;
    c->src = slot ? state->lastsrcR : state->lastsrc;
    c->tg = slot ? state->lasttgR : state->lasttg;
    if (!(c->src || c->tg) || !(burst == 16 || burst == 0 || burst == 1)) return;
    int so = slot ? state->dmr_soR : state->dmr_so;
    c->emerg = so != 0x100 && (so & 0x80);
    c->gi = state->gi[slot];
    trim_id (c->alias, sizeof(c->alias), state->generic_talker_alias[slot], sizeof(state->generic_talker_alias[slot]));
    trim_id (c->gps, sizeof(c->gps), state->dmr_embedded_gps[slot], sizeof(state->dmr_embedded_gps[slot]));
    if (state->dmr_color_code < 16) snprintf (c->sys, sizeof(c->sys), "CC %u", state->dmr_color_code);
  }
  else if (st == 0 || st == 1 || st == 35 || st == 36)
  {
    if (slot && st < 35) return;
    int p2r = slot && st >= 35;
    int burst = p2r ? state->dmrburstR : state->dmrburstL;
    c->src = p2r ? state->lastsrcR : state->lastsrc;
    c->tg = p2r ? state->lasttgR : state->lasttg;
    if (st < 35 && !(burst >= 25 && burst <= 27)) return;
    if (st >= 35 && !(burst == 20 || burst == 21)) return;
    if (!(c->src || c->tg)) return;
    c->gi = state->gi[slot];
    trim_id (c->alias, sizeof(c->alias), state->generic_talker_alias[slot], sizeof(state->generic_talker_alias[slot]));
    unsigned long long nac = state->nac ? (unsigned long long)state->nac : state->p2_cc;
    if (nac) snprintf (c->sys, sizeof(c->sys), "NAC %03llX", nac);
  }
  else if (st == 28 || st == 29)
  {
    if (slot) return;
    c->src = state->nxdn_last_rid;
    c->tg = state->nxdn_last_tg;
    if (!(c->src || c->tg)) return;
    c->gi = state->gi[0];
    snprintf (c->sys, sizeof(c->sys), "RAN %u", state->nxdn_last_ran);
  }
  else if (st == 30 || st == 31)
  {
    if (slot) return;
    trim_id (c->srcs, sizeof(c->srcs), state->ysf_src, sizeof(state->ysf_src));
    trim_id (c->tgs, sizeof(c->tgs), state->ysf_tgt, sizeof(state->ysf_tgt));
    if (!c->srcs[0]) return;
  }
  else if (st == 6 || st == 7 || st == 18 || st == 19)
  {
    if (slot) return;
    trim_id (c->srcs, sizeof(c->srcs), state->dstar_src, sizeof(state->dstar_src));
    trim_id (c->tgs, sizeof(c->tgs), state->dstar_dst, sizeof(state->dstar_dst));
    if (!c->srcs[0]) return;
  }
  else if (st == 8 || st == 9 || st == 16 || st == 17)
  {
    if (slot) return;
    trim_id (c->srcs, sizeof(c->srcs), state->m17_src_str, sizeof(state->m17_src_str));
    trim_id (c->tgs, sizeof(c->tgs), state->m17_dst_str, sizeof(state->m17_dst_str));
    if (!c->srcs[0]) return;
    snprintf (c->sys, sizeof(c->sys), "CAN %d", state->m17_can);
  }
  else if (st >= 20 && st <= 27)
  {
    if (slot) return;
    trim_id (c->srcs, sizeof(c->srcs), state->dpmr_caller_id, sizeof(state->dpmr_caller_id));
    trim_id (c->tgs, sizeof(c->tgs), state->dpmr_target_id, sizeof(state->dpmr_target_id));
    if (!c->srcs[0]) return;
    snprintf (c->sys, sizeof(c->sys), "CC %d", state->dpmr_color_code);
  }
  else if (st == 14 || st == 15 || st == 37 || st == 38 || (st >= 2 && st <= 5))
  {
    if (slot && st >= 14) return;
    c->src = slot ? state->lastsrcR : state->lastsrc;
    c->tg = slot ? state->lasttgR : state->lasttg;
    if (!(c->src || c->tg)) return;
  }
  else return;

  web_enc_name (state, slot, c->enc, sizeof(c->enc));
  c->active = 1;
}

//same call, allowing for ids that were not known yet (late entry fills them in)
static int same_call (const call_snap * a, const call_snap * b)
{
  if (strcmp (a->proto, b->proto)) return 0;
  if (a->tg && b->tg && a->tg != b->tg) return 0;
  if (a->src && b->src && a->src != b->src) return 0;
  if (a->srcs[0] && b->srcs[0] && strcmp (a->srcs, b->srcs)) return 0;
  if (a->tgs[0] && b->tgs[0] && strcmp (a->tgs, b->tgs)) return 0;
  return 1;
}

static int call_changed (const call_snap * a, const call_snap * b)
{
  return a->tg != b->tg || a->src != b->src || strcmp (a->srcs, b->srcs) || strcmp (a->tgs, b->tgs) ||
         a->gi != b->gi || a->emerg != b->emerg || strcmp (a->enc, b->enc) || strcmp (a->alias, b->alias) ||
         strcmp (a->gps, b->gps) || strcmp (a->sys, b->sys);
}

//ids, alias and position learned during the call stick even when the decoder clears them before
//the end (TLC clears ids and alias, late entry fills them in)
static void call_merge (const call_track * k, call_snap * c)
{
  const call_snap * o = &k->s;
  if (!c->tg) c->tg = o->tg;
  if (!c->src) c->src = o->src;
  if (!c->tgs[0]) snprintf (c->tgs, sizeof(c->tgs), "%s", o->tgs);
  if (!c->srcs[0]) snprintf (c->srcs, sizeof(c->srcs), "%s", o->srcs);
  if (c->gi < 0) c->gi = o->gi;
  if (!c->alias[0]) snprintf (c->alias, sizeof(c->alias), "%s", o->alias);
  if ((!c->gps[0] || !strcmp (c->gps, k->gps0)) && strcmp (o->gps, k->gps0)) snprintf (c->gps, sizeof(c->gps), "%s", o->gps);
}

static void call_fill (call_track * k, const call_snap * c)
{
  act_item * a = &k->a;
  //ids learned during the call stick even when the decoder clears them before the end
  if (c->tg) a->dst = c->tg;
  if (c->src) a->src = c->src;
  if (c->tgs[0]) snprintf (a->dsts, sizeof(a->dsts), "%s", c->tgs);
  if (c->srcs[0]) snprintf (a->srcs, sizeof(a->srcs), "%s", c->srcs);
  if (c->gi >= 0) a->gi = c->gi;
  snprintf (a->enc, sizeof(a->enc), "%s", c->enc);
  if (!a->type[0] || c->gi >= 0)
    act_type (a, a->gi == 1 ? "Private call" : (a->gi == 0 ? "Group call" : "Voice call"));
  jb_reset (&a->f);
  if (c->sys[0]) act_kv (a, "System", "%s", c->sys);
  if (c->emerg) act_kv (a, "Emergency", "yes");
  if (c->alias[0]) act_kv (a, "Talker alias", "%s", c->alias);
  double lat, lon;
  if (c->gps[0] && strcmp (c->gps, k->gps0)) //only a position sent during this call
  {
    act_kv (a, "GPS", "%s", c->gps);
    if (parse_pos (c->gps, &lat, &lon)) act_pos (a, lat, lon);
  }
  char who[96] = "", to[96] = "";
  if (a->srcs[0]) snprintf (who, sizeof(who), "%s", a->srcs);
  else if (a->src > 0) snprintf (who, sizeof(who), "%lld", a->src);
  if (a->dsts[0]) snprintf (to, sizeof(to), "%s", a->dsts);
  else if (a->dst > 0) snprintf (to, sizeof(to), "%s%lld", a->gi == 1 ? "radio " : "TG ", a->dst);
  act_sum (a, "%s%s%s%s%s%s%s", c->emerg ? "EMERGENCY " : "", who[0] ? who : "unknown radio", to[0] ? " → " : "", to,
           c->alias[0] ? " (" : "", c->alias, c->alias[0] ? ")" : "");
  k->s = *c;
}

static void call_end (call_track * k)
{
  if (!k->on) return;
  k->a.live = 0;
  act_publish (&k->a);
  act_free (&k->a);
  k->on = 0;
}

static void calls_poll (dsd_opts * opts, dsd_state * state)
{
  double now = wall_now ();
  for (int s = 0; s < 2; s++)
  {
    call_snap c;
    call_track * k = &A.call[s];
    call_snapshot (opts, state, s, &c);
    if (k->on && (c.active ? !same_call (&k->s, &c) : now - k->a.t_end > CALL_HANG)) call_end (k);
    if (!c.active) continue;
    int slotted = (state->lastsynctype >= 10 && state->lastsynctype <= 13) || state->lastsynctype == 35 || state->lastsynctype == 36;
    if (!k->on)
    {
      act_new (&k->a, "call", c.proto, slotted ? s + 1 : 0);
      memset (&k->s, 0, sizeof(k->s)); //nothing carries over from the previous call
      snprintf (k->gps0, sizeof(k->gps0), "%s", c.gps);
      k->a.live = 1;
      k->a.freq = opts->rtlsdr_center_freq > 0 ? (long long)opts->rtlsdr_center_freq : 0;
      k->on = 1;
      call_fill (k, &c);
      act_publish (&k->a);
      continue;
    }
    k->a.t_end = now;
    call_merge (k, &c);
    if (call_changed (&k->s, &c))
    {
      call_fill (k, &c);
      act_publish (&k->a);
    }
  }
}

//GPS the radio on the slot sent during its current call, "" when none (for the slot cards)
const char * web_call_gps (int slot)
{
  call_track * k = &A.call[slot & 1];
  if (!A.inited || !k->on || !k->s.gps[0] || !strcmp (k->s.gps, k->gps0)) return "";
  return k->s.gps;
}

//----------------------------------------------------------------------------
// Packet data payloads (shared by the DMR packet decoder; IP based ones by P25 too)
//----------------------------------------------------------------------------

static void dec_udp_app (act_item * a, int sport, int dport, const uint8_t * p, int len, int depth);

static void set_type (act_item * a, const char * t)
{
  if (!a->type[0]) act_type (a, t);
}

//Motorola LRRP: variable length numbers (7 bits per byte, top bit = more follows)
static int lrrp_vlq (const uint8_t * p, int end, int * off, uint32_t * out)
{
  uint32_t v = 0;
  int n = 0;
  while (*off < end && n < 5)
  {
    uint8_t b = p[(*off)++];
    v = (v << 7) | (b & 0x7F);
    n++;
    if (!(b & 0x80)) { *out = v; return n; }
  }
  return 0;
}

//LRRP float: an integer part and a fraction part, both variable length
static int lrrp_float (const uint8_t * p, int end, int * off, double * out)
{
  uint32_t i = 0, f = 0;
  if (!lrrp_vlq (p, end, off, &i)) return 0;
  int nf = lrrp_vlq (p, end, off, &f);
  if (!nf) return 0;
  *out = i + f / pow (128.0, nf);
  return 1;
}

static void dec_lrrp (act_item * a, const uint8_t * p, int len)
{
  static const struct { int t; const char * n; } types[] = {
    { 0x04, "Immediate location request (no-ack)" }, { 0x05, "Immediate location request" },
    { 0x06, "Immediate location response (no-ack)" }, { 0x07, "Immediate location response" },
    { 0x09, "Triggered location start request" }, { 0x0B, "Triggered location start response" },
    { 0x0D, "Triggered location report" }, { 0x0F, "Triggered location stop request" },
    { 0x11, "Triggered location stop response" }, { 0x14, "Protocol version request" },
    { 0x15, "Protocol version response" } };
  act_type (a, "LRRP");
  if (len < 2) { act_sum (a, "LRRP (truncated)"); return; }
  int type = p[0];
  const char * tn = NULL;
  for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
    if (types[i].t == type) tn = types[i].n;
  int end = 2 + p[1];
  if (end > len) end = len;
  act_kv (a, "LRRP message", "%s (0x%02X)", tn ? tn : "unknown", type);
  int response = type == 0x07 || type == 0x0B || type == 0x0D || type == 0x11 || type == 0x15 || type == 0x06;
  int off = 2;
  double lat = 0, lon = 0, rad = -1, alt = -1, spd = -1;
  int have_pos = 0, dir = -1, result = -1;
  char when[40] = "";
  while (off < end)
  {
    int tok = p[off++];
    if (tok == 0x22 || tok == 0x23 || tok == 0x24 || tok == 0x25) //request id
    {
      if (off >= end) break;
      int n = p[off++];
      if (off + n > end) break;
      uint32_t id = 0;
      for (int i = 0; i < n && i < 4; i++) id = (id << 8) | p[off + i];
      act_kv (a, "Request id", "%u", id);
      off += n;
      continue;
    }
    if (!response)
    {
      //request parameters are not worth listing one by one
      if (tok == 0x34 || tok == 0x31) { uint32_t v; if (tok == 0x34 && off < end && p[off] == 0x31) off++; if (lrrp_vlq (p, end, &off, &v)) act_kv (a, "Interval", "%u s", v); continue; }
      break;
    }
    if (tok == 0x34 || tok == 0x35) //time stamp, UTC
    {
      if (off + 5 > end) break;
      const uint8_t * t = p + off;
      int year = (t[0] << 6) | (t[1] >> 2), mon = ((t[1] & 3) << 2) | (t[2] >> 6), day = (t[2] & 0x3E) >> 1;
      int hour = ((t[2] & 1) << 4) | (t[3] >> 4), min = ((t[3] & 0xF) << 2) | (t[4] >> 6), sec = t[4] & 0x3F;
      snprintf (when, sizeof(when), "%04d-%02d-%02d %02d:%02d:%02d UTC", year, mon, day, hour, min, sec);
      off += 5;
    }
    else if (tok == 0x51 || tok == 0x54 || tok == 0x55 || tok == 0x66 || tok == 0x69 || tok == 0x6A)
    {
      if (off + 8 > end) break;
      //signed 32 bit, 180 (latitude) and 360 (longitude) degrees over 2^32
      lat = (int32_t)be32 (p + off) * (180.0 / 4294967296.0);
      lon = (int32_t)be32 (p + off + 4) * (360.0 / 4294967296.0);
      have_pos = 1;
      off += 8;
      if ((tok == 0x51 || tok == 0x54 || tok == 0x55) && !lrrp_float (p, end, &off, &rad)) break;
      if ((tok == 0x55 || tok == 0x69 || tok == 0x6A) && !lrrp_float (p, end, &off, &alt)) break;
      if (tok == 0x55) { double acc; if (!lrrp_float (p, end, &off, &acc)) break; }
    }
    else if (tok == 0x56) //heading, 2 degree steps
    {
      if (off >= end) break;
      dir = p[off++] * 2;
    }
    else if (tok == 0x6C || tok == 0x70) //speed m/s (horizontal, vertical)
    {
      double v;
      if (!lrrp_float (p, end, &off, &v)) break;
      if (tok == 0x6C) spd = v;
    }
    else if (tok == 0x37) //result code
    {
      uint32_t v;
      if (!lrrp_vlq (p, end, &off, &v)) break;
      result = (int)v;
    }
    else if (tok == 0x38) result = 0;
    else if (tok == 0x36) { if (off < end) off++; }
    else break; //unknown token, its length is not known either
  }
  if (when[0]) act_kv (a, "Fix time", "%s", when);
  if (have_pos)
  {
    act_kv (a, "Position", "%.6f, %.6f", lat, lon);
    act_pos (a, lat, lon);
  }
  if (rad >= 0) act_kv (a, "Accuracy", "%.0f m", rad);
  if (alt >= 0) act_kv (a, "Altitude", "%.0f m", alt);
  if (spd >= 0) act_kv (a, "Speed", "%.1f km/h", spd * 3.6);
  if (dir >= 0) act_kv (a, "Heading", "%d°", dir);
  if (result >= 0) act_kv (a, "Result", result == 0 ? "success" : "code %d", result);
  if (have_pos && spd >= 0) act_sum (a, "Position %.5f, %.5f, %.0f km/h", lat, lon, spd * 3.6);
  else if (have_pos) act_sum (a, "Position %.5f, %.5f", lat, lon);
  else act_sum (a, "%s%s", tn ? tn : "LRRP message", result > 0 ? " (failed)" : "");
}

static void dec_ars (act_item * a, const uint8_t * p, int len)
{
  static const char * pdu[16] = { "Device registration", "Device deregistration", NULL, NULL, "Registration query",
    "User registration", "User deregistration", "User registration answer", NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    "Registration answer" };
  static const char * fail[] = { "device not authorized", "user id not valid", "user validation timed out" };
  act_type (a, "ARS");
  if (len < 3) { act_sum (a, "ARS (truncated)"); return; }
  int end = 2 + (int)be16 (p);
  if (end > len) end = len;
  int h = p[2];
  int ext = h >> 7, ack = (h >> 6) & 1, pri = (h >> 5) & 1, type = h & 0xF;
  int off = 3;
  const char * tn = pdu[type] ? pdu[type] : "ARS message";
  act_kv (a, "ARS message", "%s (type %d)%s", tn, type, pri ? ", priority" : "");
  if (type == 0 || type == 5)
  {
    int event = -1;
    if (ext && off < end)
    {
      int rh = p[off++];
      event = (rh >> 5) & 3;
      while ((rh & 0x80) && off < end) rh = p[off++];
    }
    char dev[80] = "", user[80] = "";
    int pw = 0;
    if (off < end) { int n = p[off++]; if (off + n <= end) { text8 (dev, sizeof(dev), p + off, n); off += n; } else off = end; }
    if (off < end) { int n = p[off++]; if (off + n <= end) { text8 (user, sizeof(user), p + off, n); off += n; } else off = end; }
    if (off < end) pw = p[off];
    if (event == 1) act_kv (a, "Event", "initial (first registration since power on or a new site)");
    else if (event == 2) act_kv (a, "Event", "refresh");
    if (dev[0]) act_kv (a, "Device id", "%s", dev);
    if (user[0]) act_kv (a, "User id", "%s", user);
    if (pw) act_kv (a, "Password", "%d characters", pw);
    act_kv (a, "Answer wanted", "%s", ack ? "yes" : "no");
    act_sum (a, "%s%s%s%s%s%s", type == 5 ? "User registration" : "Registration",
             event == 1 ? " (initial)" : (event == 2 ? " (refresh)" : ""),
             dev[0] ? ", device " : "", dev, user[0] ? ", user " : "", user);
  }
  else if (type == 15 || type == 7)
  {
    int v = -1;
    if (ext && off < end) v = p[off++] & 0x7F;
    if (ack)
    {
      if (v > 0)
      {
        act_kv (a, "Refresh", "every %d minutes", v * 30);
        act_sum (a, "Registration accepted, refresh every %d min", v * 30);
      }
      else act_sum (a, "Registration accepted");
    }
    else
    {
      const char * r = (v >= 0 && v < 3) ? fail[v] : (v >= 0 ? "transmission failure" : "no reason given");
      act_kv (a, "Reason", "%s", r);
      act_sum (a, "Registration refused: %s", r);
    }
  }
  else if (type == 1) act_sum (a, "Deregistration (radio going off)");
  else if (type == 4) act_sum (a, "Registration query (is the radio still there?)");
  else if (type == 6) act_sum (a, "User deregistration");
  else act_sum (a, "%s", tn);
}

//Motorola text message service (TMS)
static void dec_tms (act_item * a, const uint8_t * p, int len)
{
  act_type (a, "Text");
  if (len < 3) { act_sum (a, "Text message (truncated)"); return; }
  int end = 2 + (int)be16 (p);
  if (end > len) end = len;
  int h = p[2];
  int ext = h >> 7, ackreq = (h >> 6) & 1, type = h & 0x1F;
  if (type == 0x1F)
  {
    act_type (a, "Text ack");
    act_sum (a, "Text message delivered (acknowledgement)");
    return;
  }
  int off = 3;
  if (off < end)
  {
    int al = p[off++];
    if (al && off + al <= end)
    {
      char addr[80];
      text16 (addr, sizeof(addr), p + off, al, 1);
      if (addr[0]) act_kv (a, "Address", "%s", addr);
      off += al;
    }
  }
  int more = ext;
  while (more && off < end) more = p[off++] >> 7;
  char text[700];
  text16 (text, sizeof(text), p + off, end - off, 1);
  char * t = text;
  while (*t == ' ') t++;
  act_kv (a, "Text", "%s", t);
  if (ackreq) act_kv (a, "Delivery report", "requested");
  act_sum (a, "\"%s\"", t);
}

static uint32_t bits_at (const uint8_t * bits, int s, int n)
{
  uint32_t v = 0;
  for (int i = 0; i < n; i++) v = (v << 1) | bits[s + i];
  return v;
}

//ETSI LIP short location report (same layout as the DSD-FME console decoder)
static void dec_lip (act_item * a, const uint8_t * p, int len)
{
  act_type (a, "LIP");
  if (len < 11) { act_sum (a, "LIP location (truncated)"); return; }
  uint8_t bits[88];
  for (int i = 0; i < 88; i++) bits[i] = (p[i >> 3] >> (7 - (i & 7))) & 1;
  int lon_s = bits[8], lat_s = bits[33];
  uint32_t lon = bits_at (bits, 9, 24), lat = bits_at (bits, 34, 23);
  int pos_err = (int)bits_at (bits, 57, 2), vel = (int)bits_at (bits, 59, 7), dir = (int)bits_at (bits, 66, 4);
  if (lat_s) lat = 0x800001 - lat;
  if (lon_s) lon = 0x1000001 - lon;
  double la = lat * (180.0 / 16777216.0) * (lat_s ? -1 : 1);
  double lo = lon * (360.0 / 33554432.0) * (lon_s ? -1 : 1);
  double v = vel > 28 ? 16.0 * pow (1.038, vel - 13) : vel;
  act_kv (a, "Position", "%.5f, %.5f", la, lo);
  act_kv (a, "Speed", "%.0f km/h", v);
  act_kv (a, "Heading", "%.0f°", dir * 22.5);
  if (pos_err != 3) act_kv (a, "Accuracy", "< %d m", 2 * (int)pow (10, pos_err));
  act_pos (a, la, lo);
  act_sum (a, "Position %.5f, %.5f, %.0f km/h", la, lo, v);
}

//18 byte GPS fix of the compressed Hytera location messages: UTC time and date, then
//DDMM.MMMM x 10^4 (DDDMM.MMMM for longitude) little endian with a hemisphere flag in the top
//bit, speed in 0.1 knot and heading in degrees. The flags are inferred from radios south and
//west of Greenwich: a set longitude flag means west, a clear latitude flag means south.
static void hyt_gps_bin (act_item * a, const uint8_t * g)
{
  uint32_t la = le32 (g + 6), lo = le32 (g + 10);
  uint32_t lam = la & 0x7FFFFFFF, lom = lo & 0x7FFFFFFF;
  int spd = g[14] | (g[15] << 8), dir = g[16] | (g[17] << 8);
  if (g[0] < 24 && g[1] < 60 && g[2] < 60 && g[3] >= 1 && g[3] <= 31 && g[4] >= 1 && g[4] <= 12)
    act_kv (a, "Fix time", "20%02d-%02d-%02d %02d:%02d:%02d UTC", g[5], g[4], g[3], g[0], g[1], g[2]);
  if (!lam && !lom)
  {
    act_kv (a, "Position", "no GPS fix");
    act_sum (a, "Location report without a GPS fix");
    return;
  }
  if (lam % 1000000 >= 600000 || lom % 1000000 >= 600000 || lam / 1000000 > 90 || lom / 1000000 > 180)
  {
    act_kv (a, "Position", "not decodable (%08X %08X)", la, lo);
    act_sum (a, "Location report (position not decodable)");
    return;
  }
  int south = !(la >> 31), west = lo >> 31;
  double lat = (lam / 1000000) + (lam % 1000000) / 600000.0;
  double lon = (lom / 1000000) + (lom % 1000000) / 600000.0;
  if (south) lat = -lat;
  if (west) lon = -lon;
  act_kv (a, "Position", "%.6f, %.6f (%u°%07.4f'%c %u°%07.4f'%c)", lat, lon, lam / 1000000, (lam % 1000000) / 10000.0,
          south ? 'S' : 'N', lom / 1000000, (lom % 1000000) / 10000.0, west ? 'W' : 'E');
  act_kv (a, "Speed", "%.1f km/h (%.1f kn)", spd * 0.1852, spd * 0.1);
  if (dir < 360 && spd > 0) act_kv (a, "Heading", "%d°", dir);
  act_pos (a, lat, lon);
  act_sum (a, "Position %.5f, %.5f, %.0f km/h", lat, lon, spd * 0.1852);
}

//does an 18 byte GPS block hold a sane time, date and coordinates (or an empty fix)?
static int hyt_gps_ok (const uint8_t * g)
{
  if (g[0] > 23 || g[1] > 59 || g[2] > 59 || g[3] < 1 || g[3] > 31 || g[4] < 1 || g[4] > 12 || g[5] > 99) return 0;
  uint32_t lam = le32 (g + 6) & 0x7FFFFFFF, lom = le32 (g + 10) & 0x7FFFFFFF;
  if (!lam && !lom) return 1;
  return lam % 1000000 < 600000 && lom % 1000000 < 600000 && lam / 1000000 <= 90 && lom / 1000000 <= 180;
}

static const char * hyt_lp_name (int op)
{
  switch (op)
  {
    case 0xA001: return "Location request";
    case 0xA002: return "Location answer";
    case 0xA003: return "Location answer with RSSI";
    case 0xA004: return "Location answer (GPS / Bluetooth)";
    case 0xB001: return "Emergency report stop request";
    case 0xB002: return "Emergency report stop answer";
    case 0xB003: return "Emergency location report";
    case 0xC001: return "Triggered report request";
    case 0xC002: return "Triggered report answer";
    case 0xC003: return "Triggered location report";
    case 0xC004: return "Triggered report stop request";
    case 0xC005: return "Triggered report stop answer";
    case 0xD001: return "Condition report request";
    case 0xD002: return "Condition report answer";
    case 0xD003: return "Condition location report";
    case 0xD004: return "Condition location report with RSSI";
    case 0xD011: return "Quick GPS request";
    case 0xD012: return "Quick GPS answer";
  }
  return NULL;
}

//Hytera Location Protocol as radios send it behind an ETSI compressed UDP header (seen with
//port index 116): 0x01, the LP opcode, the LP fields without the radio IP, and the GPS fix
//packed into 18 bytes instead of ASCII
static int dec_hytera_clp (act_item * a, const uint8_t * p, int len)
{
  if (len < 7 || p[0] != 0x01) return 0;
  int op = (int)be16 (p + 1);
  const char * name = hyt_lp_name (op);
  if (!name) return 0;
  act_type (a, "GPS");
  act_kv (a, "Hytera location", "%s (0x%04X, compressed)", name, op);
  int off = 3;
  act_kv (a, "Request id", "%u", be32 (p + off));
  //a resend carries a new RSSI: the request id and fix time make it the same report
  snprintf (a->key, sizeof(a->key), "hlp:%04X:%u:%02X%02X%02X", op, be32 (p + off), len > off + 12 ? p[off + 6] : 0,
            len > off + 13 ? p[off + 7] : 0, len > off + 14 ? p[off + 8] : 0);
  off += 4;
  int has_result = op == 0xA002 || op == 0xA003 || op == 0xD004;
  int has_rssi = op == 0xA003 || op == 0xD004;
  if (op == 0xC003) off += 8; //time remaining
  if (op == 0xB003) off += 1; //emergency type
  if (has_result)
  {
    if (off + 2 > len) { act_sum (a, "%s", name); return 1; }
    int r = (int)be16 (p + off);
    if (r) act_kv (a, "Result", "%s (%d)", r == 6 ? "position method failure" : (r == 0x69 ? "request format error" : "failed"), r);
    off += 2;
  }
  int is_report = op == 0xA002 || op == 0xA003 || op == 0xB003 || op == 0xC003 || op == 0xD003 || op == 0xD004;
  if (!is_report)
  {
    act_sum (a, "%s", name);
    return 1;
  }
  //the GPS block where the opcode puts it, else close by (a variant with other optional fields)
  int g = -1;
  for (int d = 0; d <= 10 && g < 0; d++)
    for (int sgn = 0; sgn < 2 && g < 0; sgn++)
    {
      int o = off + (sgn ? -d : d);
      if (o >= 3 && o + 18 <= len && hyt_gps_ok (p + o)) g = o;
    }
  if (g < 0)
  {
    act_sum (a, "%s without a position (%d bytes)", name, len);
    return 1;
  }
  off = g;
  hyt_gps_bin (a, p + off);
  off += 18;
  if (has_rssi && off + 2 <= len)
  {
    int rssi = (int16_t)be16 (p + off);
    if (rssi < -150 || rssi > 0) act_kv (a, "RSSI", "not measured (%d)", rssi); //seen as -231
    else
    {
      act_kv (a, "RSSI", "%d dBm", rssi);
      size_t l = strlen (a->sum);
      snprintf (a->sum + l, sizeof(a->sum) - l, ", RSSI %d dBm", rssi);
    }
  }
  return 1;
}

//ASCII GPS block of the uncompressed Hytera location messages
static void hyt_gps_ascii (act_item * a, const uint8_t * g)
{
  char s[42];
  memcpy (s, g, 41);
  s[41] = 0;
  //A HHMMSS DDMMYY N DDMM.MMMM E DDDMM.MMMM spd dir
  if (s[0] != 'A')
  {
    act_kv (a, "Position", "no GPS fix");
    act_sum (a, "Location report without a GPS fix");
    return;
  }
  char lat_s[10], lon_s[11], spd_s[4], dir_s[4];
  memcpy (lat_s, s + 14, 9); lat_s[9] = 0;
  memcpy (lon_s, s + 24, 10); lon_s[10] = 0;
  memcpy (spd_s, s + 34, 3); spd_s[3] = 0;
  memcpy (dir_s, s + 37, 3); dir_s[3] = 0;
  double la = atof (lat_s), lo = atof (lon_s);
  double lat = (int)(la / 100) + fmod (la, 100.0) / 60.0, lon = (int)(lo / 100) + fmod (lo, 100.0) / 60.0;
  if (s[13] == 'S') lat = -lat;
  if (s[23] == 'W') lon = -lon;
  act_kv (a, "Fix time", "20%.2s-%.2s-%.2s %.2s:%.2s:%.2s UTC", s + 11, s + 9, s + 7, s + 1, s + 3, s + 5);
  act_kv (a, "Position", "%.6f, %.6f", lat, lon);
  act_kv (a, "Speed", "%.1f km/h", atof (spd_s) * 1.852);
  act_kv (a, "Heading", "%d°", atoi (dir_s));
  act_pos (a, lat, lon);
  act_sum (a, "Position %.5f, %.5f, %.0f km/h", lat, lon, atof (spd_s) * 1.852);
}

//Hytera DMR application protocol: header, opcode, length, payload, checksum, 0x03
static int dec_hytera_hdap (act_item * a, const uint8_t * p, int len)
{
  if (len < 7) return 0;
  int hdr = p[0] & 0x7F;
  int plen = (int)be16 (p + 3);
  if (5 + plen + 2 > len || p[5 + plen + 1] != 0x03) return 0;
  int op = (int)be16 (p + 1);
  const uint8_t * d = p + 5;
  if (hdr == 0x08)
  {
    const char * name = hyt_lp_name (op);
    act_type (a, "GPS");
    act_kv (a, "Hytera location", "%s (0x%04X)", name ? name : "unknown", op);
    int g = -1;
    if ((op == 0xA002 || op == 0xA003 || op == 0xD004) && plen >= 10 + 41) g = 10;
    else if (op == 0xD003 && plen >= 8 + 41) g = 8;
    else if (op == 0xB003 && plen >= 5 + 41) g = 5;
    if (g >= 0)
    {
      act_kv (a, "Radio IP", "%d.%d.%d.%d", d[g == 5 ? 0 : 4], d[g == 5 ? 1 : 5], d[g == 5 ? 2 : 6], d[g == 5 ? 3 : 7]);
      hyt_gps_ascii (a, d + g);
    }
    else act_sum (a, "%s", name ? name : "Hytera location message");
    return 1;
  }
  if (hdr == 0x09 && plen >= 12)
  {
    int svc = op & 0xFF, opt = (op >> 14) & 1;
    act_type (a, "Text");
    int off = 4 + (opt ? 2 : 0) + 4 + 4 + 4;
    if ((svc == 0xA1 || svc == 0xB1) && off <= plen)
    {
      char text[700];
      text16 (text, sizeof(text), d + off, plen - off, 1);
      act_kv (a, "Text", "%s", text);
      act_sum (a, "\"%s\"", text);
    }
    else act_sum (a, "%s", svc == 0xA2 || svc == 0xB2 ? "Text message acknowledgement" : "Hytera text message");
    return 1;
  }
  if (hdr == 0x11)
  {
    static const struct { int t; const char * n; } rrs[] = { { 0x03, "Registration" }, { 0x80, "Registration answer" },
      { 0x01, "Deregistration" }, { 0x02, "Online check" }, { 0x82, "Online check answer" } };
    const char * n = "Radio registration message";
    for (size_t i = 0; i < sizeof(rrs) / sizeof(rrs[0]); i++) if (rrs[i].t == (op & 0xFF)) n = rrs[i].n;
    act_type (a, "Registration");
    if (plen >= 4) act_kv (a, "Radio IP", "%d.%d.%d.%d", d[0], d[1], d[2], d[3]);
    act_sum (a, "%s (Hytera)", n);
    return 1;
  }
  act_type (a, "Hytera");
  act_sum (a, "Hytera application message (protocol 0x%02X, opcode 0x%04X)", hdr, op);
  return 1;
}

static int moto_port_id (int id)
{
  switch (id)
  {
    case 1: return 4001;
    case 3: return 4005;
    case 4: return 4007;
    case 5: return 4008;
  }
  return 0;
}

//port ids of the ETSI UDP/IP header compression: 1 and 2 are defined, newer Motorola radios
//put their own ids 94 higher
static int etsi_port_id (int id)
{
  if (id == 1) return 5016;
  if (id == 2) return 5017;
  if (id > 94) return moto_port_id (id - 94);
  return 0;
}

static const char * port_name (int port)
{
  switch (port)
  {
    case 231: return "Cellocator";
    case 4001: return "LRRP location";
    case 4004: return "XCMP";
    case 4005: return "ARS registration";
    case 4007: return "TMS text";
    case 4008: return "Telemetry";
    case 4009: return "OTAP programming";
    case 4012: return "Battery management";
    case 4013: return "Job tickets";
    case 4069: return "TRBOnet SCADA";
    case 5016: return "ETSI text";
    case 5017: return "LIP location";
    case 9361: return "Atlas registration";
    case 49198: return "P25 location";
  }
  return NULL;
}

static void dec_udp_app (act_item * a, int sport, int dport, const uint8_t * p, int len, int depth)
{
  UNUSED(depth);
  if (len < 0) len = 0;
  int port = port_name (dport) ? dport : (port_name (sport) ? sport : dport);
  switch (port)
  {
    case 4001: case 49198: dec_lrrp (a, p, len); return;
    case 4005: dec_ars (a, p, len); return;
    case 4007: dec_tms (a, p, len); return;
    case 5017: dec_lip (a, p, len); return;
    case 5016:
    {
      char text[700];
      text16 (text, sizeof(text), p, len, 0);
      act_type (a, "Text");
      act_kv (a, "Text", "%s", text);
      act_sum (a, "\"%s\"", text);
      return;
    }
  }
  if (dec_hytera_clp (a, p, len) || dec_hytera_hdap (a, p, len)) return;
  const char * pn = port_name (port);
  set_type (a, pn ? pn : "UDP");
  if (len > 0 && printable_pct (p, len) >= 85)
  {
    char text[700];
    text8 (text, sizeof(text), p, len);
    act_kv (a, "Text", "%s", text);
    act_sum (a, "%s: \"%s\"", pn ? pn : "UDP data", text);
  }
  else if (pn) act_sum (a, "%s message, %d bytes", pn, len);
  else act_sum (a, "UDP data to port %d, %d bytes", dport, len);
}

static void ip_label (char * out, size_t n, const uint8_t * ip)
{
  //MOTOTRBO addressing: 12.x.x.x radios, 13.x.x.x servers and control stations, 225.x.x.x talkgroups
  uint32_t id = be24 (ip + 1);
  if (ip[0] == 12) snprintf (out, n, "%d.%d.%d.%d (radio %u)", ip[0], ip[1], ip[2], ip[3], id);
  else if (ip[0] == 13) snprintf (out, n, "%d.%d.%d.%d (server %u)", ip[0], ip[1], ip[2], ip[3], id);
  else if (ip[0] == 225) snprintf (out, n, "%d.%d.%d.%d (group %u)", ip[0], ip[1], ip[2], ip[3], id);
  else snprintf (out, n, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

static void dec_ipv4 (act_item * a, const uint8_t * p, int len, int depth)
{
  if (len < 20 || (p[0] >> 4) != 4)
  {
    set_type (a, "Packet data");
    if (!a->sum[0]) act_sum (a, "Packet data (not an IPv4 datagram), %d bytes", len);
    return;
  }
  int ihl = (p[0] & 0xF) * 4;
  if (ihl < 20 || ihl > len) ihl = 20;
  int tlen = (int)be16 (p + 2);
  if (tlen < ihl || tlen > len) tlen = len;
  int proto = p[9];
  char s[48], d[48];
  ip_label (s, sizeof(s), p + 12);
  ip_label (d, sizeof(d), p + 16);
  act_kv (a, depth ? "Returned packet" : "IP", "%s → %s", s, d);
  if (proto == 17 && tlen >= ihl + 8)
  {
    int sport = (int)be16 (p + ihl), dport = (int)be16 (p + ihl + 2);
    int plen = tlen - ihl - 8;
    int ulen = (int)be16 (p + ihl + 4);
    if (ulen >= 8 && ulen - 8 < plen) plen = ulen - 8;
    const char * pn = port_name (dport) ? port_name (dport) : port_name (sport);
    act_kv (a, depth ? "Returned UDP" : "UDP", "port %d → %d%s%s%s", sport, dport, pn ? " (" : "", pn ? pn : "", pn ? ")" : "");
    if (depth) return;
    dec_udp_app (a, sport, dport, p + ihl + 8, plen, depth);
  }
  else if (proto == 1 && tlen >= ihl + 4)
  {
    static const char * unreach[] = { "network", "host", "protocol", "port" };
    int type = p[ihl], code = p[ihl + 1];
    char what[80];
    if (type == 3) snprintf (what, sizeof(what), "Destination %s unreachable", code < 4 ? unreach[code] : "");
    else if (type == 0) snprintf (what, sizeof(what), "Echo reply");
    else if (type == 8) snprintf (what, sizeof(what), "Echo request");
    else if (type == 11) snprintf (what, sizeof(what), "Time exceeded");
    else snprintf (what, sizeof(what), "ICMP type %d code %d", type, code);
    if (depth) return;
    act_type (a, "ICMP");
    act_kv (a, "ICMP", "%s", what);
    //the undeliverable packet comes back attached
    if (type == 3 && tlen >= ihl + 8 + 20) dec_ipv4 (a, p + ihl + 8, tlen - ihl - 8, 1);
    act_sum (a, "%s", what);
  }
  else if (proto == 6 && tlen >= ihl + 4)
  {
    set_type (a, "TCP");
    act_kv (a, "TCP", "port %u → %u", be16 (p + ihl), be16 (p + ihl + 2));
    if (!a->sum[0]) act_sum (a, "TCP segment, port %u → %u", be16 (p + ihl), be16 (p + ihl + 2));
  }
  else
  {
    set_type (a, "IP");
    if (!a->sum[0]) act_sum (a, "IP protocol %d datagram", proto);
  }
}

static const char * aid_name_etsi (int id)
{
  switch (id)
  {
    case 0: return "radio network";
    case 1: return "USB / Ethernet";
    case 2: return "group network";
  }
  return id >= 11 ? "vendor" : "reserved";
}

//ETSI UDP/IP header compression (SAP 3)
static void dec_udp_etsi (act_item * a, const uint8_t * p, int len)
{
  if (len < 5) { act_type (a, "Packet data"); act_sum (a, "Compressed UDP/IP (truncated)"); return; }
  int said = p[2] >> 4, daid = p[2] & 0xF;
  int spid = p[3] & 0x7F, dpid = p[4] & 0x7F;
  int off = 5;
  int sport = spid ? etsi_port_id (spid) : 0, dport = dpid ? etsi_port_id (dpid) : 0;
  if (!spid && off + 2 <= len) { sport = (int)be16 (p + off); off += 2; }
  if (!dpid && off + 2 <= len) { dport = (int)be16 (p + off); off += 2; }
  act_kv (a, "Compressed UDP/IP", "IP id %u, %s → %s", be16 (p), aid_name_etsi (said), aid_name_etsi (daid));
  char sp[32], dp[32];
  if (spid) snprintf (sp, sizeof(sp), sport ? "%d (id %d)" : "id %d", sport ? sport : spid, spid);
  else snprintf (sp, sizeof(sp), "%d", sport);
  if (dpid) snprintf (dp, sizeof(dp), dport ? "%d (id %d)" : "id %d", dport ? dport : dpid, dpid);
  else snprintf (dp, sizeof(dp), "%d", dport);
  act_kv (a, "Ports", "%s → %s", sp, dp);
  dec_udp_app (a, sport, dport, p + off, len - off, 0);
}

//Motorola UDP/IP header compression in a proprietary header (SAP 1, MFID 10, opcode 2):
//2 bit address ids and 4 bit port ids, then the IP identification
static void dec_udp_moto (act_item * a, const uint8_t * p, int len)
{
  static const char * aid[] = { "radio", "server", "group", "id 3" };
  if (len < 4) { act_type (a, "Packet data"); act_sum (a, "Compressed UDP/IP (truncated)"); return; }
  int said = (p[0] >> 2) & 3, daid = p[0] & 3;
  int spid = p[1] >> 4, dpid = p[1] & 0xF;
  int off = 4;
  int sport = moto_port_id (spid), dport = moto_port_id (dpid);
  if (!spid && off + 2 <= len) { sport = (int)be16 (p + off); off += 2; }
  if (!dpid && off + 2 <= len) { dport = (int)be16 (p + off); off += 2; }
  act_kv (a, "Compressed UDP/IP (Motorola)", "IP id %u, %s → %s, port %d → %d", be16 (p + 2), aid[said], aid[daid], sport, dport);
  dec_udp_app (a, sport, dport, p + off, len - off, 0);
}

//Motorola LOCN / NMEA style ASCII positions in short data
static int dec_locn (act_item * a, const char * s)
{
  const char * n = NULL, * e = NULL;
  for (const char * q = s; *q; q++)
  {
    if ((*q == 'N' || *q == 'S') && !n && isdigit ((unsigned char)q[1]) && isdigit ((unsigned char)q[4])) n = q;
    else if ((*q == 'E' || *q == 'W') && !e && isdigit ((unsigned char)q[1]) && isdigit ((unsigned char)q[5])) e = q;
  }
  if (!n || !e) return 0;
  double la = atof (n + 1), lo = atof (e + 1);
  double lat = (int)(la / 100) + fmod (la, 100.0) / 60.0, lon = (int)(lo / 100) + fmod (lo, 100.0) / 60.0;
  if (*n == 'S') lat = -lat;
  if (*e == 'W') lon = -lon;
  if (fabs (lat) > 90 || fabs (lon) > 180) return 0;
  act_type (a, "GPS");
  act_kv (a, "Position", "%.6f, %.6f", lat, lon);
  act_pos (a, lat, lon);
  act_sum (a, "Position %.5f, %.5f", lat, lon);
  return 1;
}

static void dec_short_data (act_item * a, const uint8_t * p, int len, int ddfmt)
{
  static const char * fmts[] = { "binary", "BCD", "7-bit text", "ISO 8859-1", "ISO 8859-2", "ISO 8859-3", "ISO 8859-4",
    "ISO 8859-5", "ISO 8859-6", "ISO 8859-7", "ISO 8859-8", "ISO 8859-9", "ISO 8859-10", "ISO 8859-11", "ISO 8859-13",
    "ISO 8859-14", "ISO 8859-15", "ISO 8859-16", "UTF-8", "UTF-16", "UTF-16BE", "UTF-16LE", "UTF-32", "UTF-32BE", "UTF-32LE" };
  char text[700] = "";
  //the format field is often set for binary payloads, so text has to read like text too
  if (ddfmt >= 0x13 && ddfmt <= 0x14 && text16_pct (p, len, 0) >= 80) text16 (text, sizeof(text), p, len, 0);
  else if (ddfmt == 0x15 && text16_pct (p, len, 1) >= 80) text16 (text, sizeof(text), p, len, 1);
  else if (printable_pct (p, len) >= 80) text8 (text, sizeof(text), p, len);
  if (ddfmt >= 0) act_kv (a, "Short data", "defined, format %s", ddfmt < 25 ? fmts[ddfmt] : "reserved");
  else act_kv (a, "Short data", "raw");
  if (text[0])
  {
    act_kv (a, "Text", "%s", text);
    if (dec_locn (a, text)) return;
    act_type (a, "Text");
    act_sum (a, "\"%s\"", text);
  }
  else
  {
    act_type (a, "Short data");
    act_sum (a, "Short data (binary), %d bytes", len);
  }
}

//----------------------------------------------------------------------------
// DMR packet data (taps in dmr_block.c)
//----------------------------------------------------------------------------

static const char * dmr_sap_name (int sap)
{
  switch (sap)
  {
    case 0: return "UDT";
    case 1: return "Proprietary";
    case 2: return "TCP/IP compressed";
    case 3: return "UDP/IP compressed";
    case 4: return "IP";
    case 5: return "ARP";
    case 9: return "Proprietary";
    case 10: return "Short data";
  }
  return "Reserved";
}

static const char * mfid_name (int mfid)
{
  switch (mfid)
  {
    case 0x00: return "standard";
    case 0x06: return "Trident";
    case 0x08: case 0x68: return "Hytera";
    case 0x10: return "Motorola";
    case 0x58: return "Tait";
    case 0x77: return "Vertex Standard";
  }
  return "vendor";
}

//the DMR CRC-32 of a packet (octet pairs swapped, as the DMR spec orders them) over p[0, n)
static uint32_t dmr_crc32 (const uint8_t * p, int n)
{
  static uint8_t bits[24 * 128 * 8];
  int k = 0;
  if (n < 0) n = 0;
  if (n > 24 * 127) n = 24 * 127;
  for (int i = 0; i < n; i += 2)
  {
    uint8_t hi = i + 1 < n ? p[i + 1] : 0, lo = p[i];
    for (int b = 7; b >= 0; b--) bits[k++] = (hi >> b) & 1;
    for (int b = 7; b >= 0; b--) bits[k++] = (lo >> b) & 1;
  }
  return ComputeCrc32Bit (bits, (uint32_t)k);
}

//Basic Privacy key for a Motorola compressed packet header (MNIS), from what its first bytes must
//be once deciphered: address ids, known port ids, and the length of the application message.
//Returns the key number, 0 when no key fits, -1 when more than one does.
static int bp_find_mnis (const uint8_t * c, int len)
{
  int found = 0;
  if (len < 6) return 0;
  for (int k = 1; k < 256; k++)
  {
    uint8_t k0 = BPK[k] >> 8, k1 = BPK[k] & 0xFF;
    uint8_t d0 = c[0] ^ k0, d1 = c[1] ^ k1;
    if ((d0 >> 4) || ((d0 >> 2) & 3) > 2 || (d0 & 3) > 2) continue;
    int sp = d1 >> 4, dp = d1 & 0xF;
    if (!moto_port_id (sp) || !moto_port_id (dp)) continue; //ports in the header, not extended
    int app = 4, alen = len - app;
    uint8_t m0 = c[app] ^ k0, m1 = c[app + 1] ^ k1; //keystream alternates with the byte position
    if (dp == 3 || dp == 4) //ARS / TMS: a 16 bit length leads the message
    {
      int l = (m0 << 8) | m1;
      if (l + 2 != alen && l + 2 != alen - 1) continue;
    }
    else if (dp == 1) //LRRP: type byte then a length byte
    {
      uint8_t t1 = c[app + 1] ^ k1;
      if (t1 + 2 != alen && t1 + 2 != alen - 1) continue;
    }
    else continue;
    if (found) return -1;
    found = k;
  }
  return found;
}

void web_dmr_dheader (dsd_opts * opts, dsd_state * state, uint8_t * dheader)
{
  UNUSED(opts);
  if (!A.inited) return;
  int slot = state->currentslot & 1;
  int dpf = dheader[0] & 0xF;
  if (dpf == 15)
  {
    if (A.hdr[slot].nprop < 3) memcpy (A.hdr[slot].prop[A.hdr[slot].nprop++], dheader, 10);
    return;
  }
  A.hdr[slot].dpf = dpf;
  A.hdr[slot].gi = (dheader[0] >> 7) & 1; //set = group
  A.hdr[slot].a = (dheader[0] >> 6) & 1;
  A.hdr[slot].sap = dheader[1] >> 4;
  A.hdr[slot].ddfmt = dpf == 13 ? (dheader[8] >> 2) & 0x3F : -1;
  A.hdr[slot].bp = (dpf == 13 || dpf == 14) ? dheader[9] : 0; //short data: trailing pad bits
  A.hdr[slot].nprop = 0;
  if (dpf != 1) return;

  //response packet: the receiving end acknowledges a confirmed packet
  int cls = dheader[9] >> 6, type = (dheader[9] >> 3) & 7, status = dheader[9] & 7;
  static const char * nack[] = { "illegal format", "packet CRC error", "memory full", "fragment out of sequence",
    "undeliverable", "packet out of sequence", "invalid user", "reason 7" };
  long long from = state->dmr_lrrp_source[slot], to = state->dmr_lrrp_target[slot];
  double now = wall_now ();
  recent_item * best = NULL;
  for (int i = 0; i < RECENT_MAX; i++)
  {
    recent_item * r = &A.recent[i];
    if (!r->on || r->slot != slot || r->a.src != to || r->a.dst != from || now - r->a.t_end > 10.0) continue;
    if (!best || r->a.t_end > best->a.t_end) best = r;
  }
  if (best)
  {
    if (cls == 0 && type == 1) snprintf (best->a.ack, sizeof(best->a.ack), "delivered");
    else if (cls == 1) snprintf (best->a.ack, sizeof(best->a.ack), "not delivered: %s", nack[type]);
    else if (cls == 2) snprintf (best->a.ack, sizeof(best->a.ack), "partly delivered");
    else snprintf (best->a.ack, sizeof(best->a.ack), "response %d/%d", cls, type);
    act_publish (&best->a);
    return;
  }
  act_item a;
  act_new (&a, "sig", "DMR", (state->lastsynctype >= 10 && state->lastsynctype <= 13) ? slot + 1 : 0);
  a.src = state->dmr_lrrp_source[slot];
  a.dst = state->dmr_lrrp_target[slot];
  a.gi = A.hdr[slot].gi ? 0 : 1;
  if (cls == 0 && type == 1) { act_type (&a, "ACK"); act_sum (&a, "Data delivered (acknowledged)"); }
  else if (cls == 1) { act_type (&a, "NACK"); act_sum (&a, "Data not delivered: %s", nack[type]); }
  else if (cls == 2) { act_type (&a, "SACK"); act_sum (&a, "Data partly delivered, blocks to resend"); }
  else { act_type (&a, "Response"); act_sum (&a, "Data response class %d type %d", cls, type); }
  act_kv (&a, "Response", "class %d, type %d, status %d", cls, type, status);
  act_emit (&a);
}

static uint32_t fnv32 (uint32_t h, const uint8_t * p, int n)
{
  for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

//how complete a packet item is: CRC ok, then readable
static int act_score (const act_item * a)
{
  return (a->crc == 1 ? 2 : 0) + (strcmp (a->type, "Encrypted data") && strcmp (a->type, "Data error") ? 1 : 0);
}

//the decoder's console summary only adds something when the packet was not understood here
static int act_generic (const act_item * a)
{
  return !strcmp (a->type, "Data") || !strcmp (a->type, "Data error") || !strcmp (a->type, "UDP") ||
         !strcmp (a->type, "Packet data") || !strcmp (a->type, "Short data") || !strcmp (a->type, "Encrypted data");
}

//before the assembler deciphers anything: keep the packet as received and check its CRC
//(the assembler's own check does not cover Motorola proprietary packets correctly)
void web_dmr_pdu_raw (dsd_opts * opts, dsd_state * state, int crc_ok)
{
  UNUSED(opts);
  if (!A.inited) return;
  int slot = state->currentslot & 1;
  int ctr = state->data_byte_ctr[slot];
  if (ctr < 4 || ctr > 24 * 127) { A.raw_valid[slot] = 0; return; }
  memcpy (A.raw[slot], state->dmr_pdu_sf[slot], (size_t)ctr);
  const uint8_t * buf = A.raw[slot];
  if (state->data_header_sap[slot] == 1 && buf[0] == 0x1F && buf[1] == 0x10 && ctr >= 12)
  {
    uint32_t want = be32 (buf + ctr - 4);
    crc_ok = dmr_crc32 (buf + 3, ctr - 7) == want || dmr_crc32 (buf + 3, ctr - 8) == want || dmr_crc32 (buf + 3, ctr - 9) == want;
  }
  A.raw_len[slot] = ctr;
  A.raw_crc[slot] = crc_ok;
  A.raw_valid[slot] = 1;
}

void web_dmr_pdu (dsd_opts * opts, dsd_state * state, int undecrypted)
{
  UNUSED(opts);
  if (!A.inited) return;
  int slot = state->currentslot & 1;
  if (!A.raw_valid[slot]) return;
  A.raw_valid[slot] = 0;
  int ctr = A.raw_len[slot];
  int crc_ok = A.raw_crc[slot];
  int poc = state->data_block_poc[slot];
  int sap = state->data_header_sap[slot];
  //deciphered by the assembler when it had the key, otherwise still as received
  static uint8_t buf[24 * 128];
  memcpy (buf, undecrypted ? A.raw[slot] : state->dmr_pdu_sf[slot], (size_t)ctr);
  //Motorola proprietary header (SAP 1, DPF 15, MFID 10) stored ahead of the data
  int mnis = sap == 1 && buf[0] == 0x1F && buf[1] == 0x10;
  int start = mnis ? 3 : 0; //after the proprietary header's SAP, MFID and opcode
  if (ctr < start + 4) return;
  //short data headers carry appended blocks where the others carry pad octets, and pad bits instead
  if (A.hdr[slot].dpf == 13 || A.hdr[slot].dpf == 14) poc = A.hdr[slot].bp / 8;
  if (poc < 0 || poc > ctr - start - 4) poc = 0;
  int end = ctr - 4 - poc;

  act_item na, * a = &na;
  act_new (a, "data", "DMR", (state->lastsynctype >= 10 && state->lastsynctype <= 13) ? slot + 1 : 0);
  a->src = state->dmr_lrrp_source[slot];
  a->dst = state->dmr_lrrp_target[slot];
  a->gi = A.hdr[slot].gi ? 0 : 1;
  a->crc = crc_ok ? 1 : 0;
  a->rep = 1;

  const char * dpf_name = A.hdr[slot].dpf == 3 ? "confirmed" : (A.hdr[slot].dpf == 2 ? "unconfirmed" : (A.hdr[slot].dpf >= 13 ? "short data" : "packet"));
  act_kv (a, "Delivery", "%s%s, SAP %d (%s)", dpf_name, A.hdr[slot].a ? ", answer requested" : "", sap, dmr_sap_name (sap));
  for (int i = 0; i < A.hdr[slot].nprop; i++)
  {
    const uint8_t * h = A.hdr[slot].prop[i];
    act_kv (a, "Vendor header", "%s (MFID %02X), SAP %d: %02X%02X%02X%02X%02X%02X%02X%02X", mfid_name (h[1]), h[1], h[0] >> 4,
            h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9]);
  }

  int alg = slot ? state->payload_algidR : state->payload_algid;
  int kid = slot ? state->payload_keyidR : state->payload_keyid;
  int enc = (slot ? state->dmr_soR : state->dmr_so) == 0x100;
  int bp_key = 0;
  const char * enc_how = NULL;
  if (enc && alg == 0x07) snprintf (a->enc, sizeof(a->enc), "Vertex Standard");
  else if (enc && alg == 0) snprintf (a->enc, sizeof(a->enc), "Basic Privacy");
  else if (enc) snprintf (a->enc, sizeof(a->enc), "%s", alg_name (alg) ? alg_name (alg) : "encrypted");
  if (enc && !undecrypted) enc_how = "deciphered with the loaded key";

  //Basic Privacy on a Motorola compressed packet: the header gives the key away
  if (undecrypted && alg == 0 && mnis && crc_ok)
  {
    int ks = state->data_ks_start[slot] ? state->data_ks_start[slot] : 3;
    bp_key = bp_find_mnis (buf + ks, end - ks);
    if (bp_key > 0 && web_offer_bp_key (bp_key))
    {
      uint8_t k0 = BPK[bp_key] >> 8, k1 = BPK[bp_key] & 0xFF;
      for (int i = ks; i < end; i++) buf[i] ^= ((i - ks) & 1) ? k1 : k0;
      undecrypted = 0;
      enc_how = "deciphered, key found by Auto-find";
      act_kv (a, "Basic Privacy key", "%d (found from the packet header)", bp_key);
    }
  }

  if (enc_how) act_kv (a, "Encryption", "%s, %s", a->enc, enc_how);
  if (undecrypted)
  {
    act_type (a, "Encrypted data");
    act_kv (a, "Encryption", "%s, ALG %02X, key id %02X", a->enc, alg, kid);
    if (bp_key > 0)
      act_sum (a, "Encrypted data (%s); turn on Keys → Auto-find BP key to decode it", a->enc);
    else act_sum (a, "Encrypted data (%s), no key loaded", a->enc);
  }
  else if (!crc_ok)
  {
    act_type (a, "Data error");
    act_sum (a, "%s packet failed its CRC check (not decoded)", dmr_sap_name (sap));
  }
  else if (mnis && (buf[2] & 0xF) == 2) dec_udp_moto (a, buf + 3, end - 3);
  else if (sap == 4) dec_ipv4 (a, buf + start, end - start, 0);
  else if (sap == 3) dec_udp_etsi (a, buf + start, end - start);
  else if (sap == 10) dec_short_data (a, buf + start, end - start, A.hdr[slot].ddfmt);
  else
  {
    act_type (a, "Data");
    act_sum (a, "%s packet, %d bytes", dmr_sap_name (sap), end - start);
  }
  if (!a->type[0]) act_type (a, "Data");
  act_hex (a, buf + start, end - start);

  //a retry of a packet heard a moment ago updates that item (and replaces it when this copy reads
  //better); a copy that failed its CRC counts as an earlier try of the next one from the same radio
  uint32_t h = a->key[0] ? fnv32 (2166136261u, (const uint8_t *)a->key, (int)strlen (a->key)) : fnv32 (2166136261u, A.raw[slot] + start, end - start);
  h = fnv32 (h, (const uint8_t *)&a->src, sizeof(a->src));
  h = fnv32 (h, (const uint8_t *)&a->dst, sizeof(a->dst));
  double now = wall_now ();
  recent_item * r = NULL;
  for (int i = 0; i < RECENT_MAX; i++)
  {
    recent_item * x = &A.recent[i];
    if (!x->on || x->slot != slot || now - x->a.t_end >= REPEAT_WIN) continue;
    if (x->hash == h) r = x;
    else if (!r && x->a.src == a->src && x->a.dst == a->dst && (x->a.crc == 0 || a->crc == 0)) r = x;
  }
  if (r)
  {
    if (act_score (a) > act_score (&r->a))
    {
      a->id = r->a.id;
      a->t = r->a.t;
      a->rep = r->a.rep + 1;
      snprintf (a->ack, sizeof(a->ack), "%s", r->a.ack);
      act_free (&r->a);
      r->a = *a;
      A.claim[slot] = 1;
    }
    else
    {
      act_free (a);
      r->a.rep++;
      A.claim[slot] = 2;
    }
    r->a.t_end = now;
    act_publish (&r->a);
    A.claimed[slot] = &r->a;
    return;
  }
  r = &A.recent[A.recent_next];
  A.recent_next = (A.recent_next + 1) % RECENT_MAX;
  if (r->on) act_free (&r->a);
  r->on = 1;
  r->slot = slot;
  r->hash = h;
  r->a = *a;
  act_publish (&r->a);
  //the decoder logs this packet in a moment; keep its line with this item if it adds anything
  A.claim[slot] = act_generic (&r->a) ? 1 : 2;
  A.claimed[slot] = &r->a;
}

//----------------------------------------------------------------------------
// Data events logged by the decoders themselves (watchdog_event_datacall)
//----------------------------------------------------------------------------

void web_datacall (dsd_opts * opts, dsd_state * state, int slot, uint32_t src, uint32_t dst, const char * text)
{
  UNUSED(opts);
  if (!A.inited || !text) return;
  slot &= 1;
  //skip the date / time the event history puts first, the page shows its own
  const char * t = text;
  while (*t == ' ') t++;
  if (src == 0 && dst == 0) return; //start-up notices
  if (A.claim[slot])
  {
    if (A.claim[slot] == 1 && A.claimed[slot])
    {
      act_kv (A.claimed[slot], "Decoder", "%s", t);
      act_publish (A.claimed[slot]);
      A.claim[slot] = 2; //once (nested IP packets log twice)
    }
    return;
  }
  act_item a;
  int st = state->lastsynctype;
  int slotted = (st >= 10 && st <= 13) || st == 35 || st == 36;
  act_new (&a, (src == 0xFFFFFF && dst == 0xFFFFFF) ? "sys" : "data", sync_name (st), slotted ? slot + 1 : 0);
  if (src && src != 0xFFFFFF) a.src = src;
  if (dst && dst != 0xFFFFFF) a.dst = dst;
  act_type (&a, a.kind[0] == 's' ? "Event" : "Data");
  act_sum (&a, "%s", t);
  //P25 data responses name the radio only as an LLID in the text
  const char * ll = strstr (t, "LLID: ");
  if (ll && a.dst < 0) a.dst = atoll (ll + 6);
  if (strstr (t, "Data Call Response"))
  {
    a.kind = "sig";
    act_type (&a, strstr (t, "NACK") ? "NACK" : (strstr (t, "SACK") ? "SACK" : "ACK"));
  }
  double lat, lon;
  if (strstr (t, "LRRP") || strstr (t, "GPS") || strstr (t, "NMEA") || strstr (t, "LOCN") || strstr (t, "LIP"))
    if (parse_pos (t, &lat, &lon)) act_pos (&a, lat, lon);
  act_emit (&a);
}

//----------------------------------------------------------------------------
// P25 trunking signalling (tap in p25p1_tsbk.c), decoded from the TSBK
//----------------------------------------------------------------------------

//channel number to frequency, as process_channel_to_freq does but without the console output
static long long p25_freq (dsd_state * state, int ch)
{
  static const int slots[16] = { 1, 1, 1, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
  if (ch <= 0 || ch == 0xFFFF) return 0;
  if (state->trunk_chan_map[ch]) return state->trunk_chan_map[ch];
  int iden = (ch >> 12) & 0xF;
  if (!state->p25_base_freq[iden]) return 0;
  int step = (ch & 0xFFF) / slots[state->p25_chan_type[iden] & 0xF];
  return (long long)state->p25_base_freq[iden] * 5 + (long long)step * state->p25_chan_spac[iden] * 125;
}

static void ch_label (char * out, size_t n, dsd_state * state, int ch)
{
  long long f = p25_freq (state, ch);
  if (f) snprintf (out, n, "%.5f MHz (ch %04X)", f / 1e6, ch);
  else snprintf (out, n, "ch %04X", ch);
}

static void svc_text (char * out, size_t n, int svc)
{
  snprintf (out, n, "%s%s%s, priority %d", svc & 0x80 ? "emergency, " : "", svc & 0x40 ? "encrypted, " : "",
            svc & 0x10 ? "packet" : "circuit", svc & 7);
}

static void grant_end (grant_track * g)
{
  if (!g->on) return;
  g->a.live = 0;
  act_publish (&g->a);
  act_free (&g->a);
  g->on = 0;
}

static void grant_label (grant_track * g)
{
  act_item * a = &g->a;
  char ch[48];
  ch_label (ch, sizeof(ch), A.state, g->ch);
  char who[32] = "";
  if (a->src > 0) snprintf (who, sizeof(who), "%lld → ", a->src);
  static const char * what[] = { "TG", "radio", "radio", "phone call to radio" };
  act_sum (a, "%s%s%s %lld on %s", g->svc & 0x80 ? "EMERGENCY " : "", who, what[g->kind & 3], g->id, ch);
  jb_reset (&a->f);
  act_kv (a, "Channel", "%s", ch);
  char sv[96];
  svc_text (sv, sizeof(sv), g->svc);
  act_kv (a, "Service options", "%s", sv);
  if (A.p25_wacn) act_kv (a, "System", "WACN %05llX, SYS %03X, RFSS %d, site %d", A.p25_wacn, A.p25_sysid, A.p25_rfss, A.p25_site);
}

//a grant or grant update; the first one for a channel and id opens a trunked call item
static void p25_grant (int kind, int ch, long long id, long long src, int svc, int full)
{
  static const char * types[] = { "Group call grant", "Private call grant", "Data channel grant", "Phone call grant" };
  if (ch <= 0 || ch == 0xFFFF || id <= 0) return;
  double now = wall_now ();
  grant_track * g = NULL, * spare = NULL;
  for (int i = 0; i < GRANT_MAX; i++)
  {
    grant_track * x = &A.grant[i];
    if (x->on && x->kind == kind && x->id == id && x->ch == ch) { g = x; break; }
    if (!x->on && !spare) spare = x;
  }
  if (!g)
  {
    if (!spare)
    {
      spare = &A.grant[0];
      for (int i = 1; i < GRANT_MAX; i++) if (A.grant[i].a.t_end < spare->a.t_end) spare = &A.grant[i];
      grant_end (spare);
    }
    g = spare;
    act_new (&g->a, "call", "P25", 0);
    g->on = 1;
    g->kind = kind;
    g->ch = ch;
    g->id = id;
    g->svc = full ? svc : 0;
    g->a.live = 1;
    g->a.dst = id;
    g->a.src = src > 0 ? src : -1;
    g->a.gi = kind == 0 ? 0 : 1;
    g->a.freq = p25_freq (A.state, ch);
    act_type (&g->a, types[kind & 3]);
    if (g->svc & 0x40) snprintf (g->a.enc, sizeof(g->a.enc), "encrypted");
    grant_label (g);
    act_publish (&g->a);
    return;
  }
  g->a.t_end = now;
  int changed = 0;
  if (src > 0 && g->a.src != src) { g->a.src = src; changed = 1; }
  if (full && svc != g->svc) { g->svc = svc; changed = 1; snprintf (g->a.enc, sizeof(g->a.enc), "%s", svc & 0x40 ? "encrypted" : ""); }
  if (!g->a.freq && p25_freq (A.state, ch)) { g->a.freq = p25_freq (A.state, ch); changed = 1; }
  if (changed)
  {
    grant_label (g);
    act_publish (&g->a);
  }
}

static void grants_poll (void)
{
  double now = wall_now ();
  for (int i = 0; i < GRANT_MAX; i++)
    if (A.grant[i].on && now - A.grant[i].a.t_end > GRANT_HANG) grant_end (&A.grant[i]);
}

static void p25_sig (const char * type, long long src, long long dst, int gi, const char * fmt, ...) __attribute__((format(printf, 5, 6)));
static void p25_sig (const char * type, long long src, long long dst, int gi, const char * fmt, ...)
{
  act_item a;
  act_new (&a, "sig", "P25", 0);
  act_type (&a, type);
  //FFFFFx are the system's own addresses (FNE, registration default, all units), not radios
  if (src > 0 && src < 0xFFFFF0) a.src = src;
  if (dst > 0 && dst < 0xFFFFF0) a.dst = dst;
  a.gi = gi;
  va_list ap;
  va_start (ap, fmt);
  vsnprintf (a.sum, sizeof(a.sum), fmt, ap);
  va_end (ap);
  act_emit (&a);
}

static void p25_sys_note (const char * fmt, ...) __attribute__((format(printf, 1, 2)));
static void p25_sys_note (const char * fmt, ...)
{
  act_item a;
  act_new (&a, "sys", "P25", 0);
  act_type (&a, "System");
  va_list ap;
  va_start (ap, fmt);
  vsnprintf (a.sum, sizeof(a.sum), fmt, ap);
  va_end (ap);
  act_emit (&a);
}

static const char * p25_svc_type (int t)
{
  switch (t)
  {
    case 0x00: return "group voice"; case 0x02: return "group voice update"; case 0x04: return "unit to unit voice";
    case 0x05: return "unit to unit answer"; case 0x08: return "phone call"; case 0x14: return "data channel";
    case 0x18: return "status update"; case 0x1A: return "status query"; case 0x1C: return "message";
    case 0x1D: return "radio monitor"; case 0x1F: return "call alert"; case 0x24: return "extended function";
    case 0x28: return "affiliation"; case 0x2C: return "registration"; case 0x2F: return "deregistration";
  }
  return NULL;
}

static const char * p25_reason (int r)
{
  switch (r)
  {
    case 0x10: return "target not valid"; case 0x11: return "target not authorized"; case 0x17: return "target not registered";
    case 0x1F: return "requester not valid"; case 0x20: return "requester not authorized"; case 0x28: return "group not valid";
    case 0x29: return "group not authorized"; case 0x2F: return "group not registered"; case 0x30: return "talkgroup busy";
    case 0x40: return "resources busy"; case 0x60: return "site access denied"; case 0x61: return "system access denied";
    case 0xF0: return "no network connection";
  }
  return NULL;
}

static void p25_neighbor_seen (int rfss, int site, int sysid, int ch, int cfva, int ssc)
{
  double now = wall_now ();
  for (int i = 0; i < A.nnb; i++)
  {
    p25_neighbor * n = &A.nb[i];
    if (n->rfss == rfss && n->site == site && n->sysid == sysid)
    {
      n->ch = ch; n->cfva = cfva; n->ssc = ssc; n->t = now;
      return;
    }
  }
  if (A.nnb < P25_NB_MAX)
  {
    A.nb[A.nnb++] = (p25_neighbor){ rfss, site, sysid, ch, cfva, ssc, now };
    char cl[48];
    ch_label (cl, sizeof(cl), A.state, ch);
    p25_sys_note ("Neighbor site: RFSS %d, site %d (SYS %03X) on %s", rfss, site, sysid, cl);
  }
}

static void p25_site_check (void)
{
  //one note per new system / site / control channel
  if (!A.p25_wacn && !A.p25_rfss) return;
  if (A.p25_wacn == A.p25_logged_wacn && A.p25_sysid == A.p25_logged_sys && A.p25_rfss == A.p25_logged_rfss &&
      A.p25_site == A.p25_logged_site && A.p25_cc_ch == A.p25_logged_cc) return;
  if (!A.p25_wacn || !A.p25_rfss) return; //wait until both broadcasts were heard
  A.p25_logged_wacn = A.p25_wacn; A.p25_logged_sys = A.p25_sysid; A.p25_logged_rfss = A.p25_rfss;
  A.p25_logged_site = A.p25_site; A.p25_logged_cc = A.p25_cc_ch;
  char cl[48];
  ch_label (cl, sizeof(cl), A.state, A.p25_cc_ch);
  p25_sys_note ("P25 system WACN %05llX, SYS %03X, RFSS %d, site %d, control channel %s",
                A.p25_wacn, A.p25_sysid, A.p25_rfss, A.p25_site, cl);
}

static void p25_osp (dsd_state * state, int op, const uint8_t * b)
{
  char cl[48];
  switch (op)
  {
    case 0x00: p25_grant (0, (int)be16 (b + 1), be16 (b + 3), be24 (b + 5), b[0], 1); break;
    case 0x02:
    {
      int c1 = (int)be16 (b), c2 = (int)be16 (b + 4);
      p25_grant (0, c1, be16 (b + 2), -1, 0, 0);
      if (c2 != c1) p25_grant (0, c2, be16 (b + 6), -1, 0, 0);
      break;
    }
    case 0x03: p25_grant (0, (int)be16 (b + 2), be16 (b + 6), -1, b[0], 1); break;
    case 0x04: case 0x06: p25_grant (1, (int)be16 (b), be24 (b + 2), be24 (b + 5), 0, 0); break;
    case 0x05: p25_sig ("Call request", be24 (b + 5), be24 (b + 2), 1, "Private call request (answer awaited)"); break;
    case 0x08: case 0x09: p25_grant (3, (int)be16 (b + 1), be24 (b + 5), -1, b[0], 1); break;
    case 0x14: p25_grant (2, (int)be16 (b + 1), be24 (b + 5), -1, 0, 0); break;
    case 0x18: p25_sig ("Status", be24 (b + 5), be24 (b + 2), 1, "Status update: user %d, unit %d", b[0], b[1]); break;
    case 0x1A: p25_sig ("Status query", be24 (b + 5), be24 (b + 2), 1, "Status query"); break;
    case 0x1C: p25_sig ("Message", be24 (b + 5), be24 (b + 2), 1, "Short message %u", be16 (b)); break;
    case 0x1D: p25_sig ("Radio monitor", be24 (b + 5), be24 (b + 2), 1, "Radio monitor command"); break;
    case 0x1F: p25_sig ("Call alert", be24 (b + 5), be24 (b + 2), 1, "Call alert (page)"); break;
    case 0x20:
    {
      const char * s = p25_svc_type (b[0] & 0x3F);
      long long src = ((b[0] & 0x80) && !(b[0] & 0x40)) ? (long long)be24 (b + 2) : -1;
      p25_sig ("ACK", src, be24 (b + 5), 1, "Acknowledged%s%s", s ? ": " : "", s ? s : "");
      break;
    }
    case 0x21: case 0x27:
    {
      const char * s = p25_svc_type (b[0] & 0x3F), * r = p25_reason (b[1]);
      char rs[32];
      if (!r) { snprintf (rs, sizeof(rs), "reason %02X", b[1]); r = rs; }
      p25_sig (op == 0x27 ? "Denied" : "Queued", -1, be24 (b + 5), 1, "%s%s%s (%s)", op == 0x27 ? "Denied" : "Queued",
               s ? ": " : "", s ? s : "", r);
      break;
    }
    case 0x24:
    {
      int cls = b[0], oper = b[1];
      const char * w = (cls == 0 && oper == 0x00) ? "Radio check" : (cls == 0 && oper == 0x7F) ? "Radio inhibit" :
                       (cls == 0 && oper == 0x7E) ? "Radio uninhibit" : (cls == 0 && oper == 0x80) ? "Radio check answer" : "Extended function";
      p25_sig ("Radio command", be24 (b + 2), be24 (b + 5), 1, "%s", w);
      break;
    }
    case 0x28:
    {
      static const char * gav[] = { "accepted", "failed", "denied", "refused" };
      p25_sig ("Affiliation", be24 (b + 5), be16 (b + 3), 0, "Group affiliation %s%s", gav[b[0] & 3], b[0] & 0x80 ? " (local)" : "");
      break;
    }
    case 0x29: //secondary control channel, explicit
    case 0x39: //secondary control channels
    {
      int chs[2] = { (int)be16 (b + 2), op == 0x39 ? (int)be16 (b + 5) : 0 };
      for (int k = 0; k < 2; k++)
      {
        if (!chs[k] || chs[k] == 0xFFFF) continue;
        int known = 0;
        for (int i = 0; i < A.nsccb; i++) if (A.sccb[i] == chs[k]) known = 1;
        if (!known && A.nsccb < 4)
        {
          A.sccb[A.nsccb++] = chs[k];
          ch_label (cl, sizeof(cl), state, chs[k]);
          p25_sys_note ("Secondary control channel: %s (RFSS %d, site %d)", cl, b[0], b[1]);
        }
      }
      break;
    }
    case 0x2A: p25_sig ("Affiliation query", be24 (b + 5), be24 (b + 2), 1, "Group affiliation query"); break;
    case 0x2B:
    {
      static const char * rv[] = { "accepted", "failed", "denied", "refused" };
      p25_sig ("Location registration", -1, be24 (b + 5), 1, "Location registration %s, group %u, RFSS %d site %d",
               rv[b[0] & 3], be16 (b + 1), b[3], b[4]);
      break;
    }
    case 0x2C:
    {
      static const char * rv[] = { "accepted", "failed", "denied", "refused" };
      p25_sig ("Registration", be24 (b + 5), -1, 1, "Unit registration %s (SYS %03X)", rv[(b[0] >> 4) & 3], ((b[0] & 0xF) << 8) | b[1]);
      break;
    }
    case 0x2D: p25_sig ("Registration", -1, be24 (b + 5), 1, "Registration command (register again)"); break;
    case 0x2F: p25_sig ("Deregistration", be24 (b + 5), -1, 1, "Deregistration acknowledged"); break;
    case 0x33: case 0x34: case 0x3D: //channel identifiers: the decoder stores them, note new ones
    {
      int id = b[0] >> 4;
      if (A.iden_seen & (1u << id)) break;
      A.iden_seen |= 1u << id;
      long long base = (long long)be32 (b + 4) * 5;
      int spac = ((b[2] & 3) << 8) | b[3];
      //transmit offset, top bit set = above the receive frequency: 9 bits of 250 kHz steps after a
      //9 bit bandwidth in IDEN_UP, 14 bits of channel steps after a 4 bit field in the VU / TDMA forms
      double offm;
      if (op == 0x3D)
      {
        int off = ((b[1] & 7) << 6) | (b[2] >> 2);
        offm = ((off >> 8) & 1 ? 1 : -1) * (off & 0xFF) * 0.25;
      }
      else
      {
        int off = (b[1] << 6) | (b[2] >> 2);
        offm = ((off >> 13) & 1 ? 1 : -1) * (off & 0x1FFF) * spac * 0.000125;
      }
      p25_sys_note ("Channel identifier %d: base %.5f MHz, %.2f kHz steps, transmit offset %+.4f MHz%s", id, base / 1e6,
                    spac * 0.125, offm, op == 0x33 ? ", TDMA" : "");
      break;
    }
    case 0x35:
    {
      //date and time announcement (UTC); the decoder ignores it
      if (!(b[0] & 0x80)) break;
      int mon = (b[1] >> 4) & 0xF, day = ((b[1] & 0xF) << 1) | (b[2] >> 7), year = ((b[2] & 0x7F) << 6) | (b[3] >> 2);
      int hour = (b[4] >> 3) & 0x1F, min = ((b[4] & 7) << 3) | (b[5] >> 5), sec = ((b[5] & 0x1F) << 1) | (b[6] >> 7);
      snprintf (A.p25_time, sizeof(A.p25_time), "%04d-%02d-%02d %02d:%02d:%02d UTC", year, mon, day, hour, min, sec);
      break;
    }
    case 0x3A:
      A.p25_rfss = b[3]; A.p25_site = b[4]; A.p25_ssc = b[7];
      if (!A.p25_sysid) A.p25_sysid = ((b[1] & 0xF) << 8) | b[2];
      A.p25_t = wall_now ();
      p25_site_check ();
      break;
    case 0x3B:
      A.p25_wacn = ((long long)b[1] << 12) | (b[2] << 4) | (b[3] >> 4);
      A.p25_sysid = ((b[3] & 0xF) << 8) | b[4];
      A.p25_cc_ch = (int)be16 (b + 5);
      A.p25_t = wall_now ();
      p25_site_check ();
      break;
    case 0x3C: p25_neighbor_seen (b[3], b[4], ((b[1] & 0xF) << 8) | b[2], (int)be16 (b + 5), b[1] >> 4, b[7]); break;
  }
}

static void p25_moto (dsd_state * state, int op, const uint8_t * b)
{
  UNUSED(state);
  switch (op)
  {
    case 0x00: case 0x01:
    {
      char gl[64] = "";
      size_t l = 0;
      for (int i = 2; i <= 6; i += 2)
      {
        unsigned g = be16 (b + i);
        if (g && l < sizeof(gl) - 8) l += (size_t)snprintf (gl + l, sizeof(gl) - l, "%s%u", l ? ", " : "", g);
      }
      act_item a;
      act_new (&a, "sig", "P25", 0);
      act_type (&a, op ? "Patch removed" : "Patch");
      a.dst = be16 (b);
      a.gi = 0;
      act_sum (&a, "%s supergroup %u: %s", op ? "Removed" : "Patched", be16 (b), gl);
      act_emit (&a);
      break;
    }
    case 0x02: p25_grant (0, (int)be16 (b + 1), be16 (b + 3), be24 (b + 5), b[0], 1); break;
    case 0x03:
    {
      int c1 = (int)be16 (b), c2 = (int)be16 (b + 4);
      p25_grant (0, c1, be16 (b + 2), -1, 0, 0);
      if (c2 != c1) p25_grant (0, c2, be16 (b + 6), -1, 0, 0);
      break;
    }
  }
}

void web_p25_tsbk (dsd_opts * opts, dsd_state * state, uint8_t * tsbk)
{
  UNUSED(opts);
  if (!A.inited) return;
  if ((tsbk[0] >> 6) & 1) return; //protected
  int op = tsbk[0] & 0x3F, mfid = tsbk[1];
  if (mfid == 0x00 || mfid == 0x01) p25_osp (state, op, tsbk + 2);
  else if (mfid == 0x90) p25_moto (state, op, tsbk + 2);
}

//----------------------------------------------------------------------------
// System picture (control channel / site details), sent when it changes
//----------------------------------------------------------------------------

static char * build_system (dsd_opts * opts, dsd_state * state)
{
  UNUSED(opts);
  jb b;
  jb_init (&b);
  jb_obj (&b, NULL);
  jb_str (&b, "type", "system");
  double now = wall_now ();
  int st = state->lastsynctype;
  int is_dmr = (st >= 10 && st <= 13) || (st >= 32 && st <= 34), is_nxdn = st == 28 || st == 29;
  if (state->carrier && is_dmr) { A.dmr_seen = now; A.dmr_ms = st >= 32; }
  if (state->carrier && is_nxdn) A.nxdn_seen = now;
  if (is_dmr || (A.dmr_seen > 0 && now - A.dmr_seen < 60))
  {
    jb_obj (&b, "dmr");
    if (state->dmr_color_code < 16) jb_int (&b, "cc", state->dmr_color_code);
    if (state->dmr_t3_syscode) jb_hex (&b, "syscode", state->dmr_t3_syscode, 4);
    char t[200];
    trim_id (t, sizeof(t), state->dmr_branding, sizeof(state->dmr_branding));
    if (t[0]) jb_str (&b, "brand", t);
    trim_id (t, sizeof(t), state->dmr_branding_sub, sizeof(state->dmr_branding_sub));
    if (t[0]) jb_str (&b, "sub", t);
    trim_id (t, sizeof(t), state->dmr_site_parms, sizeof(state->dmr_site_parms));
    if (t[0]) jb_str (&b, "site", t);
    if (state->dmr_rest_channel > 0) jb_int (&b, "rest", state->dmr_rest_channel);
    jb_bool (&b, "ms", A.dmr_ms);
    jb_obj_end (&b);
  }
  //the decoder keeps the last P25 identity after leaving the system; only show it while on P25
  //or while the control channel broadcasts are fresh
  int on_p25 = st == 0 || st == 1 || st == 35 || st == 36 || (A.p25_t > 0 && now - A.p25_t < 60);
  if (on_p25 && (A.p25_wacn || A.p25_rfss || A.nnb || state->p2_wacn || state->p2_cc))
  {
    jb_obj (&b, "p25");
    long long wacn = A.p25_wacn ? A.p25_wacn : (long long)state->p2_wacn;
    int sysid = A.p25_sysid ? A.p25_sysid : (int)state->p2_sysid;
    if (wacn) jb_hex (&b, "wacn", (unsigned long long)wacn, 5);
    if (sysid) jb_hex (&b, "sys", (unsigned long long)sysid, 3);
    if (state->p2_cc) jb_hex (&b, "nac", state->p2_cc, 3);
    if (A.p25_rfss || state->p2_rfssid) jb_int (&b, "rfss", A.p25_rfss ? A.p25_rfss : (int)state->p2_rfssid);
    if (A.p25_site || state->p2_siteid) jb_int (&b, "site", A.p25_site ? A.p25_site : (int)state->p2_siteid);
    if (A.p25_cc_ch) { jb_hex (&b, "cc_ch", A.p25_cc_ch, 4); }
    long long ccf = A.p25_cc_ch ? p25_freq (state, A.p25_cc_ch) : 0;
    if (!ccf) ccf = state->p25_cc_freq;
    if (ccf > 0) jb_int (&b, "cc", ccf);
    if (A.p25_ssc) jb_hex (&b, "ssc", A.p25_ssc, 2);
    if (A.p25_time[0]) jb_str (&b, "time", A.p25_time);
    jb_arr (&b, "nb");
    for (int i = 0; i < A.nnb; i++)
    {
      p25_neighbor * n = &A.nb[i];
      jb_obj (&b, NULL);
      jb_int (&b, "rfss", n->rfss);
      jb_int (&b, "site", n->site);
      jb_hex (&b, "sys", n->sysid, 3);
      jb_hex (&b, "ch", n->ch, 4);
      long long f = p25_freq (state, n->ch);
      if (f) jb_int (&b, "f", f);
      jb_bool (&b, "conv", n->cfva & 8);
      jb_bool (&b, "fail", n->cfva & 4);
      jb_obj_end (&b);
    }
    jb_arr_end (&b);
    jb_arr (&b, "sec");
    for (int i = 0; i < A.nsccb; i++)
    {
      jb_obj (&b, NULL);
      jb_hex (&b, "ch", A.sccb[i], 4);
      long long f = p25_freq (state, A.sccb[i]);
      if (f) jb_int (&b, "f", f);
      jb_obj_end (&b);
    }
    jb_arr_end (&b);
    jb_arr (&b, "iden");
    for (int i = 0; i < 16; i++)
    {
      if (!state->p25_base_freq[i]) continue;
      jb_obj (&b, NULL);
      jb_int (&b, "id", i);
      jb_int (&b, "base", (long long)state->p25_base_freq[i] * 5);
      jb_dbl (&b, "step", state->p25_chan_spac[i] * 0.125, 3);
      jb_bool (&b, "tdma", state->p25_chan_tdma[i]);
      jb_obj_end (&b);
    }
    jb_arr_end (&b);
    jb_obj_end (&b);
  }
  if (is_nxdn || (A.nxdn_seen > 0 && now - A.nxdn_seen < 60))
  {
    jb_obj (&b, "nxdn");
    jb_int (&b, "ran", state->nxdn_last_ran);
    if (state->nxdn_location_sys_code) jb_int (&b, "sys", state->nxdn_location_sys_code);
    if (state->nxdn_location_site_code) jb_int (&b, "site", state->nxdn_location_site_code);
    jb_obj_end (&b);
  }
  jb_obj_end (&b);
  char * s = b.buf ? b.buf : strdup ("{\"type\":\"system\"}");
  b.buf = NULL;
  jb_free (&b);
  return s;
}

static void sys_poll (dsd_opts * opts, dsd_state * state)
{
  double now = wall_now ();
  if (now - A.sys_t < 1.0) return;
  A.sys_t = now;
  char * s = build_system (opts, state);
  //resend when it changed, and every 10 s for browsers that missed one (status kind can drop)
  static double last_sent;
  int same = A.sys_json && !strcmp (A.sys_json, s);
  if (!same || now - last_sent > 10.0)
  {
    if (web_server_clients () > 0) web_broadcast (WEB_KIND_STATUS, WS_OP_TEXT, s, strlen (s), 0);
    last_sent = now;
  }
  free (A.sys_json);
  A.sys_json = s;
}

//----------------------------------------------------------------------------
// Entry points for dsd_web.c
//----------------------------------------------------------------------------

void web_act_init (dsd_opts * opts, dsd_state * state)
{
  A.opts = opts;
  A.state = state;
  A.next_id = 1;
  A.inited = 1;
}

void web_act_poll (dsd_opts * opts, dsd_state * state)
{
  if (!A.inited) return;
  //a frame went by: claims on the decoder's data log lines end here
  for (int s = 0; s < 2; s++) { A.claim[s] = 0; A.claimed[s] = NULL; }
  calls_poll (opts, state);
  grants_poll ();
  sys_poll (opts, state);
  act_flush ();
}

//everything in the ring, oldest first (sent to a browser that just connected)
void web_act_send (int client)
{
  if (!A.inited) return;
  jb b;
  jb_init (&b);
  jb_raw (&b, "{\"type\":\"activity\",\"reset\":true,\"items\":[");
  for (int k = A.count - 1; k >= 0; k--)
  {
    int i = (A.head - 1 - k + ACT_MAX) % ACT_MAX;
    jb_raw (&b, A.json[i]);
    if (k) jb_rawn (&b, ",", 1);
  }
  jb_raw (&b, "]}");
  if (b.buf) web_send (client, WEB_KIND_CTRL, WS_OP_TEXT, b.buf, b.len);
  jb_free (&b);
  if (A.sys_json) web_send (client, WEB_KIND_CTRL, WS_OP_TEXT, A.sys_json, strlen (A.sys_json));
}

void web_act_clear (void)
{
  if (!A.inited) return;
  for (int i = 0; i < ACT_MAX; i++) { free (A.json[i]); A.json[i] = NULL; A.ids[i] = 0; }
  A.head = A.count = A.npend = 0;
  const char * msg = "{\"type\":\"activity\",\"reset\":true,\"items\":[]}";
  web_broadcast (WEB_KIND_CTRL, WS_OP_TEXT, msg, strlen (msg), 0);
  //calls and grants still running show up again with their next update
  for (int s = 0; s < 2; s++) if (A.call[s].on) act_publish (&A.call[s].a);
  for (int i = 0; i < GRANT_MAX; i++) if (A.grant[i].on) act_publish (&A.grant[i].a);
}

//the P25 site picture belongs to the system the radio was on; forget it with a new frequency
void web_act_retuned (void)
{
  if (!A.inited) return;
  A.p25_wacn = 0; A.p25_sysid = A.p25_rfss = A.p25_site = A.p25_cc_ch = A.p25_ssc = 0;
  A.p25_t = 0; A.nnb = 0; A.nsccb = 0; A.iden_seen = 0; A.p25_time[0] = 0;
  A.dmr_seen = A.nxdn_seen = 0;
  A.p25_logged_wacn = 0; A.p25_logged_sys = A.p25_logged_rfss = A.p25_logged_site = A.p25_logged_cc = 0;
  for (int i = 0; i < GRANT_MAX; i++) grant_end (&A.grant[i]);
}
