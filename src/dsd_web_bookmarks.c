/*-------------------------------------------------------------------------------
 * dsd_web_bookmarks.c
 * Frequency bookmarks for the DSD-FME web interface
 *
 * Bookmarks live on the DSD-FME host in a small JSON file, so every browser
 * and device that connects sees the same list. They are only touched on the
 * decoder thread (from web_poll), changes are written straight back to disk
 * and the new list is pushed to all connected clients.
 *-----------------------------------------------------------------------------*/

#include "dsd_web.h"
#include <errno.h>

static struct
{
  web_bookmark items[WEB_BM_MAX];
  int count;
  int next_id;
  char path[1024];
} B;

//----------------------------------------------------------------------------
// Helpers
//----------------------------------------------------------------------------

//copy at most n-1 bytes, trimmed, without cutting a UTF-8 sequence or keeping control characters
static void copy_text (char * dst, size_t n, const char * src)
{
  size_t o = 0;
  if (!src) src = "";
  while (*src == ' ' || *src == '\t') src++;
  for (size_t i = 0; src[i] && o + 1 < n; )
  {
    unsigned char c = (unsigned char)src[i];
    size_t l = 1;
    if (c >= 0xF0) l = 4;
    else if (c >= 0xE0) l = 3;
    else if (c >= 0xC0) l = 2;
    if (o + l >= n) break;
    for (size_t k = 0; k < l && src[i]; k++, i++)
    {
      unsigned char d = (unsigned char)src[i];
      dst[o++] = (d < 0x20 || d == 0x7F) ? ' ' : (char)d;
    }
  }
  while (o > 0 && dst[o-1] == ' ') o--;
  dst[o] = 0;
}

static int mod_ok (const char * m)
{
  return !m[0] || !strcmp (m, "c4fm") || !strcmp (m, "qpsk") || !strcmp (m, "gfsk");
}

static web_bookmark * find (int id)
{
  for (int i = 0; i < B.count; i++)
    if (B.items[i].id == id) return &B.items[i];
  return NULL;
}

//fill a bookmark from a JSON object, returns 0 (and why) when it is not usable
static int parse_item (jv * o, web_bookmark * bm, const char ** why)
{
  memset (bm, 0, sizeof(*bm));
  bm->id = (int)jv_int (o, "id", 0);
  bm->freq = jv_int (o, "freq", 0);
  if (bm->freq <= 0 || bm->freq > 10000000000LL) { *why = "frequency out of range"; return 0; }
  copy_text (bm->name, sizeof(bm->name), jv_str (o, "name", ""));
  copy_text (bm->group, sizeof(bm->group), jv_str (o, "group", ""));
  copy_text (bm->mode, sizeof(bm->mode), jv_str (o, "mode", ""));
  copy_text (bm->mod, sizeof(bm->mod), jv_str (o, "mod", ""));
  if (!web_mode_valid (bm->mode)) { *why = "unknown decoder"; return 0; }
  if (!mod_ok (bm->mod)) { *why = "unknown modulation"; return 0; }
  if (!bm->name[0]) snprintf (bm->name, sizeof(bm->name), "%.4f MHz", bm->freq / 1e6);
  return 1;
}

static void item_json (jb * b, const web_bookmark * bm)
{
  jb_obj (b, NULL);
  jb_int (b, "id", bm->id);
  jb_int (b, "freq", bm->freq);
  jb_str (b, "name", bm->name);
  jb_str (b, "group", bm->group);
  jb_str (b, "mode", bm->mode);
  jb_str (b, "mod", bm->mod);
  jb_obj_end (b);
}

//mkdir -p for the directory part of a path
static void make_parent_dirs (const char * path)
{
  char tmp[1024];
  snprintf (tmp, sizeof(tmp), "%s", path);
  for (char * p = tmp + 1; *p; p++)
  {
    if (*p != '/') continue;
    *p = 0;
    if (mkdir (tmp, 0700) != 0 && errno != EEXIST) return;
    *p = '/';
  }
}

//write to a temp file and rename it over the old one, one bookmark per line so it is easy to hand edit
static int save (void)
{
  char tmp[1100];
  snprintf (tmp, sizeof(tmp), "%s.tmp", B.path);
  make_parent_dirs (B.path);
  FILE * f = fopen (tmp, "w");
  int ok = 0;
  if (f)
  {
    jb b; jb_init (&b);
    ok = fputs ("{\n  \"version\": 1,\n  \"bookmarks\": [\n", f) >= 0;
    for (int i = 0; i < B.count && ok; i++)
    {
      jb_reset (&b);
      item_json (&b, &B.items[i]);
      ok = fprintf (f, "    %s%s\n", b.buf, i + 1 < B.count ? "," : "") > 0;
    }
    ok &= fputs ("  ]\n}\n", f) >= 0;
    jb_free (&b);
    ok &= fclose (f) == 0;
    if (ok) ok = rename (tmp, B.path) == 0;
    if (!ok) remove (tmp);
  }
  if (!ok) fprintf (stderr, "Web: unable to write bookmarks to %s (%s)\n", B.path, strerror (errno));
  return ok;
}

static void reply (int client, const char * type, const char * key, const char * msg, int id)
{
  jb b; jb_init (&b);
  jb_obj (&b, NULL);
  jb_str (&b, "type", type);
  if (key) jb_str (&b, key, msg);
  if (id) jb_int (&b, "id", id);
  jb_obj_end (&b);
  web_send (client, WEB_KIND_CTRL, WS_OP_TEXT, b.buf, b.len);
  jb_free (&b);
}

//----------------------------------------------------------------------------
// Public
//----------------------------------------------------------------------------

void bm_init (const char * path)
{
  memset (&B, 0, sizeof(B));
  B.next_id = 1;
  const char * xdg = getenv ("XDG_CONFIG_HOME");
  const char * home = getenv ("HOME");
  if (path && path[0]) snprintf (B.path, sizeof(B.path), "%s", path);
  else if (xdg && xdg[0]) snprintf (B.path, sizeof(B.path), "%s/dsd-fme/web_bookmarks.json", xdg);
  else if (home && home[0]) snprintf (B.path, sizeof(B.path), "%s/.config/dsd-fme/web_bookmarks.json", home);
  else snprintf (B.path, sizeof(B.path), "dsd-fme-bookmarks.json");

  FILE * f = fopen (B.path, "rb");
  if (!f) return; //nothing saved yet
  char * text = NULL;
  struct stat st;
  if (fstat (fileno (f), &st) == 0 && st.st_size > 0 && st.st_size < 4 * 1024 * 1024)
  {
    text = malloc ((size_t)st.st_size + 1);
    if (text && fread (text, 1, (size_t)st.st_size, f) != (size_t)st.st_size) { free (text); text = NULL; }
  }
  fclose (f);
  if (!text) return;

  jv * root = jv_parse (text, (size_t)st.st_size);
  jv * arr = root && root->t == JV_ARR ? root : jv_get (root, "bookmarks");
  int bad = 0;
  for (jv * e = jv_first (arr); e && B.count < WEB_BM_MAX; e = e->next)
  {
    web_bookmark bm;
    const char * why = NULL;
    if (!parse_item (e, &bm, &why)) { bad++; continue; }
    if (bm.id <= 0 || find (bm.id)) bm.id = 0; //renumbered below
    B.items[B.count++] = bm;
    if (bm.id >= B.next_id) B.next_id = bm.id + 1;
  }
  for (int i = 0; i < B.count; i++)
    if (B.items[i].id == 0) B.items[i].id = B.next_id++;
  if (!root) fprintf (stderr, "Web: bookmark file %s is not valid JSON, starting with an empty list\n", B.path);
  else if (bad) fprintf (stderr, "Web: skipped %d unusable bookmarks in %s\n", bad, B.path);
  jv_free (root);
  free (text);
}

const char * bm_path (void)
{
  return B.path;
}

int bm_get (int id, web_bookmark * out)
{
  web_bookmark * bm = find (id);
  if (!bm) return 0;
  *out = *bm;
  return 1;
}

//id of the bookmark closest to freq within tol Hz, 0 if there is none
int bm_near (long long freq, long long tol)
{
  int id = 0;
  long long best = tol + 1;
  for (int i = 0; i < B.count; i++)
  {
    long long d = llabs (B.items[i].freq - freq);
    if (d < best) { best = d; id = B.items[i].id; }
  }
  return id;
}

//add a bookmark from the decoder side (band scan); a channel that is already
//bookmarked is not added twice. Returns the bookmark id, 0 if the list is full
int bm_add (const web_bookmark * in)
{
  int id = bm_near (in->freq, 2500);
  if (id) return id;
  if (B.count >= WEB_BM_MAX) return 0;
  web_bookmark * bm = &B.items[B.count++];
  memset (bm, 0, sizeof(*bm));
  bm->id = B.next_id++;
  bm->freq = in->freq;
  copy_text (bm->name, sizeof(bm->name), in->name); //names can carry over-the-air callsigns
  copy_text (bm->group, sizeof(bm->group), in->group);
  copy_text (bm->mode, sizeof(bm->mode), web_mode_valid (in->mode) ? in->mode : "");
  copy_text (bm->mod, sizeof(bm->mod), mod_ok (in->mod) ? in->mod : "");
  if (!bm->name[0]) snprintf (bm->name, sizeof(bm->name), "%.4f MHz", bm->freq / 1e6);
  save ();
  bm_send (-1);
  return bm->id;
}

//send the list to one client, or everybody when client < 0
void bm_send (int client)
{
  jb b; jb_init (&b);
  jb_obj (&b, NULL);
  jb_str (&b, "type", "bookmarks");
  jb_str (&b, "path", B.path);
  jb_int (&b, "max", WEB_BM_MAX);
  jb_arr (&b, "items");
  for (int i = 0; i < B.count; i++) item_json (&b, &B.items[i]);
  jb_arr_end (&b);
  jb_obj_end (&b);
  if (client < 0) web_broadcast (WEB_KIND_CTRL, WS_OP_TEXT, b.buf, b.len, 0);
  else web_send (client, WEB_KIND_CTRL, WS_OP_TEXT, b.buf, b.len);
  jb_free (&b);
}

//bookmark commands from a client; returns 1 when the command was one of ours
int bm_command (int client, const char * c, jv * cmd)
{
  if (!strcmp (c, "bm_list"))
  {
    bm_send (client);
    return 1;
  }

  if (!strcmp (c, "bm_save"))
  {
    web_bookmark bm;
    const char * why = NULL;
    if (!parse_item (cmd, &bm, &why)) { reply (client, "bm_error", "msg", why, 0); return 1; }
    web_bookmark * old = bm.id > 0 ? find (bm.id) : NULL;
    if (!old)
    {
      if (B.count >= WEB_BM_MAX) { reply (client, "bm_error", "msg", "bookmark list is full", 0); return 1; }
      bm.id = B.next_id++;
      old = &B.items[B.count++];
    }
    *old = bm;
    if (save ()) reply (client, "bm_saved", NULL, NULL, bm.id);
    else reply (client, "bm_saved", "warn", "kept for this session only, the bookmark file is not writable", bm.id);
    bm_send (-1);
    return 1;
  }

  if (!strcmp (c, "bm_delete"))
  {
    int id = (int)jv_int (cmd, "id", 0);
    for (int i = 0; i < B.count; i++)
    {
      if (B.items[i].id != id) continue;
      memmove (&B.items[i], &B.items[i+1], sizeof(web_bookmark) * (size_t)(B.count - i - 1));
      B.count--;
      save ();
      bm_send (-1);
      break;
    }
    return 1;
  }

  if (!strcmp (c, "bm_import"))
  {
    //merge a list parsed by the browser (our export, SDR++ or CSV), skipping exact duplicates
    jv * arr = jv_get (cmd, "items");
    int added = 0, skipped = 0;
    for (jv * e = jv_first (arr); e; e = e->next)
    {
      web_bookmark bm;
      const char * why = NULL;
      if (B.count >= WEB_BM_MAX || !parse_item (e, &bm, &why)) { skipped++; continue; }
      int dup = 0;
      for (int k = 0; k < B.count && !dup; k++)
        dup = (B.items[k].freq == bm.freq && strcmp (B.items[k].name, bm.name) == 0);
      if (dup) { skipped++; continue; }
      bm.id = B.next_id++;
      B.items[B.count++] = bm;
      added++;
    }
    if (added) save ();
    char msg[128];
    if (skipped) snprintf (msg, sizeof(msg), "Imported %d bookmarks (%d duplicates or invalid skipped).", added, skipped);
    else snprintf (msg, sizeof(msg), "Imported %d bookmarks.", added);
    reply (client, "bm_imported", "msg", msg, 0);
    bm_send (-1);
    return 1;
  }

  return 0;
}
