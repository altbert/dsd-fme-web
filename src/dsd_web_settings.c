/*-------------------------------------------------------------------------------
 * dsd_web_settings.c
 * Settings the web page keeps on the DSD-FME host
 *
 * The band scan's and the bookmark scan's settings live here instead of in
 * each browser, so every browser shows the same ones and they survive a
 * restart: in web_settings.json, next to the bookmark file. Only touched on the
 * decoder thread (from web_poll), like the bookmarks; a change is written
 * straight back to disk and sent to every connected page.
 *-----------------------------------------------------------------------------*/

#include "dsd_web.h"
#include <errno.h>

//the Bands menu's choices (web/app.js, BS_PRESETS and custom)
static const char * band_keys[] = { "lmr", "vhf", "uhf", "uhft", "800", "900", "ham", "pmr", "custom", NULL };

static struct
{
  char path[1100];
  int saved;                  //the file exists (or was written): a page has nothing of its own to bring
  char bands[8];              //band scan: the Bands choice
  char from[24], to[24];      //...the custom range, as typed (MHz)
  double threshold;           //...dB over the noise
  char exclude[4096];         //...frequencies and ranges never stopped on, as typed
  int skip_bookmarked;
  int keep_going;
  double hang;                //bookmark scan: s it listens after a bookmark goes quiet
} S;

//at most n-1 bytes, without cutting a UTF-8 sequence; control characters become spaces (newlines stay
//when the field is a list)
static void copy_field (char * dst, size_t n, const char * src, int lines)
{
  size_t o = 0;
  if (!src) src = "";
  for (size_t i = 0; src[i] && o + 1 < n; )
  {
    unsigned char c = (unsigned char)src[i];
    size_t l = c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : (c >= 0xC0 ? 2 : 1));
    if (o + l >= n) break;
    for (size_t k = 0; k < l && src[i]; k++, i++)
    {
      unsigned char d = (unsigned char)src[i];
      dst[o++] = d == '\n' && lines ? '\n' : ((d < 0x20 || d == 0x7F) ? ' ' : (char)d);
    }
  }
  dst[o] = 0;
}

static void defaults (void)
{
  snprintf (S.bands, sizeof(S.bands), "lmr");
  S.from[0] = S.to[0] = S.exclude[0] = 0;
  S.threshold = 10;
  S.skip_bookmarked = 1;
  S.keep_going = 0;
  S.hang = 2;
}

//take what an object holds of band_scan and bookmark_scan, leaving the rest as it is
static void take (jv * o)
{
  jv * b = jv_get (o, "band_scan");
  if (b && b->t == JV_OBJ)
  {
    if (jv_has (b, "bands"))
    {
      const char * k = jv_str (b, "bands", "");
      for (int i = 0; band_keys[i]; i++)
        if (!strcmp (k, band_keys[i])) snprintf (S.bands, sizeof(S.bands), "%s", k);
    }
    if (jv_has (b, "from")) copy_field (S.from, sizeof(S.from), jv_str (b, "from", ""), 0);
    if (jv_has (b, "to")) copy_field (S.to, sizeof(S.to), jv_str (b, "to", ""), 0);
    if (jv_has (b, "threshold"))
    {
      double t = jv_num (b, "threshold", 10);
      S.threshold = !(t >= 4) ? 4 : (t > 40 ? 40 : t);
    }
    if (jv_has (b, "exclude")) copy_field (S.exclude, sizeof(S.exclude), jv_str (b, "exclude", ""), 1);
    if (jv_has (b, "skip_bookmarked")) S.skip_bookmarked = jv_bool (b, "skip_bookmarked", 1);
    if (jv_has (b, "keep_going")) S.keep_going = jv_bool (b, "keep_going", 0);
  }
  jv * m = jv_get (o, "bookmark_scan");
  if (m && m->t == JV_OBJ && jv_has (m, "hang"))
  {
    double h = jv_num (m, "hang", 2);
    S.hang = !(h >= 0) ? 0 : (h > 600 ? 600 : h);
  }
}

static void put (jb * b)
{
  jb_obj (b, "band_scan");
  jb_str (b, "bands", S.bands);
  jb_str (b, "from", S.from);
  jb_str (b, "to", S.to);
  jb_dbl (b, "threshold", S.threshold, 1);
  jb_str (b, "exclude", S.exclude);
  jb_bool (b, "skip_bookmarked", S.skip_bookmarked);
  jb_bool (b, "keep_going", S.keep_going);
  jb_obj_end (b);
  jb_obj (b, "bookmark_scan");
  jb_dbl (b, "hang", S.hang, 1);
  jb_obj_end (b);
}

//mkdir -p for the directory part of a path
static void make_parent_dirs (const char * path)
{
  char tmp[1100];
  snprintf (tmp, sizeof(tmp), "%s", path);
  for (char * p = tmp + 1; *p; p++)
  {
    if (*p != '/') continue;
    *p = 0;
    if (mkdir (tmp, 0700) != 0 && errno != EEXIST) return;
    *p = '/';
  }
}

//write to a temp file and rename it over the old one, as the bookmarks are
static int save (void)
{
  char tmp[1200];
  snprintf (tmp, sizeof(tmp), "%s.tmp", S.path);
  make_parent_dirs (S.path);
  FILE * f = fopen (tmp, "w");
  int ok = 0;
  if (f)
  {
    jb b; jb_init (&b);
    jb_obj (&b, NULL);
    jb_int (&b, "version", 1);
    put (&b);
    jb_obj_end (&b);
    ok = fprintf (f, "%s\n", b.buf) > 0;
    jb_free (&b);
    ok &= fclose (f) == 0;
    if (ok) ok = rename (tmp, S.path) == 0;
    if (!ok) remove (tmp);
  }
  if (!ok) fprintf (stderr, "Web: unable to write settings to %s (%s)\n", S.path, strerror (errno));
  return ok;
}

//to one client, or everybody when client < 0
static void send_settings (int client)
{
  jb b; jb_init (&b);
  jb_obj (&b, NULL);
  jb_str (&b, "type", "settings");
  jb_bool (&b, "saved", S.saved);
  put (&b);
  jb_obj_end (&b);
  if (client < 0) web_broadcast (WEB_KIND_CTRL, WS_OP_TEXT, b.buf, b.len, 0);
  else web_send (client, WEB_KIND_CTRL, WS_OP_TEXT, b.buf, b.len);
  jb_free (&b);
}

//----------------------------------------------------------------------------
// Public
//----------------------------------------------------------------------------

void web_settings_init (const char * dir)
{
  memset (&S, 0, sizeof(S));
  defaults ();
  snprintf (S.path, sizeof(S.path), "%s/web_settings.json", dir && dir[0] ? dir : ".");
  fprintf (stderr, "Web settings: %s\n", S.path);

  FILE * f = fopen (S.path, "rb");
  if (!f) return; //nothing saved yet
  char * text = NULL;
  struct stat st;
  if (fstat (fileno (f), &st) == 0 && st.st_size > 0 && st.st_size < 1024 * 1024)
  {
    text = malloc ((size_t)st.st_size + 1);
    if (text && fread (text, 1, (size_t)st.st_size, f) != (size_t)st.st_size) { free (text); text = NULL; }
  }
  fclose (f);
  if (!text) return;
  jv * root = jv_parse (text, (size_t)st.st_size);
  if (root && root->t == JV_OBJ)
  {
    take (root);
    S.saved = 1;
  }
  else fprintf (stderr, "Web: settings file %s is not valid JSON, starting with the defaults\n", S.path);
  jv_free (root);
  free (text);
}

//settings commands from a client; returns 1 when the command was one of ours
int web_settings_command (int client, const char * c, jv * cmd)
{
  if (!strcmp (c, "settings_get"))
  {
    send_settings (client);
    return 1;
  }
  if (!strcmp (c, "settings_set"))
  {
    take (cmd);
    if (save ()) S.saved = 1;
    send_settings (-1); //every page shows the same, the one that sent it too
    return 1;
  }
  return 0;
}
