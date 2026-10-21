/*-------------------------------------------------------------------------------
 * dsd_web_json.c
 * Minimal JSON parser and builder for the DSD-FME web interface
 *
 * The parser builds a small tree of jv nodes, the builder appends to a
 * growable buffer and always emits valid UTF-8 (decoder strings can carry
 * arbitrary bytes, and browsers drop websocket connections on bad UTF-8).
 *-----------------------------------------------------------------------------*/

#include "dsd_web.h"
#include <stdarg.h>

//----------------------------------------------------------------------------
// Parser
//----------------------------------------------------------------------------

typedef struct
{
  const char * s;
  size_t len;
  size_t pos;
  int depth;
  int err;
} jp;

static jv * jp_value (jp * p);

static void jp_ws (jp * p)
{
  while (p->pos < p->len)
  {
    char c = p->s[p->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
    else break;
  }
}

static jv * jv_new (jv_type t)
{
  jv * v = calloc (1, sizeof(jv));
  if (v) v->t = t;
  return v;
}

static int jp_hex4 (jp * p, unsigned int * out)
{
  unsigned int v = 0;
  if (p->pos + 4 > p->len) return 0;
  for (int i = 0; i < 4; i++)
  {
    char c = p->s[p->pos++];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned int)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned int)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned int)(c - 'A' + 10);
    else return 0;
  }
  *out = v;
  return 1;
}

static size_t utf8_put (char * o, unsigned int cp)
{
  if (cp < 0x80) { o[0] = (char)cp; return 1; }
  if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
  o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

static char * jp_string (jp * p)
{
  //expects p->s[p->pos] == '"'
  p->pos++;
  size_t cap = 32, n = 0;
  char * out = malloc (cap);
  if (!out) { p->err = 1; return NULL; }
  while (p->pos < p->len)
  {
    char c = p->s[p->pos++];
    if (c == '"')
    {
      out[n] = 0;
      return out;
    }
    if (n + 8 >= cap)
    {
      cap *= 2;
      char * t = realloc (out, cap);
      if (!t) { free (out); p->err = 1; return NULL; }
      out = t;
    }
    if (c == '\\')
    {
      if (p->pos >= p->len) break;
      char e = p->s[p->pos++];
      switch (e)
      {
        case '"':  out[n++] = '"'; break;
        case '\\': out[n++] = '\\'; break;
        case '/':  out[n++] = '/'; break;
        case 'b':  out[n++] = '\b'; break;
        case 'f':  out[n++] = '\f'; break;
        case 'n':  out[n++] = '\n'; break;
        case 'r':  out[n++] = '\r'; break;
        case 't':  out[n++] = '\t'; break;
        case 'u':
        {
          unsigned int cp = 0;
          if (!jp_hex4 (p, &cp)) { free (out); p->err = 1; return NULL; }
          //surrogate pair
          if (cp >= 0xD800 && cp <= 0xDBFF && p->pos + 6 <= p->len && p->s[p->pos] == '\\' && p->s[p->pos+1] == 'u')
          {
            unsigned int lo = 0;
            p->pos += 2;
            if (!jp_hex4 (p, &lo)) { free (out); p->err = 1; return NULL; }
            if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            else cp = 0xFFFD;
          }
          else if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
          n += utf8_put (out + n, cp);
          break;
        }
        default:
          free (out);
          p->err = 1;
          return NULL;
      }
    }
    else out[n++] = c;
  }
  free (out);
  p->err = 1;
  return NULL;
}

static jv * jp_number (jp * p)
{
  size_t start = p->pos;
  if (p->pos < p->len && (p->s[p->pos] == '-' || p->s[p->pos] == '+')) p->pos++;
  while (p->pos < p->len)
  {
    char c = p->s[p->pos];
    if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') p->pos++;
    else break;
  }
  size_t n = p->pos - start;
  if (n == 0 || n > 64) { p->err = 1; return NULL; }
  jv * v = jv_new (JV_NUM);
  if (!v) { p->err = 1; return NULL; }
  v->str = malloc (n + 1);
  if (!v->str) { free (v); p->err = 1; return NULL; }
  memcpy (v->str, p->s + start, n);
  v->str[n] = 0;
  v->num = strtod (v->str, NULL);
  return v;
}

static int jp_lit (jp * p, const char * lit)
{
  size_t n = strlen (lit);
  if (p->pos + n > p->len) return 0;
  if (strncmp (p->s + p->pos, lit, n) != 0) return 0;
  p->pos += n;
  return 1;
}

static jv * jp_value (jp * p)
{
  jp_ws (p);
  if (p->pos >= p->len || p->depth > 64) { p->err = 1; return NULL; }
  char c = p->s[p->pos];
  if (c == '{' || c == '[')
  {
    int is_obj = (c == '{');
    jv * v = jv_new (is_obj ? JV_OBJ : JV_ARR);
    jv * last = NULL;
    if (!v) { p->err = 1; return NULL; }
    p->pos++;
    p->depth++;
    jp_ws (p);
    if (p->pos < p->len && p->s[p->pos] == (is_obj ? '}' : ']'))
    {
      p->pos++;
      p->depth--;
      return v;
    }
    while (!p->err)
    {
      char * key = NULL;
      if (is_obj)
      {
        jp_ws (p);
        if (p->pos >= p->len || p->s[p->pos] != '"') { p->err = 1; break; }
        key = jp_string (p);
        if (!key) break;
        jp_ws (p);
        if (p->pos >= p->len || p->s[p->pos] != ':') { free (key); p->err = 1; break; }
        p->pos++;
      }
      jv * child = jp_value (p);
      if (!child) { free (key); p->err = 1; break; }
      child->key = key;
      if (last) last->next = child;
      else v->child = child;
      last = child;
      jp_ws (p);
      if (p->pos >= p->len) { p->err = 1; break; }
      if (p->s[p->pos] == ',') { p->pos++; continue; }
      if (p->s[p->pos] == (is_obj ? '}' : ']')) { p->pos++; break; }
      p->err = 1;
    }
    p->depth--;
    if (p->err) { jv_free (v); return NULL; }
    return v;
  }
  if (c == '"')
  {
    jv * v = jv_new (JV_STR);
    if (!v) { p->err = 1; return NULL; }
    v->str = jp_string (p);
    if (!v->str) { free (v); return NULL; }
    return v;
  }
  if (c == 't' && jp_lit (p, "true"))  { jv * v = jv_new (JV_BOOL); if (v) v->b = 1; return v; }
  if (c == 'f' && jp_lit (p, "false")) { jv * v = jv_new (JV_BOOL); if (v) v->b = 0; return v; }
  if (c == 'n' && jp_lit (p, "null"))  { return jv_new (JV_NULL); }
  if (c == '-' || (c >= '0' && c <= '9')) return jp_number (p);
  p->err = 1;
  return NULL;
}

jv * jv_parse (const char * s, size_t len)
{
  jp p;
  p.s = s; p.len = len; p.pos = 0; p.depth = 0; p.err = 0;
  jv * v = jp_value (&p);
  if (p.err) { jv_free (v); return NULL; }
  return v;
}

void jv_free (jv * v)
{
  while (v)
  {
    jv * next = v->next;
    if (v->child) jv_free (v->child);
    free (v->str);
    free (v->key);
    free (v);
    v = next;
  }
}

jv * jv_get (jv * obj, const char * key)
{
  if (!obj || obj->t != JV_OBJ || !key) return NULL;
  for (jv * c = obj->child; c; c = c->next)
    if (c->key && strcmp (c->key, key) == 0) return c;
  return NULL;
}

jv * jv_at (jv * arr, int index)
{
  if (!arr || (arr->t != JV_ARR && arr->t != JV_OBJ)) return NULL;
  int i = 0;
  for (jv * c = arr->child; c; c = c->next, i++)
    if (i == index) return c;
  return NULL;
}

//first element of an array (or member of an object), walk the rest with ->next;
//loops over jv_count / jv_at are quadratic on the big arrays a client can send
jv * jv_first (jv * arr)
{
  if (!arr || (arr->t != JV_ARR && arr->t != JV_OBJ)) return NULL;
  return arr->child;
}

int jv_count (jv * arr)
{
  int n = 0;
  if (!arr || (arr->t != JV_ARR && arr->t != JV_OBJ)) return 0;
  for (jv * c = arr->child; c; c = c->next) n++;
  return n;
}

int jv_has (jv * obj, const char * key)
{
  return jv_get (obj, key) != NULL;
}

const char * jv_str (jv * obj, const char * key, const char * def)
{
  jv * v = jv_get (obj, key);
  if (!v) return def;
  if (v->t == JV_STR || v->t == JV_NUM) return v->str;
  return def;
}

double jv_num (jv * obj, const char * key, double def)
{
  jv * v = jv_get (obj, key);
  if (!v) return def;
  if (v->t == JV_NUM) return v->num;
  if (v->t == JV_BOOL) return v->b;
  if (v->t == JV_STR && v->str[0]) return strtod (v->str, NULL);
  return def;
}

long long jv_int (jv * obj, const char * key, long long def)
{
  jv * v = jv_get (obj, key);
  if (!v) return def;
  if (v->t == JV_NUM)
  {
    //prefer exact integer parse when the text has no fraction or exponent
    if (strpbrk (v->str, ".eE") == NULL) return strtoll (v->str, NULL, 10);
    return (long long)llround (v->num);
  }
  if (v->t == JV_BOOL) return v->b;
  if (v->t == JV_STR && v->str[0]) return strtoll (v->str, NULL, 0);
  return def;
}

int jv_bool (jv * obj, const char * key, int def)
{
  jv * v = jv_get (obj, key);
  if (!v) return def;
  if (v->t == JV_BOOL) return v->b;
  if (v->t == JV_NUM) return v->num != 0;
  if (v->t == JV_STR) return (strcmp (v->str, "true") == 0 || strcmp (v->str, "1") == 0 || strcmp (v->str, "on") == 0);
  return def;
}

//----------------------------------------------------------------------------
// Builder
//----------------------------------------------------------------------------

void jb_init (jb * b)
{
  memset (b, 0, sizeof(jb));
  b->first[0] = 1;
}

void jb_free (jb * b)
{
  free (b->buf);
  memset (b, 0, sizeof(jb));
}

void jb_reset (jb * b)
{
  b->len = 0;
  b->depth = 0;
  b->first[0] = 1;
  if (b->buf) b->buf[0] = 0;
}

static void jb_grow (jb * b, size_t need)
{
  if (b->len + need + 1 <= b->cap) return;
  size_t ncap = b->cap ? b->cap : 1024;
  while (ncap < b->len + need + 1) ncap *= 2;
  char * t = realloc (b->buf, ncap);
  if (!t) return;
  b->buf = t;
  b->cap = ncap;
}

void jb_rawn (jb * b, const char * s, size_t n)
{
  jb_grow (b, n);
  if (b->len + n + 1 > b->cap) return;
  memcpy (b->buf + b->len, s, n);
  b->len += n;
  b->buf[b->len] = 0;
}

void jb_raw (jb * b, const char * s)
{
  jb_rawn (b, s, strlen (s));
}

void jb_printf (jb * b, const char * fmt, ...)
{
  char tmp[512];
  va_list ap;
  va_start (ap, fmt);
  int n = vsnprintf (tmp, sizeof(tmp), fmt, ap);
  va_end (ap);
  if (n < 0) return;
  if ((size_t)n < sizeof(tmp))
  {
    jb_rawn (b, tmp, (size_t)n);
    return;
  }
  char * big = malloc ((size_t)n + 1);
  if (!big) return;
  va_start (ap, fmt);
  vsnprintf (big, (size_t)n + 1, fmt, ap);
  va_end (ap);
  jb_rawn (b, big, (size_t)n);
  free (big);
}

//decode one UTF-8 sequence, returns length consumed or 0 if invalid
static int utf8_valid_seq (const unsigned char * s, size_t n)
{
  unsigned char c = s[0];
  if (c < 0x80) return 1;
  if (c >= 0xC2 && c <= 0xDF)
  {
    if (n >= 2 && (s[1] & 0xC0) == 0x80) return 2;
    return 0;
  }
  if (c >= 0xE0 && c <= 0xEF)
  {
    if (n < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
    if (c == 0xE0 && s[1] < 0xA0) return 0; //overlong
    if (c == 0xED && s[1] >= 0xA0) return 0; //surrogates
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4)
  {
    if (n < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
    if (c == 0xF0 && s[1] < 0x90) return 0;
    if (c == 0xF4 && s[1] >= 0x90) return 0;
    return 4;
  }
  return 0;
}

void jb_escape (jb * b, const char * str, size_t n)
{
  const unsigned char * s = (const unsigned char *)str;
  size_t i = 0;
  jb_grow (b, n + 16);
  while (i < n)
  {
    unsigned char c = s[i];
    if (c == 0) break;
    if (c == '"') { jb_rawn (b, "\\\"", 2); i++; continue; }
    if (c == '\\') { jb_rawn (b, "\\\\", 2); i++; continue; }
    if (c == '\n') { jb_rawn (b, "\\n", 2); i++; continue; }
    if (c == '\r') { jb_rawn (b, "\\r", 2); i++; continue; }
    if (c == '\t') { jb_rawn (b, "\\t", 2); i++; continue; }
    if (c < 0x20 || c == 0x7F)
    {
      char u[8];
      snprintf (u, sizeof(u), "\\u%04x", c);
      jb_rawn (b, u, 6);
      i++;
      continue;
    }
    if (c < 0x80) { jb_rawn (b, (const char *)&s[i], 1); i++; continue; }
    int l = utf8_valid_seq (s + i, n - i);
    if (l > 0)
    {
      jb_rawn (b, (const char *)&s[i], (size_t)l);
      i += (size_t)l;
    }
    else
    {
      //treat stray bytes as Latin-1 so text stays readable
      char u[8];
      size_t k = utf8_put (u, c);
      jb_rawn (b, u, k);
      i++;
    }
  }
}

static void jb_sep (jb * b)
{
  if (b->depth >= 0 && b->depth < 32)
  {
    if (!b->first[b->depth]) jb_rawn (b, ",", 1);
    b->first[b->depth] = 0;
  }
}

void jb_key (jb * b, const char * key)
{
  jb_sep (b);
  if (key)
  {
    jb_rawn (b, "\"", 1);
    jb_escape (b, key, strlen (key));
    jb_rawn (b, "\":", 2);
  }
}

void jb_obj (jb * b, const char * key)
{
  jb_key (b, key);
  jb_rawn (b, "{", 1);
  if (b->depth < 31) b->depth++;
  b->first[b->depth] = 1;
}

void jb_obj_end (jb * b)
{
  jb_rawn (b, "}", 1);
  if (b->depth > 0) b->depth--;
}

void jb_arr (jb * b, const char * key)
{
  jb_key (b, key);
  jb_rawn (b, "[", 1);
  if (b->depth < 31) b->depth++;
  b->first[b->depth] = 1;
}

void jb_arr_end (jb * b)
{
  jb_rawn (b, "]", 1);
  if (b->depth > 0) b->depth--;
}

void jb_strn (jb * b, const char * key, const char * s, size_t n)
{
  jb_key (b, key);
  jb_rawn (b, "\"", 1);
  if (s) jb_escape (b, s, n);
  jb_rawn (b, "\"", 1);
}

void jb_str (jb * b, const char * key, const char * s)
{
  jb_strn (b, key, s, s ? strlen (s) : 0);
}

void jb_int (jb * b, const char * key, long long v)
{
  jb_key (b, key);
  jb_printf (b, "%lld", v);
}

void jb_uint (jb * b, const char * key, unsigned long long v)
{
  jb_key (b, key);
  jb_printf (b, "%llu", v);
}

void jb_dbl (jb * b, const char * key, double v, int prec)
{
  jb_key (b, key);
  if (isnan (v) || isinf (v)) jb_rawn (b, "null", 4);
  else jb_printf (b, "%.*f", prec, v);
}

void jb_bool (jb * b, const char * key, int v)
{
  jb_key (b, key);
  if (v) jb_rawn (b, "true", 4);
  else jb_rawn (b, "false", 5);
}

void jb_null (jb * b, const char * key)
{
  jb_key (b, key);
  jb_rawn (b, "null", 4);
}

void jb_hex (jb * b, const char * key, unsigned long long v, int width)
{
  jb_key (b, key);
  jb_printf (b, "\"%0*llX\"", width, v);
}
