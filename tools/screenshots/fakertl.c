// Test stand-in for librtlsdr, loaded with LD_PRELOAD: it synthesizes a band of radio stations, so the
// RTL path of dsd-fme (demodulator, spectrum, waterfall, band scan) runs without a dongle. shoot.mjs
// uses it for the README screenshots.
//
//   cc -O2 -shared -fPIC -o libfakertl.so fakertl.c -lm
//   LD_PRELOAD=$PWD/libfakertl.so FAKERTL_STATIONS="433475000:m17:30,433425000:fm:24" dsd-fme -i rtl ...
//
// FAKERTL_STATIONS lists freq:type:amp[:option...], the frequency in Hz and the carrier amplitude in
// 8-bit steps (the noise sigma is 3). Types: m17 (4FSK at 4800 Bd: M17 stream frames whose LICH
// decodes, carrying a demo LSF from DEMO to @ALL; the voice payload is random), m17burst (the same,
// on for 6 s of every 20), fm (voice-like analog FM), cw (bare carrier), wide (broadcast-like FM), and
// dmr, dpmr, nxdn48 and nxdn96: the protocol's frame sync at its frame period with random symbols
// in between (NXDN with a voice frame's LICH after the sync, so the decoder keeps the carrier). Those
// decode to nothing but hold the decoders' sync.
// Options: key=ON/PERIOD[/OFFSET] keys the station on for ON s of every PERIOD s (starting OFFSET s
// into each period); ct=100.0 adds a CTCSS tone, dcs=023 a DCS code (normal polarity) and dtmf=1234
// a DTMF ANI 0.3 s after each key-up (fm only), e.g. 433450000:fm:24:dcs=023:dtmf=1234:key=4/12.
// Stations outside the capture are left out. The samples come in librtlsdr's default 256 KiB
// transfers, paced in real time.
//
// FAKERTL_IQFILE=rec.u8 plays an rtl_sdr recording instead (8-bit IQ, made at the rate dsd-fme
// asks for, 1.008 MSPS by default, and tuned to the channel + rate/4, where dsd-fme puts the
// dongle), ignoring tuning, and stops dsd-fme with SIGINT at its end.
//
// It defines every rtlsdr_* function dsd-fme imports (shoot.mjs checks that with nm). When dsd-fme
// starts calling a new one, add it here: the real librtlsdr would get this fake device.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <signal.h>

typedef struct rtlsdr_dev rtlsdr_dev_t;
typedef void (*rtlsdr_read_async_cb_t) (unsigned char * buf, uint32_t len, void * ctx);
struct rtlsdr_dev { volatile uint32_t freq; volatile uint32_t rate; volatile int cancel; };
static struct rtlsdr_dev dev0 = { 100000000, 1008000, 0 };

enum { T_M17, T_M17B, T_FM, T_CW, T_WIDE, T_DMR, T_DPMR, T_NXDN48, T_NXDN96 };
typedef struct
{
  double freq, amp, phase; int type; uint32_t seed;
  double key_on, key_period; // keyed on for key_on s of every key_period s (period 0: always on)
  double key_off;            // from this far into each period
  double ct;                 // CTCSS tone in Hz, 0 = none
  int dcs;                   // DCS code, -1 = none
  uint32_t dcs_word;         // its 23-bit word, sent from bit 0
  char dtmf[16];             // ANI sent at each key-up
  uint64_t m17_fn;           // M17: frame number + 1 that m17_d holds
  uint8_t m17_d[184];        // its payload dibits
} station;
static station st[32];
static int nst, loaded;
static uint32_t rng = 0x12345678;

static inline uint32_t xs (uint32_t * s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static inline float unif (void) { return (float)(xs (&rng) & 0xFFFFFF) / 8388608.0f - 1.0f; } // [-1, 1)

// DCS: Golay (23,12) word of the code, the 12 data bits being the 9-bit code and 100; sent LSB first
static uint32_t dcs_word (int code)
{
  uint32_t d = (uint32_t)code | 0x800, r = d << 11;
  for (int i = 22; i >= 11; i--)
    if (r >> i & 1) r ^= 0xC75u << (i - 11);
  return d | (r & 0x7FF) << 12;
}

static void load_stations (void)
{
  const char * env = getenv ("FAKERTL_STATIONS");
  if (loaded || !env) return;
  loaded = 1;
  char buf[2048];
  snprintf (buf, sizeof(buf), "%s", env);
  char * save = NULL;
  for (char * tok = strtok_r (buf, ",", &save); tok && nst < 32; tok = strtok_r (NULL, ",", &save))
  {
    char type[16] = "";
    double f = 0, a = 20;
    if (sscanf (tok, "%lf:%15[^:]:%lf", &f, type, &a) < 2) continue;
    int t = !strcmp (type, "m17") ? T_M17 : !strcmp (type, "m17burst") ? T_M17B : !strcmp (type, "fm") ? T_FM
          : !strcmp (type, "cw") ? T_CW : !strcmp (type, "wide") ? T_WIDE : !strcmp (type, "dmr") ? T_DMR
          : !strcmp (type, "dpmr") ? T_DPMR : !strcmp (type, "nxdn48") ? T_NXDN48 : !strcmp (type, "nxdn96") ? T_NXDN96 : -1;
    if (t < 0) { fprintf (stderr, "fakertl: unknown station type %s\n", type); continue; }
    station * s = &st[nst++];
    memset (s, 0, sizeof(*s));
    s->freq = f; s->amp = a; s->type = t; s->seed = 0x9E3779B9u * (uint32_t)nst; s->dcs = -1;
    if (t == T_M17B) { s->key_on = 6; s->key_period = 20; }
    //options after freq:type:amp
    char * p = strchr (tok, ':');
    if (p) p = strchr (p + 1, ':');
    if (p) p = strchr (p + 1, ':');
    while (p)
    {
      p++;
      if (!strncmp (p, "key=", 4)) sscanf (p + 4, "%lf/%lf/%lf", &s->key_on, &s->key_period, &s->key_off);
      else if (!strncmp (p, "ct=", 3)) s->ct = atof (p + 3);
      else if (!strncmp (p, "dcs=", 4)) { s->dcs = (int)strtol (p + 4, NULL, 8); s->dcs_word = dcs_word (s->dcs); }
      else if (!strncmp (p, "dtmf=", 5)) sscanf (p + 5, "%15[0-9A-D*#]", s->dtmf);
      else fprintf (stderr, "fakertl: unknown option %s\n", p);
      p = strchr (p, ':');
    }
    fprintf (stderr, "fakertl: station %.4f MHz %s amp %.0f", f / 1e6, type, a);
    if (s->key_period > 0) fprintf (stderr, " keyed %.1f/%.1f s from %.1f s", s->key_on, s->key_period, s->key_off);
    if (s->ct > 0) fprintf (stderr, " CTCSS %.1f", s->ct);
    if (s->dcs >= 0) fprintf (stderr, " DCS %03o", s->dcs);
    if (s->dtmf[0]) fprintf (stderr, " ANI %s", s->dtmf);
    fprintf (stderr, "\n");
  }
}

// The LSF the M17 frames carry in their LICH: @ALL from DEMO, stream, voice 3200, no encryption
static uint8_t m17_lsf[30];
static void m17_lsf_init (void)
{
  uint64_t dst = 0xFFFFFFFFFFFFull, src = 4 + 5 * 40ull + 13 * 1600ull + 15 * 64000ull; // base 40 "DEMO"
  memset (m17_lsf, 0, sizeof(m17_lsf));
  for (int i = 0; i < 6; i++) { m17_lsf[i] = (uint8_t)(dst >> (40 - 8 * i)); m17_lsf[6+i] = (uint8_t)(src >> (40 - 8 * i)); }
  m17_lsf[13] = 0x05;
  uint32_t crc = 0xFFFF;
  for (int i = 0; i < 28; i++)
  {
    crc ^= (uint32_t)m17_lsf[i] << 8;
    for (int j = 0; j < 8; j++) { crc <<= 1; if (crc & 0x10000) crc = (crc ^ 0x5935) & 0xFFFF; }
  }
  m17_lsf[28] = (uint8_t)(crc >> 8); m17_lsf[29] = (uint8_t)crc;
}

// One stream frame's 184 payload dibits: the LICH (a sixth of the LSF and its counter as four Golay
// (24,12) words) and random bits for the rest, interleaved and randomized as the M17 spec says
static void m17_frame (station * s, uint64_t fn)
{
  static const uint16_t gpar[12] = { 0xC75, 0x63B, 0xF68, 0x7B4, 0x3DA, 0xD99, 0x6CD, 0x367, 0xDC6, 0xA97, 0x93E, 0x8EB };
  static const uint8_t rnd[46] = {
    0xD6, 0xB5, 0xE2, 0x30, 0x82, 0xFF, 0x84, 0x62, 0xBA, 0x4E, 0x96, 0x90, 0xD8, 0x98, 0xDD, 0x5D,
    0x0C, 0xC8, 0x52, 0x43, 0x91, 0x1D, 0xF8, 0x6E, 0x68, 0x2F, 0x35, 0xDA, 0x14, 0xEA, 0xCD, 0x76,
    0x19, 0x8D, 0xD5, 0x80, 0xD1, 0x33, 0x87, 0x13, 0x57, 0x18, 0x2D, 0x29, 0x78, 0xC3 };
  if (!m17_lsf[13]) m17_lsf_init ();
  uint8_t lich[48] = { 0 }, bits[368], il[368];
  int chunk = (int)(fn % 6);
  for (int i = 0; i < 40; i++) { int b = chunk * 40 + i; lich[i] = (m17_lsf[b / 8] >> (7 - b % 8)) & 1; }
  for (int i = 0; i < 3; i++) lich[40 + i] = (chunk >> (2 - i)) & 1;
  for (int w = 0; w < 4; w++)
  {
    uint16_t par = 0;
    for (int j = 0; j < 12; j++) { bits[w * 24 + j] = lich[w * 12 + j]; if (lich[w * 12 + j]) par ^= gpar[j]; }
    for (int j = 0; j < 12; j++) bits[w * 24 + 12 + j] = (par >> (11 - j)) & 1;
  }
  uint32_t h = s->seed ^ (uint32_t)(fn * 2654435761u);
  if (!h) h = 1;
  for (int i = 96; i < 368; i++) bits[i] = xs (&h) & 1;
  for (int i = 0; i < 368; i++) il[(45 * i + 92 * i * i) % 368] = bits[i];
  for (int i = 0; i < 368; i++) il[i] ^= (rnd[i / 8] >> (7 - i % 8)) & 1;
  for (int k = 0; k < 184; k++) s->m17_d[k] = (uint8_t)(il[2 * k] << 1 | il[2 * k + 1]);
  s->m17_fn = fn + 1;
}

// M17 stream frames: 8 symbols of sync (0xFF5D) and 184 of payload at 4800 Bd, +-800/+-2400 Hz
static double m17_dev (station * s, double t)
{
  static const int sync[8] = { -3, -3, -3, -3, 3, 3, -3, 3 };
  static const int map[4] = { 1, 3, -1, -3 };
  uint64_t sym = (uint64_t)(t * 4800.0), fn = sym / 192;
  int pos = (int)(sym % 192);
  if (pos < 8) return sync[pos] * 800.0;
  if (s->m17_fn != fn + 1) m17_frame (s, fn);
  return map[s->m17_d[pos - 8]] * 800.0;
}

// Other protocols' frames: the sync word every period symbols ('1' = +3, '3' = -3, as DSD-FME writes
// them, and 'a' = +1, 'b' = -1 for the inner symbols some words have), then random 4FSK symbols; dev
// is Hz per level step (the outer level is 3 dev)
typedef struct { int baud, period; const char * sync; double dev; int lich; } syn_def;
static const syn_def syn[] = {
  { 4800, 144, "313333111331131131331131", 648, 0 }, // dmr: a repeater's data sync in every 30 ms burst
  { 2400, 384, "113333131331", 350, 0 },             // dpmr: FS2 every other 80 ms frame
  { 2400, 192, "3a313311b1", 350, 1 },               // nxdn48: FSW (0xCDF59) and LICH every 80 ms
  { 4800, 192, "3a313311b1", 800, 1 },               // nxdn96: every 40 ms
};

// NXDN: a voice frame's LICH (0x56, RTCH with voice in both halves, parity 0) the way DSD-FME reads
// it: the bits on the dibits' high bits, the low bits 1, and the dibits scrambled by the PN9 sequence
// (seed 228): a scrambled dibit is inverted
static int nxdn_lich_level (int i)
{
  static const int map[4] = { 1, 3, -1, -3 };
  int lich_full = 0x56 << 1;
  uint16_t lfsr = 228;
  int d = 0;
  for (int k = 0; k <= i; k++)
  {
    d = (lich_full >> (7 - k) & 1) << 1 | 1;
    if (lfsr & 1) d ^= 2;
    int bit = ((lfsr >> 4) ^ lfsr) & 1;
    lfsr = (uint16_t)(lfsr >> 1 | bit << 8);
  }
  return map[d];
}

static double syn_dev (station * s, double t)
{
  static const int lev[4] = { -3, -1, 1, 3 };
  const syn_def * d = &syn[s->type - T_DMR];
  uint64_t sym = (uint64_t)(t * d->baud);
  int pos = (int)(sym % (uint64_t)d->period), len = (int)strlen (d->sync);
  if (pos < len)
  {
    char c = d->sync[pos];
    return (c == '1' ? 3 : c == '3' ? -3 : c == 'a' ? 1 : -1) * d->dev;
  }
  if (d->lich && pos < len + 8) return nxdn_lich_level (pos - len) * d->dev;
  uint32_t h = s->seed ^ (uint32_t)(sym * 2654435761u);
  if (!h) h = 1;
  xs (&h);
  return lev[xs (&h) & 3] * d->dev;
}

static void dtmf_tones (char c, double * lo, double * hi)
{
  static const char keys[] = "123A456B789C*0#D";
  static const double row[4] = { 697, 770, 852, 941 }, col[4] = { 1209, 1336, 1477, 1633 };
  const char * k = strchr (keys, c);
  int i = k ? (int)(k - keys) : 13;
  *lo = row[i / 4]; *hi = col[i % 4];
}

// seconds since the station keyed up (t itself when it isn't keyed)
static double keyed_for (const station * s, double t)
{
  if (s->key_period <= 0) return t;
  double r = fmod (t - s->key_off, s->key_period);
  return r < 0 ? r + s->key_period : r;
}

// analog FM: CTCSS or DCS all through, the ANI 0.3 s after key-up (50 ms per digit, 50 ms apart),
// then voice
static double fm_dev (station * s, double t)
{
  double tr = keyed_for (s, t), d = 0;
  if (s->ct > 0) d += 600.0 * sin (2 * M_PI * s->ct * t);
  if (s->dcs >= 0)
  {
    double x = t * 134.4, fr = x - floor (x);
    uint64_t b = (uint64_t)x;
    double cur = (s->dcs_word >> (b % 23) & 1) ? 1 : -1, prev = (s->dcs_word >> ((b + 22) % 23) & 1) ? 1 : -1;
    d += 600.0 * (fr < 0.3 ? prev + (cur - prev) * (1 - cos (M_PI * fr / 0.3)) / 2 : cur); // rounded edges
  }
  int nd = (int)strlen (s->dtmf);
  if (nd && tr < 0.4 + 0.1 * nd)
  {
    double u = tr - 0.3;
    if (u >= 0 && u < 0.1 * nd && u - 0.1 * (int)(u / 0.1) < 0.05)
    {
      double lo, hi;
      dtmf_tones (s->dtmf[(int)(u / 0.1)], &lo, &hi);
      d += 1200.0 * (sin (2 * M_PI * lo * t) + sin (2 * M_PI * hi * t));
    }
    return d;
  }
  return d + 2500.0 * (0.6 + 0.4 * sin (2 * M_PI * 3 * t)) *
             (0.5 * sin (2 * M_PI * 420 * t) + 0.3 * sin (2 * M_PI * 1170 * t) + 0.2 * sin (2 * M_PI * 2350 * t));
}

// frequency deviation (Hz) at time t
static double station_dev (station * s, double t)
{
  switch (s->type)
  {
    case T_M17: case T_M17B: return m17_dev (s, t);
    case T_DMR: case T_DPMR: case T_NXDN48: case T_NXDN96: return syn_dev (s, t);
    case T_FM: return fm_dev (s, t);
    case T_WIDE: return 60000.0 * (0.4 * sin (2 * M_PI * 1000 * t) + 0.3 * sin (2 * M_PI * 4700 * t) + 0.3 * sin (2 * M_PI * 13000 * t));
  }
  return 0;
}

int rtlsdr_open (rtlsdr_dev_t ** dev, uint32_t index) { (void)index; load_stations (); *dev = &dev0; return 0; }
int rtlsdr_close (rtlsdr_dev_t * dev) { (void)dev; return 0; }
int rtlsdr_set_center_freq (rtlsdr_dev_t * dev, uint32_t freq) { dev->freq = freq; return 0; }
uint32_t rtlsdr_get_center_freq (rtlsdr_dev_t * dev) { return dev->freq; }
int rtlsdr_set_sample_rate (rtlsdr_dev_t * dev, uint32_t rate) { dev->rate = rate; return 0; }
uint32_t rtlsdr_get_sample_rate (rtlsdr_dev_t * dev) { return dev->rate; }
int rtlsdr_set_tuner_gain_mode (rtlsdr_dev_t * dev, int manual) { (void)dev; (void)manual; return 0; }
int rtlsdr_set_tuner_gain (rtlsdr_dev_t * dev, int gain) { (void)dev; (void)gain; return 0; }
int rtlsdr_get_tuner_gains (rtlsdr_dev_t * dev, int * gains)
{
  static const int g[] = { 0, 90, 197, 280, 372, 496 };
  (void)dev;
  if (gains) memcpy (gains, g, sizeof(g));
  return 6;
}
int rtlsdr_set_agc_mode (rtlsdr_dev_t * dev, int on) { (void)dev; (void)on; return 0; }
int rtlsdr_set_freq_correction (rtlsdr_dev_t * dev, int ppm) { (void)dev; (void)ppm; return 0; }
int rtlsdr_reset_buffer (rtlsdr_dev_t * dev) { (void)dev; return 0; }
int rtlsdr_set_direct_sampling (rtlsdr_dev_t * dev, int on) { (void)dev; (void)on; return 0; }
int rtlsdr_set_offset_tuning (rtlsdr_dev_t * dev, int on) { (void)dev; (void)on; return 0; }
int rtlsdr_cancel_async (rtlsdr_dev_t * dev) { dev->cancel = 1; return 0; }
uint32_t rtlsdr_get_device_count (void) { return 1; }
int rtlsdr_get_device_usb_strings (uint32_t i, char * m, char * p, char * s)
{
  (void)i; strcpy (m, "fake"); strcpy (p, "fakertl"); strcpy (s, "0"); return 0;
}

int rtlsdr_read_async (rtlsdr_dev_t * dev, rtlsdr_read_async_cb_t cb, void * ctx, uint32_t buf_num, uint32_t buf_len)
{
  (void)buf_num;
  if (!buf_len) buf_len = 16 * 32 * 512; // librtlsdr's default: 130 ms at 1 MSPS
  unsigned char * buf = malloc (buf_len);
  if (!buf) return -1;
  struct timespec t0, now;
  clock_gettime (CLOCK_MONOTONIC, &t0);
  double tsim = 0; // seconds of signal delivered so far, whatever the rate was
  dev->cancel = 0;
  const char * iqf = getenv ("FAKERTL_IQFILE");
  FILE * iq = iqf ? fopen (iqf, "rb") : NULL;
  int playing = iq != NULL;
  if (iqf && !iq) fprintf (stderr, "fakertl: cannot open %s\n", iqf);
  while (!dev->cancel && iq)
  {
    if (fread (buf, 1, buf_len, iq) != buf_len) { fclose (iq); free (buf); raise (SIGINT); return 0; }
    tsim += buf_len / 2.0 / dev->rate;
    clock_gettime (CLOCK_MONOTONIC, &now);
    double ahead = tsim - ((now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) * 1e-9);
    if (ahead > 0) { struct timespec ts = { (time_t)ahead, (long)((ahead - (time_t)ahead) * 1e9) }; nanosleep (&ts, NULL); }
    if (!dev->cancel) cb (buf, buf_len, ctx);
  }
  if (iq) fclose (iq); //cancelled (a rate change restarts the read, and the recording from its start)
  while (!dev->cancel && !playing)
  {
    double fs = dev->rate, center = dev->freq;
    uint32_t n = buf_len / 2;
    int on[32];
    for (int k = 0; k < nst; k++)
      on[k] = fabs (st[k].freq - center) < fs / 2 && (st[k].key_period <= 0 || keyed_for (&st[k], tsim) < st[k].key_on);
    for (uint32_t i = 0; i < n; i++)
    {
      double t = tsim + i / fs;
      float I = (unif () + unif () + unif () + unif ()) * 2.6f, Q = (unif () + unif () + unif () + unif ()) * 2.6f;
      for (int k = 0; k < nst; k++)
      {
        if (!on[k]) continue;
        station * s = &st[k];
        s->phase = remainder (s->phase + 2 * M_PI * ((s->freq - center) + station_dev (s, t)) / fs, 2 * M_PI);
        I += (float)(s->amp * cos (s->phase));
        Q += (float)(s->amp * sin (s->phase));
      }
      int iv = (int)lrintf (127.5f + I), qv = (int)lrintf (127.5f + Q);
      buf[2*i]   = (unsigned char)(iv < 0 ? 0 : iv > 255 ? 255 : iv);
      buf[2*i+1] = (unsigned char)(qv < 0 ? 0 : qv > 255 ? 255 : qv);
    }
    tsim += n / fs;
    // hand the transfer over when a dongle would have
    clock_gettime (CLOCK_MONOTONIC, &now);
    double ahead = tsim - ((now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) * 1e-9);
    if (ahead > 0) { struct timespec ts = { (time_t)ahead, (long)((ahead - (time_t)ahead) * 1e9) }; nanosleep (&ts, NULL); }
    if (!dev->cancel) cb (buf, buf_len, ctx);
  }
  free (buf);
  return 0;
}
