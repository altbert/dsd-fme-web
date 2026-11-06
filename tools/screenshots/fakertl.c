// Test stand-in for librtlsdr, loaded with LD_PRELOAD: it synthesizes a band of radio stations, so the
// RTL path of dsd-fme (demodulator, spectrum, waterfall, band scan) runs without a dongle. shoot.mjs
// uses it for the README screenshots.
//
//   cc -O2 -shared -fPIC -o libfakertl.so fakertl.c -lm
//   LD_PRELOAD=$PWD/libfakertl.so FAKERTL_STATIONS="433475000:m17:30,433425000:fm:24" dsd-fme -i rtl ...
//
// FAKERTL_STATIONS lists freq:type:amp, the frequency in Hz and the carrier amplitude in 8-bit steps
// (the noise sigma is 3). Types: m17 (4FSK at 4800 Bd: M17 stream sync words, random payload),
// m17burst (the same, on for 6 s of every 20), fm (voice-like analog FM), cw (bare carrier) and wide
// (broadcast-like FM). Stations outside the capture are left out. The samples come in librtlsdr's
// default 256 KiB transfers, paced in real time.
//
// It defines every rtlsdr_* function dsd-fme imports (shoot.mjs checks that with nm). When dsd-fme
// starts calling a new one, add it here: the real librtlsdr would get this fake device.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

typedef struct rtlsdr_dev rtlsdr_dev_t;
typedef void (*rtlsdr_read_async_cb_t) (unsigned char * buf, uint32_t len, void * ctx);
struct rtlsdr_dev { volatile uint32_t freq; volatile uint32_t rate; volatile int cancel; };
static struct rtlsdr_dev dev0 = { 100000000, 1008000, 0 };

enum { T_M17, T_M17B, T_FM, T_CW, T_WIDE };
typedef struct { double freq, amp, phase; int type; uint32_t seed; } station;
static station st[32];
static int nst, loaded;
static uint32_t rng = 0x12345678;

static inline uint32_t xs (uint32_t * s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static inline float unif (void) { return (float)(xs (&rng) & 0xFFFFFF) / 8388608.0f - 1.0f; } // [-1, 1)

static void load_stations (void)
{
  const char * env = getenv ("FAKERTL_STATIONS");
  if (loaded || !env) return;
  loaded = 1;
  char buf[2048];
  snprintf (buf, sizeof(buf), "%s", env);
  for (char * tok = strtok (buf, ","); tok && nst < 32; tok = strtok (NULL, ","))
  {
    char type[16] = "";
    double f = 0, a = 20;
    if (sscanf (tok, "%lf:%15[^:]:%lf", &f, type, &a) < 2) continue;
    int t = !strcmp (type, "m17") ? T_M17 : !strcmp (type, "m17burst") ? T_M17B : !strcmp (type, "fm") ? T_FM
          : !strcmp (type, "cw") ? T_CW : !strcmp (type, "wide") ? T_WIDE : -1;
    if (t < 0) { fprintf (stderr, "fakertl: unknown station type %s\n", type); continue; }
    station * s = &st[nst++];
    s->freq = f; s->amp = a; s->phase = 0; s->type = t; s->seed = 0x9E3779B9u * (uint32_t)nst;
    fprintf (stderr, "fakertl: station %.4f MHz %s amp %.0f\n", f / 1e6, type, a);
  }
}

// M17 stream frames: 8 symbols of sync (0xFF5D) and 184 of payload at 4800 Bd, +-800/+-2400 Hz
static double m17_dev (station * s, double t)
{
  static const int sync[8] = { -3, -3, -3, -3, 3, 3, -3, 3 };
  static const int map[4] = { 1, 3, -1, -3 };
  uint64_t sym = (uint64_t)(t * 4800.0);
  int pos = (int)(sym % 192);
  if (pos < 8) return sync[pos] * 800.0;
  uint32_t h = (uint32_t)(sym * 2654435761u) ^ s->seed;
  h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
  return map[h & 3] * 800.0;
}

// frequency deviation (Hz) at time t
static double station_dev (station * s, double t)
{
  switch (s->type)
  {
    case T_M17: case T_M17B: return m17_dev (s, t);
    case T_FM: return 2500.0 * (0.6 + 0.4 * sin (2 * M_PI * 3 * t)) *
                      (0.5 * sin (2 * M_PI * 420 * t) + 0.3 * sin (2 * M_PI * 1170 * t) + 0.2 * sin (2 * M_PI * 2350 * t));
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
  while (!dev->cancel)
  {
    double fs = dev->rate, center = dev->freq;
    uint32_t n = buf_len / 2;
    int on[32];
    for (int k = 0; k < nst; k++)
      on[k] = fabs (st[k].freq - center) < fs / 2 && (st[k].type != T_M17B || fmod (tsim, 20.0) < 6.0);
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
