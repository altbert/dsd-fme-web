/*-------------------------------------------------------------------------------
 * dsd_web_rec.c
 * DSD-FME web interface: call recordings
 *
 * Records each call as the web interface plays it (web_audio_push: after the
 * decoder's gain, high-pass filter and mutes) into a temporary folder that is
 * removed at exit, oldest first past a size cap. A recording can be kept: it is
 * copied into the per-call WAV folder with the call's details next to it in a
 * .json sidecar. Upstream's per-call WAV files there get a sidecar too, matched
 * to the call they hold. Runs on the decoder thread only (web_audio_push and
 * web_poll both run there).
 *-----------------------------------------------------------------------------*/

#include "dsd_web.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/time.h>

#ifndef WEB_REC_CAP_MB
#define WEB_REC_CAP_MB 256  //temporary recordings kept, oldest removed first
#endif
#define REC_CH       3      //0 = TS1 (or the only channel), 1 = TS2, 2 = analog
#define REC_PRE_SECS 3      //audio kept from before a call opens (late entry, the analog item's delay)
#define REC_MAX      4096   //temporary recordings listed
#define REC_RECENT   32     //ended calls kept to match upstream's per-call WAV files to
#define REC_SCAN     2.0    //seconds between looks at the per-call WAV folder
#define REC_NEW      120    //per-call WAV files this many seconds old or newer are matched
#define REC_LIST_MAX 500    //files listed from the per-call WAV folder
#ifndef REC_LOUD
#define REC_LOUD     8      //sample magnitude that counts as audio (muted calls are all 0, silence frames stay under it)
#endif

typedef struct
{
  SNDFILE * f;         //opened with the call's first audio, at its rate
  uint32_t id;         //call being recorded, 0 = none
  int failed;          //the file could not be created
  int rate;
  long long frames, loud;
  short * pre;         //ring of the audio heard while no call was open
  int pre_cap, pre_len, pre_head, pre_rate;
  double pre_t;        //when the last of it came
} rec_chan;

typedef struct
{
  uint32_t id;
  long long bytes;
  double dur;          //seconds of audio
  double t, t_end;     //the call (wall clock)
  long long src, dst;
  char srcs[64], dsts[64], proto[16];
  char * json;         //the call's activity item, NULL until it ended
  char kept[256];      //file name in the per-call WAV folder once kept
} rec_file;

typedef struct
{
  double t, t_end;
  long long src, dst;
  char srcs[64], dsts[64];
  char * json;
} rec_recent;

static struct
{
  int on;
  char dir[1024];      //temporary folder, "" until the first recording
  int dir_failed;
  rec_chan ch[REC_CH];
  rec_file * files;    //oldest first
  int nfiles;
  long long total;
  rec_recent recent[REC_RECENT];
  int recent_next;
  double scan_t;
  char err[256];
} R = { .on = 1 };

static double rec_now (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1e6;
}

static void rec_error (const char * fmt, const char * arg)
{
  if (!R.err[0]) snprintf (R.err, sizeof(R.err), fmt, arg);
}

//the message of a problem met while recording on its own, once ("" none)
const char * web_rec_error (void)
{
  static char out[256];
  snprintf (out, sizeof(out), "%s", R.err);
  R.err[0] = 0;
  return out;
}

int web_rec_enabled (void)
{
  return R.on;
}

static int rec_dir_ok (void)
{
  if (R.dir[0]) return 1;
  if (R.dir_failed) return 0;
  const char * tmp = getenv ("TMPDIR");
  if (!tmp || !tmp[0]) tmp = "/tmp";
  snprintf (R.dir, sizeof(R.dir), "%s/dsd-fme-web-rec-XXXXXX", tmp);
  if (!mkdtemp (R.dir))
  {
    rec_error ("Call recording is off: unable to create a temporary folder (%s).", strerror (errno));
    R.dir[0] = 0;
    R.dir_failed = 1;
    return 0;
  }
  web_server_set_area ("rec", R.dir);
  return 1;
}

static void rec_path (char * out, size_t n, uint32_t id)
{
  snprintf (out, n, "%s/%u.wav", R.dir, id);
}

static rec_file * rec_find (uint32_t id)
{
  for (int i = 0; i < R.nfiles; i++)
    if (R.files[i].id == id) return &R.files[i];
  return NULL;
}

static void rec_drop (int i)
{
  char p[1100];
  rec_path (p, sizeof(p), R.files[i].id);
  unlink (p);
  R.total -= R.files[i].bytes;
  free (R.files[i].json);
  memmove (&R.files[i], &R.files[i + 1], (size_t)(R.nfiles - i - 1) * sizeof(rec_file));
  R.nfiles--;
}

//pre-roll: keep the last REC_PRE_SECS of a channel's audio while no call is open on it
static void pre_add (rec_chan * c, const short * s, int n, int rate)
{
  if (rate != c->pre_rate || !c->pre)
  {
    free (c->pre);
    c->pre_cap = rate * REC_PRE_SECS;
    c->pre = malloc ((size_t)c->pre_cap * sizeof(short));
    c->pre_len = c->pre_head = 0;
    c->pre_rate = rate;
    if (!c->pre) { c->pre_cap = 0; return; }
  }
  for (int i = 0; i < n; i++)
  {
    c->pre[c->pre_head] = s[i];
    c->pre_head = (c->pre_head + 1) % c->pre_cap;
    if (c->pre_len < c->pre_cap) c->pre_len++;
  }
  c->pre_t = rec_now ();
}

static void chan_write (rec_chan * c, const short * s, int n)
{
  sf_write_short (c->f, s, n);
  c->frames += n;
  for (int i = 0; i < n; i++)
    if (s[i] > REC_LOUD || s[i] < -REC_LOUD) c->loud++;
}

static void cap_files (void)
{
  const long long cap = (long long)WEB_REC_CAP_MB * 1024 * 1024;
  while (R.total > cap && R.nfiles > 1) rec_drop (0);
}

//close the channel's recording: kept when it holds audio, removed when it is all silence
static void chan_close (rec_chan * c)
{
  if (!c->id) return;
  if (!c->f) { c->id = 0; return; } //no audio came
  sf_close (c->f);
  c->f = NULL;
  char p[1100];
  rec_path (p, sizeof(p), c->id);
  //a muted call (encrypted without a key, blocked, held) only wrote zeros
  if (c->loud < c->rate / 4)
  {
    unlink (p);
    c->id = 0;
    return;
  }
  rec_file * f = realloc (R.files, (size_t)(R.nfiles + 1) * sizeof(rec_file));
  if (!f) { unlink (p); c->id = 0; return; }
  R.files = f;
  rec_file * r = &R.files[R.nfiles++];
  memset (r, 0, sizeof(*r));
  r->id = c->id;
  r->dur = (double)c->frames / c->rate;
  struct stat st;
  r->bytes = stat (p, &st) == 0 ? (long long)st.st_size : c->frames * 2;
  r->src = r->dst = -1;
  R.total += r->bytes;
  if (R.nfiles > REC_MAX) rec_drop (0);
  cap_files ();
  c->id = 0;
}

//a call opened on a channel: record it. Returns 0 when recording, -1 when not
double web_rec_open (int ch, uint32_t id)
{
  if (!R.on || ch < 0 || ch >= REC_CH || !id) return -1;
  if (!rec_dir_ok ()) return -1;
  rec_chan * c = &R.ch[ch];
  chan_close (c); //a call the decoder didn't close
  //the pre-roll belongs to this call only if it was heard just now
  if (rec_now () - c->pre_t > REC_PRE_SECS) c->pre_len = 0;
  c->id = id;
  c->f = NULL;
  c->failed = 0;
  c->frames = c->loud = 0;
  return 0;
}

//the call's first audio: create its file at that rate, the pre-roll first
static void chan_start (rec_chan * c, int rate)
{
  char p[1100];
  rec_path (p, sizeof(p), c->id);
  SF_INFO info;
  memset (&info, 0, sizeof(info));
  info.samplerate = rate;
  info.channels = 1;
  info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  c->f = sf_open (p, SFM_WRITE, &info);
  if (!c->f)
  {
    rec_error ("Unable to record a call: %s", sf_strerror (NULL));
    c->failed = 1;
    return;
  }
  c->rate = rate;
  if (c->pre_len && c->pre_rate == rate)
  {
    int start = (c->pre_head - c->pre_len + c->pre_cap) % c->pre_cap;
    int first = c->pre_cap - start < c->pre_len ? c->pre_cap - start : c->pre_len;
    chan_write (c, c->pre + start, first);
    if (c->pre_len > first) chan_write (c, c->pre, c->pre_len - first);
  }
  c->pre_len = 0;
}

//the call ended: seconds of audio in its recording, 0 when there is none
double web_rec_close (int ch, uint32_t id)
{
  if (ch >= 0 && ch < REC_CH && R.ch[ch].id && R.ch[ch].id == id) chan_close (&R.ch[ch]);
  if (ch >= 0 && ch < REC_CH) R.ch[ch].pre_len = 0; //what follows belongs to the next call
  rec_file * r = rec_find (id);
  return r ? r->dur : 0;
}

void web_rec_set (int on)
{
  R.on = on ? 1 : 0;
  if (!R.on)
    for (int i = 0; i < REC_CH; i++) chan_close (&R.ch[i]);
}

//audio the web interface plays (web_audio_push): interleaved channels, int16 or float32
void web_rec_audio (const void * data, size_t nbytes, int channels, int is_float, int rate, int analog)
{
  if (!R.on || channels < 1 || rate <= 0) return;
  int bps = is_float ? 4 : 2;
  int frames = (int)(nbytes / (size_t)(bps * channels));
  if (frames <= 0) return;
  short buf[4096];
  for (int k = 0; k < channels && k < 2; k++)
  {
    int ch = analog ? 2 : k;
    rec_chan * c = &R.ch[ch];
    for (int off = 0; off < frames; off += 4096)
    {
      int n = frames - off < 4096 ? frames - off : 4096;
      for (int i = 0; i < n; i++)
      {
        int j = (off + i) * channels + k;
        if (is_float)
        {
          float v = ((const float *)data)[j] * 32767.0f;
          buf[i] = (short)(v > 32767.0f ? 32767 : (v < -32768.0f ? -32768 : v));
        }
        else buf[i] = ((const short *)data)[j];
      }
      if (!c->id) pre_add (c, buf, n, rate);
      else
      {
        if (!c->f && !c->failed) chan_start (c, rate);
        if (c->f && rate == c->rate) chan_write (c, buf, n);
      }
    }
    if (analog) break;
  }
}

//the call's details once it ended, for its recording and for matching per-call WAV files
void web_rec_ended (const web_rec_call * call)
{
  if (!call || !call->id) return;
  rec_file * r = rec_find (call->id);
  if (r)
  {
    free (r->json);
    r->json = call->json ? strdup (call->json) : NULL;
    r->t = call->t;
    r->t_end = call->t_end;
    r->src = call->src;
    r->dst = call->dst;
    snprintf (r->srcs, sizeof(r->srcs), "%s", call->srcs ? call->srcs : "");
    snprintf (r->dsts, sizeof(r->dsts), "%s", call->dsts ? call->dsts : "");
    snprintf (r->proto, sizeof(r->proto), "%s", call->proto ? call->proto : "");
  }
  rec_recent * e = &R.recent[R.recent_next];
  R.recent_next = (R.recent_next + 1) % REC_RECENT;
  free (e->json);
  memset (e, 0, sizeof(*e));
  e->t = call->t;
  e->t_end = call->t_end;
  e->src = call->src;
  e->dst = call->dst;
  snprintf (e->srcs, sizeof(e->srcs), "%s", call->srcs ? call->srcs : "");
  snprintf (e->dsts, sizeof(e->dsts), "%s", call->dsts ? call->dsts : "");
  e->json = call->json ? strdup (call->json) : NULL;
}

//----------------------------------------------------------------------------
// The per-call WAV folder: sidecars, upstream's file names
//----------------------------------------------------------------------------

static int ends_wav (const char * s)
{
  size_t n = strlen (s);
  return n > 4 && !strcasecmp (s + n - 4, ".wav");
}

//write <dir>/<name>.json: the call's activity item with "rec_kind" added
static int write_sidecar (const char * dir, const char * name, const char * kind, const char * json)
{
  if (!json || json[0] != '{') return 0;
  char p[2100];
  snprintf (p, sizeof(p), "%s/%s.json", dir, name);
  FILE * f = fopen (p, "w");
  if (!f) return 0;
  fprintf (f, "{\"rec_kind\":\"%s\"%s%s\n", kind, json[1] == '}' ? "" : ",", json + 1);
  fclose (f);
  return 1;
}

//the file's sidecar, NULL when it has none or it doesn't parse (free the result)
static char * read_sidecar (const char * dir, const char * name)
{
  char p[2100];
  snprintf (p, sizeof(p), "%s/%s.json", dir, name);
  FILE * f = fopen (p, "r");
  if (!f) return NULL;
  char * buf = NULL;
  if (fseek (f, 0, SEEK_END) == 0)
  {
    long n = ftell (f);
    if (n > 0 && n < 65536 && fseek (f, 0, SEEK_SET) == 0 && (buf = malloc ((size_t)n + 1)) != NULL)
    {
      size_t got = fread (buf, 1, (size_t)n, f);
      buf[got] = 0;
      jv * v = jv_parse (buf, got);
      if (!v || v->t != JV_OBJ) { free (buf); buf = NULL; }
      jv_free (v);
    }
  }
  fclose (f);
  return buf;
}

//upstream's per-call WAV name: YYYYMMDD_HHMMSS_NNNNN_<system>_<GROUP|PRIVATE>[_tags]_TGT_<t>_SRC_<s>.wav
typedef struct
{
  double t;
  int gi;
  char sys[64], tgt[64], src[64];
} pc_name;

static int parse_pc_name (const char * name, pc_name * o)
{
  memset (o, 0, sizeof(*o));
  o->gi = -1;
  struct tm tm;
  memset (&tm, 0, sizeof(tm));
  if (sscanf (name, "%4d%2d%2d_%2d%2d%2d_", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  tm.tm_isdst = -1;
  time_t t = mktime (&tm);
  if (t == (time_t)-1) return 0;
  o->t = (double)t;
  const char * tg = strstr (name, "_TGT_");
  const char * sr = tg ? strstr (tg, "_SRC_") : NULL;
  if (!tg || !sr) return 0;
  snprintf (o->tgt, sizeof(o->tgt), "%.*s", (int)(sr - tg - 5), tg + 5);
  const char * end = strrchr (sr, '.');
  snprintf (o->src, sizeof(o->src), "%.*s", (int)((end ? end : sr + strlen (sr)) - sr - 5), sr + 5);
  //the system string: from after the random number to the call kind (or the TGT)
  const char * s = name + 16;
  const char * u = strchr (s, '_');
  if (u)
  {
    s = u + 1;
    const char * stop = tg;
    const char * g = strstr (s, "_GROUP");
    const char * pv = strstr (s, "_PRIVATE");
    if (g && g < stop) { stop = g; o->gi = 0; }
    if (pv && pv < stop) { stop = pv; o->gi = 1; }
    if (!strncmp (s, "GROUP", 5)) { o->gi = 0; stop = s; }
    if (!strncmp (s, "PRIVATE", 7)) { o->gi = 1; stop = s; }
    while (stop > s && stop[-1] == '_') stop--;
    if (stop > s) snprintf (o->sys, sizeof(o->sys), "%.*s", (int)(stop - s), s);
  }
  return 1;
}

//an id from a file name against a call's: upstream leaves an id it has no string for empty (M17 to @ALL)
static int id_matches (const char * s, long long num, const char * str)
{
  if (!s[0]) return 1;
  char * e;
  long long v = strtoll (s, &e, 10);
  if (*e == 0) return v == num || (v == 0 && num < 0);
  return str[0] && !strcmp (s, str);
}

//new per-call WAV files: find the call each one holds and write its sidecar
static void match_per_call (dsd_opts * opts)
{
  DIR * d = opendir (opts->wav_out_dir);
  if (!d) return;
  time_t now = time (NULL);
  struct dirent * de;
  while ((de = readdir (d)) != NULL)
  {
    const char * name = de->d_name;
    if (name[0] == '.' || !ends_wav (name) || !strncmp (name, "TEMP_", 5)) continue;
    char p[2100];
    struct stat st;
    snprintf (p, sizeof(p), "%s/%s", opts->wav_out_dir, name);
    if (stat (p, &st) != 0 || !S_ISREG(st.st_mode) || now - st.st_mtime > REC_NEW) continue;
    snprintf (p, sizeof(p), "%s/%s.json", opts->wav_out_dir, name);
    if (stat (p, &st) == 0) continue;
    pc_name pc;
    if (!parse_pc_name (name, &pc)) continue;
    const rec_recent * best = NULL;
    double best_d = 1e9;
    for (int i = 0; i < REC_RECENT; i++)
    {
      const rec_recent * e = &R.recent[i];
      if (!e->json || !id_matches (pc.tgt, e->dst, e->dsts) || !id_matches (pc.src, e->src, e->srcs)) continue;
      //the name carries a time from the call (its start or end, in whole seconds): the call
      //whose span it is in or nearest to wins
      if (pc.t < e->t - 5 || pc.t > e->t_end + 5) continue;
      double dd = pc.t < e->t ? e->t - pc.t : (pc.t > e->t_end ? pc.t - e->t_end : 0);
      if (dd < best_d) { best_d = dd; best = e; }
    }
    if (best) write_sidecar (opts->wav_out_dir, name, "per-call", best->json);
  }
  closedir (d);
}

void web_rec_poll (dsd_opts * opts)
{
  double now = rec_now ();
  if (now - R.scan_t < REC_SCAN) return;
  R.scan_t = now;
  if (opts->dmr_stereo_wav) match_per_call (opts);
}

//----------------------------------------------------------------------------
// Keep, delete, list (commands from the browser)
//----------------------------------------------------------------------------

static void safe_part (char * out, size_t n, const char * in)
{
  size_t k = 0;
  for (size_t i = 0; in[i] && k + 1 < n; i++)
  {
    unsigned char c = (unsigned char)in[i];
    out[k++] = (isalnum (c) || c == '-' || c == '+' || c == ',') ? (char)c : '_';
  }
  out[k] = 0;
}

//copy a temporary recording into the per-call WAV folder; 0 and the file's name, or -1 and why
int web_rec_keep (dsd_opts * opts, uint32_t id, char * out, size_t n)
{
  rec_file * r = rec_find (id);
  if (!r) { snprintf (out, n, "that recording is no longer there"); return -1; }
  if (!r->json) { snprintf (out, n, "the call is still going on"); return -1; }
  if (r->kept[0]) { snprintf (out, n, "it is kept already as %s", r->kept); return -1; }
  struct stat st;
  if (stat (opts->wav_out_dir, &st) == -1) mkdir (opts->wav_out_dir, 0700);

  //upstream's per-call names without the call kind, and only the ids the call had (analog has none)
  char date[32], proto[32], tgt[80] = "", src[80] = "", part[72];
  time_t t = (time_t)r->t;
  struct tm tm;
  localtime_r (&t, &tm);
  strftime (date, sizeof(date), "%Y%m%d_%H%M%S", &tm);
  safe_part (proto, sizeof(proto), r->proto[0] ? r->proto : "CALL");
  if (r->dsts[0]) { safe_part (part, sizeof(part), r->dsts); snprintf (tgt, sizeof(tgt), "_TGT_%s", part); }
  else if (r->dst > 0) snprintf (tgt, sizeof(tgt), "_TGT_%lld", r->dst);
  if (r->srcs[0]) { safe_part (part, sizeof(part), r->srcs); snprintf (src, sizeof(src), "_SRC_%s", part); }
  else if (r->src > 0) snprintf (src, sizeof(src), "_SRC_%lld", r->src);
  char name[256];
  snprintf (name, sizeof(name), "%s_%05u_%s%s%s.wav", date, id % 100000, proto, tgt, src);

  char from[1100], to[2100];
  rec_path (from, sizeof(from), id);
  snprintf (to, sizeof(to), "%s/%s", opts->wav_out_dir, name);
  FILE * in = fopen (from, "rb");
  if (!in) { snprintf (out, n, "unable to read it (%s)", strerror (errno)); return -1; }
  FILE * o = fopen (to, "wbx");
  if (!o)
  {
    snprintf (out, n, "unable to write %s (%s)", to, strerror (errno));
    fclose (in);
    return -1;
  }
  char buf[65536];
  size_t got;
  int ok = 1;
  while ((got = fread (buf, 1, sizeof(buf), in)) > 0)
    if (fwrite (buf, 1, got, o) != got) { ok = 0; break; }
  fclose (in);
  if (fclose (o) != 0) ok = 0;
  if (!ok)
  {
    snprintf (out, n, "unable to write %s (%s)", to, strerror (errno));
    unlink (to);
    return -1;
  }
  write_sidecar (opts->wav_out_dir, name, "kept", r->json);
  snprintf (r->kept, sizeof(r->kept), "%s", name);
  snprintf (out, n, "%s", name);
  return 0;
}

//1 when removed
int web_rec_delete (dsd_opts * opts, const char * area, const char * name)
{
  if (!area || !name || !web_path_component_ok (name) || !ends_wav (name)) return 0;
  if (!strcmp (area, "rec"))
  {
    char * e;
    unsigned long id = strtoul (name, &e, 10);
    if (strcasecmp (e, ".wav")) return 0;
    for (int i = 0; i < R.nfiles; i++)
      if (R.files[i].id == id) { rec_drop (i); return 1; }
    return 0;
  }
  if (strcmp (area, "wav")) return 0;
  char p[2100];
  snprintf (p, sizeof(p), "%s/%s", opts->wav_out_dir, name);
  struct stat st;
  //upstream is still writing a TEMP_ file
  if (!strncmp (name, "TEMP_", 5) && stat (p, &st) == 0 && time (NULL) - st.st_mtime < 30) return 0;
  if (unlink (p) != 0) return 0;
  snprintf (p, sizeof(p), "%s/%s.json", opts->wav_out_dir, name);
  unlink (p);
  for (int i = 0; i < R.nfiles; i++)
    if (!strcmp (R.files[i].kept, name)) R.files[i].kept[0] = 0;
  return 1;
}

typedef struct
{
  char name[256];
  long long size, mtime;
} wav_ent;

static int wav_newer (const void * a, const void * b)
{
  long long x = ((const wav_ent *)a)->mtime, y = ((const wav_ent *)b)->mtime;
  return x < y ? 1 : (x > y ? -1 : 0);
}

static double wav_secs (const char * path)
{
  SF_INFO info;
  memset (&info, 0, sizeof(info));
  SNDFILE * f = sf_open (path, SFM_READ, &info);
  if (!f) return 0;
  sf_close (f);
  return info.samplerate > 0 ? (double)info.frames / info.samplerate : 0;
}

//{"type":"recordings","items":[...]}: temporary recordings, then the per-call WAV folder, newest first
void web_rec_list (dsd_opts * opts, jb * b)
{
  jb_obj (b, NULL);
  jb_str (b, "type", "recordings");
  jb_bool (b, "on", R.on);
  jb_int (b, "cap_mb", WEB_REC_CAP_MB);
  jb_dbl (b, "used_mb", R.total / 1048576.0, 1);
  jb_str (b, "wav_dir", opts->wav_out_dir);
  jb_arr (b, "items");
  for (int i = R.nfiles - 1; i >= 0; i--)
  {
    const rec_file * r = &R.files[i];
    char name[32];
    snprintf (name, sizeof(name), "%u.wav", r->id);
    jb_obj (b, NULL);
    jb_str (b, "area", "rec");
    jb_str (b, "name", name);
    jb_str (b, "kind", "temp");
    jb_int (b, "id", r->id);
    jb_int (b, "size", r->bytes);
    jb_int (b, "mtime", (long long)r->t_end);
    jb_dbl (b, "dur", r->dur, 1);
    if (r->kept[0]) jb_str (b, "kept", r->kept);
    if (r->json) { jb_key (b, "meta"); jb_raw (b, r->json); }
    jb_obj_end (b);
  }

  DIR * d = opendir (opts->wav_out_dir);
  wav_ent * ents = NULL;
  int n = 0, cap = 0;
  if (d)
  {
    struct dirent * de;
    time_t now = time (NULL);
    while ((de = readdir (d)) != NULL)
    {
      if (!web_path_component_ok (de->d_name) || !ends_wav (de->d_name)) continue;
      char p[2100];
      struct stat st;
      snprintf (p, sizeof(p), "%s/%s", opts->wav_out_dir, de->d_name);
      if (stat (p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
      if (!strncmp (de->d_name, "TEMP_", 5) && now - st.st_mtime < 30) continue; //upstream is writing it
      if (n == cap)
      {
        cap = cap ? cap * 2 : 64;
        wav_ent * g = realloc (ents, (size_t)cap * sizeof(wav_ent));
        if (!g) break;
        ents = g;
      }
      snprintf (ents[n].name, sizeof(ents[n].name), "%s", de->d_name);
      ents[n].size = (long long)st.st_size;
      ents[n].mtime = (long long)st.st_mtime;
      n++;
    }
    closedir (d);
  }
  if (n) qsort (ents, (size_t)n, sizeof(wav_ent), wav_newer);
  for (int i = 0; i < n && i < REC_LIST_MAX; i++)
  {
    char p[2100];
    snprintf (p, sizeof(p), "%s/%s", opts->wav_out_dir, ents[i].name);
    char * side = read_sidecar (opts->wav_out_dir, ents[i].name);
    const char * kind = "per-call";
    if (side)
    {
      jv * v = jv_parse (side, strlen (side));
      if (v && !strcmp (jv_str (v, "rec_kind", ""), "kept")) kind = "kept";
      jv_free (v);
    }
    jb_obj (b, NULL);
    jb_str (b, "area", "wav");
    jb_str (b, "name", ents[i].name);
    jb_str (b, "kind", kind);
    jb_int (b, "size", ents[i].size);
    jb_int (b, "mtime", ents[i].mtime);
    jb_dbl (b, "dur", wav_secs (p), 1);
    if (side) { jb_key (b, "meta"); jb_raw (b, side); }
    else
    {
      //what upstream's file name says
      pc_name pc;
      if (parse_pc_name (ents[i].name, &pc))
      {
        jb_obj (b, "meta");
        jb_dbl (b, "t", pc.t, 0);
        jb_str (b, "k", "call");
        if (pc.sys[0]) jb_str (b, "p", pc.sys);
        if (pc.gi >= 0) jb_int (b, "g", pc.gi);
        jb_str (b, "ty", pc.gi == 1 ? "Private call" : (pc.gi == 0 ? "Group call" : "Voice call"));
        char * e;
        long long v = strtoll (pc.tgt, &e, 10);
        if (pc.tgt[0] && !*e) jb_int (b, "dst", v); else if (pc.tgt[0]) jb_str (b, "dsts", pc.tgt);
        v = strtoll (pc.src, &e, 10);
        if (pc.src[0] && !*e) jb_int (b, "src", v); else if (pc.src[0]) jb_str (b, "srcs", pc.src);
        jb_obj_end (b);
      }
    }
    free (side);
    jb_obj_end (b);
  }
  free (ents);
  jb_arr_end (b);
  jb_obj_end (b);
}

//remove every temporary recording and the folder (at exit)
void web_rec_shutdown (void)
{
  for (int i = 0; i < REC_CH; i++)
  {
    if (R.ch[i].f) { sf_close (R.ch[i].f); R.ch[i].f = NULL; }
    free (R.ch[i].pre);
    R.ch[i].pre = NULL;
  }
  if (!R.dir[0]) return;
  DIR * d = opendir (R.dir);
  if (d)
  {
    struct dirent * de;
    while ((de = readdir (d)) != NULL)
    {
      if (de->d_name[0] == '.') continue;
      char p[2100];
      snprintf (p, sizeof(p), "%s/%s", R.dir, de->d_name);
      unlink (p);
    }
    closedir (d);
  }
  rmdir (R.dir);
  R.dir[0] = 0;
}
