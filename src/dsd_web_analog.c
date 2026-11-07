/*-------------------------------------------------------------------------------
 * dsd_web_analog.c
 * DSD-FME Web Interface: what an analog FM channel carries
 *
 * The decoder's no-sync audio (the samples the analog monitor plays, tapped in
 * dsd_symbol.c before any filtering) goes through three detectors: the CTCSS
 * tone, the DCS code and the DTMF ANI some radios send at key-up. A tone or code
 * is a positive sign of analog FM, which the auto-decode and the band scan use;
 * the activity log lists the transmissions with them. All on the decoder thread.
 *
 * 48 kHz in (the RTL demod's own rate repeated up to it), 8 kHz for DTMF, 1 kHz
 * for the sub-audio band.
 *-----------------------------------------------------------------------------*/

#include "dsd.h"
#include "dsd_web.h"
#include <math.h>
#include <time.h>
#include <sys/time.h>

#define DEC      6    //48 kHz -> 8 kHz
#define FIR_N    64
#define SUB_DEC  8    //8 kHz -> 1 kHz
#define CT_N     1000 //CTCSS window: 1 s
#define DCS_N    1500 //DCS window: 1.5 s, about 200 bits
#define SUB_HOP  500  //both looked at every 0.5 s
#define DT_N     205  //DTMF window: 25.6 ms
#define DT_HOP   80   //every 10 ms
#define DCS_BAUD 134.4

static const float ct_hz[50] = {
  67.0f, 69.3f, 71.9f, 74.4f, 77.0f, 79.7f, 82.5f, 85.4f, 88.5f, 91.5f, 94.8f, 97.4f, 100.0f, 103.5f, 107.2f,
  110.9f, 114.8f, 118.8f, 123.0f, 127.3f, 131.8f, 136.5f, 141.3f, 146.2f, 151.4f, 156.7f, 159.8f, 162.2f, 165.5f,
  167.9f, 171.3f, 173.8f, 177.3f, 179.9f, 183.5f, 186.2f, 189.9f, 192.8f, 196.6f, 199.5f, 203.5f, 206.5f, 210.7f,
  218.1f, 225.7f, 229.1f, 233.6f, 241.8f, 250.3f, 254.1f };

//the 104 standard DCS codes (octal)
static const uint16_t dcs_std[] = {
  0023, 0025, 0026, 0031, 0032, 0036, 0043, 0047, 0051, 0053, 0054, 0065, 0071, 0072, 0073, 0074, 0114, 0115,
  0116, 0122, 0125, 0131, 0132, 0134, 0143, 0145, 0152, 0155, 0156, 0162, 0165, 0172, 0174, 0205, 0212, 0223,
  0225, 0226, 0243, 0244, 0245, 0246, 0251, 0252, 0255, 0261, 0263, 0265, 0266, 0271, 0274, 0306, 0311, 0315,
  0325, 0331, 0332, 0343, 0346, 0351, 0356, 0364, 0365, 0371, 0411, 0412, 0413, 0423, 0431, 0432, 0445, 0446,
  0452, 0454, 0455, 0462, 0464, 0465, 0466, 0503, 0506, 0516, 0523, 0526, 0532, 0546, 0565, 0606, 0612, 0624,
  0627, 0631, 0632, 0654, 0662, 0664, 0703, 0712, 0723, 0731, 0732, 0734, 0743, 0754 };
#define DCS_CODES (int)(sizeof(dcs_std) / sizeof(dcs_std[0]))

static const float dt_hz[8] = { 697, 770, 852, 941, 1209, 1336, 1477, 1633 };
static const char dt_keys[4][5] = { "123A", "456B", "789C", "*0#D" };

typedef struct { uint32_t w; uint8_t code, inv; } dcs_ent; //a 23-bit window and the code it belongs to
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } biquad;

static struct
{
  int ready;
  //48 -> 8 kHz
  float fir[FIR_N];
  float hist[2 * FIR_N];
  int hpos, dphase;
  //sub-audio: 4th order Butterworth at 250 Hz, then every 8th sample
  biquad lp[2];
  int sphase;
  float sub[DCS_N];
  int spos, sfill, shop;
  int sub_since;                 //sub-audio samples since the key-up (or since starting over), up to DCS_N
  float ct_coef[50];
  int ct_cand, ct_tone, ct_miss; //tone index seen in the last window, the one confirmed, windows without it
  int dcs_code, dcs_inv, dcs_miss;
  //DTMF: 300 Hz high-pass
  biquad hp;
  float dt[DT_N];
  int dpos, dfill, dhop;
  float dt_coef[8];
  char key;                      //key in the last window, 0 none
  int run, emitted;
  char seq[24];
  int seq_n;
  uint64_t seq_t0, seq_t1;       //first and last digit (8 kHz sample count)
  //transmissions
  uint64_t t8;                   //8 kHz samples seen
  int active, need_keyup;        //need_keyup: the next open starts a transmission (new channel)
  uint64_t keyup_t8, closed_t8;  //last key-up, last time the channel closed
  double v2, v4;                 //300-2800 Hz moments since key-up, while open
  uint64_t vn;
  web_analog_info info;
  double last_tap;               //monotonic seconds
  double settle_until;           //after a retune the old channel's audio is still coming until then
  double dig_t;                  //a digital sync has held since then, 0 none
  uint32_t freq;                 //the channel all this is about
} AN;

static dcs_ent dcs_tab[DCS_CODES * 23 * 2];
static int dcs_n;

static double mono_now (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void bq_lowpass (biquad * q, double fc, double fs, double Q)
{
  double w = 2 * M_PI * fc / fs, c = cos (w), al = sin (w) / (2 * Q), a0 = 1 + al;
  q->b0 = (float)((1 - c) / 2 / a0); q->b1 = (float)((1 - c) / a0); q->b2 = q->b0;
  q->a1 = (float)(-2 * c / a0); q->a2 = (float)((1 - al) / a0);
  q->z1 = q->z2 = 0;
}

static void bq_highpass (biquad * q, double fc, double fs, double Q)
{
  double w = 2 * M_PI * fc / fs, c = cos (w), al = sin (w) / (2 * Q), a0 = 1 + al;
  q->b0 = (float)((1 + c) / 2 / a0); q->b1 = (float)(-(1 + c) / a0); q->b2 = q->b0;
  q->a1 = (float)(-2 * c / a0); q->a2 = (float)((1 - al) / a0);
  q->z1 = q->z2 = 0;
}

static inline float bq_run (biquad * q, float x)
{
  float y = q->b0 * x + q->z1;
  q->z1 = q->b1 * x - q->a1 * y + q->z2;
  q->z2 = q->b2 * x - q->a2 * y;
  return y;
}

//DCS: the Golay (23,12) word of a code, its 12 data bits the 9-bit code and 100; sent from bit 0
static uint32_t dcs_word (int code)
{
  uint32_t d = (uint32_t)code | 0x800, r = d << 11;
  for (int i = 22; i >= 11; i--)
    if (r >> i & 1) r ^= 0xC75u << (i - 11);
  return d | (r & 0x7FF) << 12;
}

static int dcs_cmp (const void * a, const void * b)
{
  uint32_t x = ((const dcs_ent *)a)->w, y = ((const dcs_ent *)b)->w;
  return x < y ? -1 : x > y;
}

static void an_init (void)
{
  memset (&AN, 0, sizeof(AN));
  //windowed sinc, 2.8 kHz: flat up to the DTMF high group, stops before the 4 kHz of the new rate
  double sum = 0;
  for (int i = 0; i < FIR_N; i++)
  {
    double m = i - (FIR_N - 1) / 2.0, x = 2 * 2800.0 / 48000.0;
    double s = m == 0 ? x : sin (M_PI * x * m) / (M_PI * m);
    AN.fir[i] = (float)(s * (0.54 - 0.46 * cos (2 * M_PI * i / (FIR_N - 1))));
    sum += AN.fir[i];
  }
  for (int i = 0; i < FIR_N; i++) AN.fir[i] = (float)(AN.fir[i] / sum);
  bq_lowpass (&AN.lp[0], 250, 8000, 0.5412);
  bq_lowpass (&AN.lp[1], 250, 8000, 1.3066);
  bq_highpass (&AN.hp, 300, 8000, 0.7071);
  for (int k = 0; k < 50; k++) AN.ct_coef[k] = (float)(2 * cos (2 * M_PI * ct_hz[k] / 1000.0));
  for (int k = 0; k < 8; k++) AN.dt_coef[k] = (float)(2 * cos (2 * M_PI * dt_hz[k] / 8000.0));
  AN.ct_cand = AN.ct_tone = AN.dcs_code = -1;
  AN.need_keyup = 1;
  if (!dcs_n)
  {
    //every rotation of every code, and their complements for the inverted codes
    for (int c = 0; c < DCS_CODES; c++)
    {
      uint32_t w = dcs_word (dcs_std[c]);
      for (int r = 0; r < 23; r++)
      {
        uint32_t x = ((w >> r) | (w << (23 - r))) & 0x7FFFFF;
        dcs_tab[dcs_n++] = (dcs_ent){ x, (uint8_t)c, 0 };
        dcs_tab[dcs_n++] = (dcs_ent){ ~x & 0x7FFFFF, (uint8_t)c, 1 };
      }
    }
    qsort (dcs_tab, dcs_n, sizeof(dcs_ent), dcs_cmp);
  }
  AN.ready = 1;
}

//Goertzel power of a tone in x[0..n)
static float goertzel (const float * x, const float * w, int n, float coef)
{
  float s1 = 0, s2 = 0;
  for (int i = 0; i < n; i++)
  {
    float s0 = (w ? x[i] * w[i] : x[i]) + coef * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - coef * s1 * s2;
}

static void set_tone (void)
{
  char * t = AN.info.tone;
  if (AN.dcs_code >= 0) snprintf (t, sizeof(AN.info.tone), "DCS %03o%c", dcs_std[AN.dcs_code], AN.dcs_inv ? 'I' : 'N');
  else if (AN.ct_tone >= 0) snprintf (t, sizeof(AN.info.tone), "CTCSS %.1f", ct_hz[AN.ct_tone]);
  else t[0] = 0;
  if (t[0] && AN.active) snprintf (AN.info.tx_tone, sizeof(AN.info.tx_tone), "%s", t);
}

//the last n sub-audio samples, oldest first
static void sub_last (float * out, int n)
{
  int p = (AN.spos - n + DCS_N) % DCS_N;
  for (int i = 0; i < n; i++) out[i] = AN.sub[(p + i) % DCS_N];
}

//CTCSS: one EIA tone far over the rest of them, in two windows in a row. The DCS line comb
//(every 5.84 Hz) puts power on many tones at once, so the runner-up away from the tone must
//stay well under it too. n: the last n samples (0.5-1 s, only what came since the key-up)
static void ctcss_window (int n)
{
  static float x[CT_N], hw[CT_N];
  float p[50], s[50];
  sub_last (x, n);
  double mean = 0;
  for (int i = 0; i < n; i++) mean += x[i];
  mean /= n;
  for (int i = 0; i < n; i++) { x[i] -= (float)mean; hw[i] = (float)(0.5 - 0.5 * cos (2 * M_PI * i / (n - 1))); }
  int b = 0;
  for (int k = 0; k < 50; k++)
  {
    p[k] = s[k] = goertzel (x, hw, n, AN.ct_coef[k]);
    if (p[k] > p[b]) b = k;
  }
  for (int i = 1; i < 50; i++) //median by insertion sort
    for (int j = i; j > 0 && s[j] < s[j-1]; j--) { float t = s[j]; s[j] = s[j-1]; s[j-1] = t; }
  float med = (s[24] + s[25]) / 2, p2 = 0;
  for (int k = 0; k < 50; k++)
    if (abs (k - b) > 1 && p[k] > p2) p2 = p[k];
  int pass = p[b] > 0 && p[b] >= 100.0f * med && p[b] >= 16.0f * p2 && AN.dcs_code < 0;
  if (pass && b == AN.ct_cand) { AN.ct_tone = b; AN.ct_miss = 0; }
  else if (AN.ct_tone >= 0 && (!pass || b != AN.ct_tone) && ++AN.ct_miss >= 4) AN.ct_tone = -1;
  AN.ct_cand = pass ? b : -1;
}

//DCS: slice the sub-audio at 134.4 bit/s (the clock phase with the widest eye) and look every
//23 bits up among the standard codes' rotations. Normal and inverted forms of two codes are the
//same signal: the normal one is named. A real code matches in most of the windows of 23 bits
//(~200 in 1.5 s), random bits (noise, voice, 4FSK) in at most a few. n: the last n samples
//(0.35-1.5 s, only what came since the key-up)
static void dcs_window (int n)
{
  static float x[DCS_N];
  sub_last (x, n);
  double mean = 0;
  for (int i = 0; i < n; i++) mean += x[i];
  mean /= n;
  double spb = 1000.0 / DCS_BAUD, best = -1, bph = 0;
  for (int k = 0; k < 16; k++)
  {
    double ph = spb * k / 16, sum = 0;
    int m = 0;
    for (double t = ph; t < n - 1; t += spb, m++)
    {
      int i = (int)t;
      double f = t - i, v = x[i] + (x[i+1] - x[i]) * f - mean;
      sum += fabs (v);
    }
    if (m && sum / m > best) { best = sum / m; bph = ph; }
  }
  uint16_t hits[DCS_CODES * 2];
  memset (hits, 0, sizeof(hits));
  uint32_t w = 0;
  int nb = 0;
  for (double t = bph; t < n - 1; t += spb)
  {
    int i = (int)t;
    double f = t - i, v = x[i] + (x[i+1] - x[i]) * f - mean;
    w = (w >> 1) | ((uint32_t)(v > 0) << 22);
    if (++nb < 23) continue;
    dcs_ent key = { w, 0, 0 };
    dcs_ent * e = bsearch (&key, dcs_tab, dcs_n, sizeof(dcs_ent), dcs_cmp);
    if (!e) continue;
    while (e > dcs_tab && e[-1].w == w) e--;
    for (; e < dcs_tab + dcs_n && e->w == w; e++) hits[e->code * 2 + e->inv]++;
  }
  int bi = 0;
  for (int i = 1; i < DCS_CODES * 2; i++)
    if (hits[i] > hits[bi] || (hits[i] == hits[bi] && (bi & 1) && !(i & 1))) bi = i;
  if (hits[bi] >= 8) { AN.dcs_code = bi / 2; AN.dcs_inv = bi & 1; AN.dcs_miss = 0; AN.ct_tone = AN.ct_cand = -1; }
  else if (AN.dcs_code >= 0 && ++AN.dcs_miss >= 3) AN.dcs_code = -1;
}

//an ANI: digits that start within 1 s of key-up, or a longer string anywhere
static void dtmf_seq_end (void)
{
  int early = AN.seq_t0 >= AN.keyup_t8 && AN.seq_t0 - AN.keyup_t8 <= 8000;
  if ((early && AN.seq_n >= 3) || AN.seq_n >= 4)
    snprintf (AN.info.ani, sizeof(AN.info.ani), "%s", AN.seq);
  AN.seq_n = 0;
  AN.seq[0] = 0;
}

//DTMF: one tone of each group well over the others of its group, the pair a good part of the
//band's energy, the two within 8 dB; a digit in two windows in a row (35 ms). Measured on air: the
//first digits after key-up come with only 3-6x over the runner-up of their group and 30-45 % of
//the energy (DCS and the opening squelch share the band then), later ones 10-100x and 60-80 %
static void dtmf_window (void)
{
  float x[DT_N], p[8];
  int p0 = (AN.dpos - DT_N + DT_N) % DT_N;
  double e = 0;
  for (int i = 0; i < DT_N; i++) { x[i] = AN.dt[(p0 + i) % DT_N]; e += (double)x[i] * x[i]; }
  for (int k = 0; k < 8; k++) p[k] = goertzel (x, NULL, DT_N, AN.dt_coef[k]);
  int lo = 0, hi = 4;
  for (int k = 1; k < 4; k++) if (p[k] > p[lo]) lo = k;
  for (int k = 5; k < 8; k++) if (p[k] > p[hi]) hi = k;
  float lo2 = 0, hi2 = 0;
  for (int k = 0; k < 4; k++) if (k != lo && p[k] > lo2) lo2 = p[k];
  for (int k = 4; k < 8; k++) if (k != hi && p[k] > hi2) hi2 = p[k];
  double norm = e * DT_N / 2;
  char key = 0;
  if (e > 1e-3 && p[lo] >= 3 * lo2 && p[hi] >= 3 * hi2 && p[lo] + p[hi] >= 0.3 * norm && p[hi] >= 0.16f * p[lo] && p[hi] <= 6 * p[lo])
    key = dt_keys[lo][hi - 4];
  if (key && key == AN.key)
  {
    if (++AN.run == 2 && !AN.emitted)
    {
      AN.emitted = 1;
      if (AN.seq_n && AN.t8 - AN.seq_t1 > 2400) dtmf_seq_end (); //over 300 ms since the last one
      if (!AN.seq_n) AN.seq_t0 = AN.t8;
      if (AN.seq_n < (int)sizeof(AN.seq) - 1) { AN.seq[AN.seq_n++] = key; AN.seq[AN.seq_n] = 0; }
      AN.seq_t1 = AN.t8;
    }
  }
  else
  {
    AN.key = key;
    AN.run = key ? 1 : 0;
    AN.emitted = 0;
  }
  if (AN.seq_n && AN.t8 - AN.seq_t1 > 2400) dtmf_seq_end ();
}

static void sample8 (float v)
{
  AN.t8++;
  //DTMF
  float h = bq_run (&AN.hp, v);
  if (AN.active)
  {
    //speech is peaky (kurtosis 5-17 measured on air), FM noise Gaussian (3), 4FSK flat (~2)
    double q = (double)h * h;
    AN.v2 += q; AN.v4 += q * q; AN.vn++;
    if (AN.vn >= 8000 && (AN.vn & 1023) == 0) AN.info.voice = AN.v4 * AN.vn >= 4.5 * AN.v2 * AN.v2;
  }
  AN.dt[AN.dpos] = h;
  AN.dpos = (AN.dpos + 1) % DT_N;
  if (AN.dfill < DT_N) AN.dfill++;
  if (++AN.dhop >= DT_HOP && AN.dfill == DT_N) { AN.dhop = 0; dtmf_window (); }
  //sub-audio
  float s = bq_run (&AN.lp[1], bq_run (&AN.lp[0], v));
  if (++AN.sphase < SUB_DEC) return;
  AN.sphase = 0;
  AN.sub[AN.spos] = s;
  AN.spos = (AN.spos + 1) % DCS_N;
  if (AN.sfill < DCS_N) AN.sfill++;
  if (AN.sub_since < DCS_N) AN.sub_since++;
  if (++AN.shop >= SUB_HOP)
  {
    //only what came since the key-up: the ring still holds the transmission before it
    AN.shop = 0;
    int nd = AN.sfill < AN.sub_since ? AN.sfill : AN.sub_since, nc = nd < CT_N ? nd : CT_N;
    if (nd >= 350) dcs_window (nd);
    if (nc >= 500) ctcss_window (nc);
    set_tone ();
  }
}

//start over on the same channel; what the last transmission had stays for the activity log, it is over
static void an_restart (double settle)
{
  web_analog_info keep = AN.info;
  uint32_t f = AN.freq;
  an_init ();
  AN.info = keep;
  AN.info.active = 0;
  AN.info.tone[0] = 0;
  AN.info.voice = 0;
  AN.freq = f;
  AN.settle_until = settle > 0 ? mono_now () + settle : 0;
}

//how long the old channel's audio keeps coming after a retune: the USB transfer in flight and the
//one the decoder is still working through (librtlsdr's 256 KiB, 131072 samples: 0.13 s at the
//default rate, 0.52 s at 0.25 MSPS)
static double settle_time (dsd_opts * opts)
{
#ifdef USE_RTLSDR
  uint32_t r = opts->audio_in_type == 3 ? rtl_dev_rate () : 0;
  if (r > 0) return 0.25 + 2.0 * 131072.0 / r;
#else
  UNUSED(opts);
#endif
  return 0.5;
}

void web_analog_retuned (dsd_opts * opts)
{
  if (!AN.ready) an_init ();
  uint32_t freq = opts->rtlsdr_center_freq;
  if (AN.freq && llabs ((long long)freq - (long long)AN.freq) < 3000) { AN.freq = freq; return; } //auto-tune centering it
  double settle = AN.freq ? settle_time (opts) : 0; //the first channel has no old audio before it
  AN.freq = freq;
  an_restart (settle);
}

void web_analog_tap (dsd_opts * opts, dsd_state * state, const short * in, int n)
{
  if (!AN.ready) an_init ();
  if (opts->rtlsdr_center_freq != AN.freq) web_analog_retuned (opts); //by the scanner or trunking
  double now = mono_now ();
  if (state->carrier)
  {
    //only what the analog monitor would play; a digital transmission ends what was heard before it
    if (AN.dig_t == 0) AN.dig_t = now;
    return;
  }
  if (AN.dig_t > 0)
  {
    if (now - AN.dig_t > 0.5) an_restart (0); //not just a false sync: start over on what follows
    AN.dig_t = 0;
  }
  if (now < AN.settle_until) return; //still the old channel's audio
  AN.last_tap = now;

  //the channel is open: over the RTL squelch, or (other inputs, after a receiver's own squelch) not silent
  double e = 0;
  for (int i = 0; i < n; i++) e += (double)in[i] * in[i];
  int act = opts->audio_in_type == 3 ? opts->rtl_rms > opts->rtl_squelch_level : sqrt (e / n) > 50;
  if (act && !AN.active)
  {
    if (AN.need_keyup || AN.t8 - AN.closed_t8 >= 2400)
    {
      AN.need_keyup = 0;
      //key-up after 300 ms closed: a new transmission
      struct timeval tv;
      gettimeofday (&tv, NULL);
      AN.keyup_t8 = AN.t8;
      AN.info.tx++;
      AN.info.t_keyup = tv.tv_sec + tv.tv_usec / 1e6;
      AN.info.tx_tone[0] = 0;
      AN.info.ani[0] = 0;
      AN.info.voice = 0;
      AN.v2 = AN.v4 = 0;
      AN.vn = 0;
      AN.seq_n = 0;
      //this transmission's tone is found from its own audio (a radio on another tone or code may
      //key up right after one that had it)
      AN.ct_tone = AN.ct_cand = AN.dcs_code = -1;
      AN.ct_miss = AN.dcs_miss = 0;
      AN.sub_since = 0;
      set_tone ();
    }
    AN.active = 1;
  }
  else if (!act && AN.active) { AN.active = 0; AN.closed_t8 = AN.t8; }
  else if (!act && AN.t8 - AN.closed_t8 > 8000 && (AN.ct_tone >= 0 || AN.dcs_code >= 0))
  {
    AN.ct_tone = AN.ct_cand = AN.dcs_code = -1; //closed for a second: the tone went with the carrier
    set_tone ();
  }
  AN.info.active = AN.active;

  for (int i = 0; i < n; i++)
  {
    AN.hist[AN.hpos] = AN.hist[AN.hpos + FIR_N] = in[i];
    AN.hpos = (AN.hpos + 1) % FIR_N;
    if (++AN.dphase < DEC) continue;
    AN.dphase = 0;
    const float * hb = &AN.hist[AN.hpos]; //the last FIR_N samples, oldest first
    float y = 0;
    for (int k = 0; k < FIR_N; k++) y += AN.fir[k] * hb[k];
    sample8 (y);
  }
}

void web_analog_get (web_analog_info * out)
{
  if (!AN.ready) an_init ();
  *out = AN.info;
  //nothing came through lately (digital sync, no input): nothing heard now
  if (mono_now () - AN.last_tap > 0.6) { out->active = 0; out->tone[0] = 0; }
}
