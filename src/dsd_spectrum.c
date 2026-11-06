/*-------------------------------------------------------------------------------
 * dsd_spectrum.c
 * IQ spectrum (FFT) for the DSD-FME web interface
 *
 * The RTL callback hands raw IQ to spectrum_push_iq, which stores it in a ring
 * buffer with light metadata. A worker thread transforms every block of the
 * stream once (50 % overlap) and averages the blocks of each frame period into
 * a power spectrum, so a burst between two frames still shows. It keeps the
 * newest linear-power frame for the scanner / snap logic and broadcasts an 8-bit
 * dB version to clients. The RTL hands over IQ a USB transfer at a time (130 ms
 * at 1 MSPS, half a second at 0.25 MSPS), so frames are shown a transfer after
 * their samples arrived, which spreads them evenly until the next one lands.
 *-----------------------------------------------------------------------------*/

#include "dsd_web.h"
#include <pthread.h>

#define RING_SAMP   (1u << 20)   // complex samples held in the ring
#define RING_MASK   (RING_SAMP - 1)
#define MAX_FFT     16384
#define MIN_FFT     256
#define MAX_AVG     32           // most FFTs a frame averages when its own span holds fewer

double spectrum_now (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static struct
{
  int running, stop;
  pthread_t thread;
  pthread_mutex_t mtx;
  pthread_mutex_t ring_mtx; // the RTL thread writes the ring until its stream is torn down, after spectrum_stop

  int16_t * ring;          // interleaved I,Q (int8 range, stored as int16)
  volatile uint64_t wtotal; // total complex samples ever written
  volatile double wtime;   // when the newest sample arrived
  uint64_t mark;           // first sample of the current center / rate
  uint32_t push_len;       // samples in the newest push (one USB transfer)
  uint32_t center;         // most recent hardware center freq (Hz)
  uint32_t rate;           // most recent span (Hz)
  int rotated;             // IQ rotated by +fs/4 before storage

  int fft_size, fps, avg;  // config (guarded by mtx)

  float * latest;          // newest linear-power frame, low->high freq
  int latest_bins;
  uint32_t latest_center, latest_rate;
  uint64_t latest_seq;
  double latest_time, latest_start; // when its newest and its oldest sample arrived
  double latest_unit;      // what a bin of it shows for white IQ noise of variance 1
  int latest_avg;          // FFTs averaged into it
} S;

//--- FFT state (worker thread only) -----------------------------------------
static int fn = 0;
static float * w_re, * w_im, * win, * tw_re, * tw_im, * acc, * sum, * hist;
static int * brev;
static int hist_head, hist_count; // ring of the newest MAX_AVG block spectra in hist
static float win_norm = 1.0f;
static double win_unit;   // bin power for white IQ noise of variance 1 (I² + Q², in 8-bit steps)

//four floats at a time: GCC and Clang turn these into SSE or NEON (or plain code elsewhere), which the
//compilers don't do on their own at -O2 for loops of unknown length
typedef float v4sf __attribute__ ((vector_size (16)));
static inline v4sf v4_load (const float * p) { v4sf v; memcpy (&v, p, sizeof(v)); return v; }
static inline void v4_store (float * p, v4sf v) { memcpy (p, &v, sizeof(v)); }

static void fft_free (void)
{
  free (w_re); free (w_im); free (win); free (tw_re); free (tw_im); free (acc); free (sum); free (hist); free (brev);
  w_re = w_im = win = tw_re = tw_im = acc = sum = hist = NULL; brev = NULL; fn = 0;
}

static int fft_prepare (int n)
{
  if (n == fn) return 0;
  fft_free ();
  w_re = malloc (sizeof(float) * n); w_im = malloc (sizeof(float) * n);
  win  = malloc (sizeof(float) * n); acc  = malloc (sizeof(float) * n); sum = malloc (sizeof(float) * n);
  tw_re = malloc (sizeof(float) * n); tw_im = malloc (sizeof(float) * n);
  hist  = malloc (sizeof(float) * n * MAX_AVG);
  brev  = malloc (sizeof(int) * n);
  if (!w_re || !w_im || !win || !acc || !sum || !tw_re || !tw_im || !hist || !brev)
  {
    fft_free ();
    return -1;
  }
  int bits = 0; while ((1 << bits) < n) bits++;
  for (int i = 0; i < n; i++)
  {
    int r = 0;
    for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
    brev[i] = r;
  }
  //twiddles of each stage from size 8 up in a row, the size/2 of them from index size/2
  for (int size = 8; size <= n; size <<= 1)
    for (int k = 0; k < size / 2; k++)
    {
      tw_re[size / 2 + k] = (float)cos (-2.0 * M_PI * k / size);
      tw_im[size / 2 + k] = (float)sin (-2.0 * M_PI * k / size);
    }
  double s = 0, s2 = 0;
  for (int i = 0; i < n; i++)
  {
    double x = 2.0 * M_PI * i / n;
    double v = 0.35875 - 0.48829 * cos (x) + 0.14128 * cos (2 * x) - 0.01168 * cos (3 * x);
    win[i] = (float)v; s += v; s2 += v * v;
  }
  win_norm = (float)(1.0 / ((s * 128.0) * (s * 128.0))); // full-scale tone -> ~0 dBFS
  win_unit = s2 / ((s * 128.0) * (s * 128.0));
  fn = n;
  return 0;
}

//in place from bit-reversed input order (fft_block loads it so), n a power of two from 8 up
static void fft_run (float * re, float * im, int n)
{
  //sizes 2 and 4 in one pass: their twiddles are 1 and -i
  for (int s = 0; s < n; s += 4)
  {
    float a0r = re[s] + re[s+1], a0i = im[s] + im[s+1], a1r = re[s] - re[s+1], a1i = im[s] - im[s+1];
    float b0r = re[s+2] + re[s+3], b0i = im[s+2] + im[s+3], b1r = re[s+2] - re[s+3], b1i = im[s+2] - im[s+3];
    re[s]   = a0r + b0r; im[s]   = a0i + b0i;
    re[s+2] = a0r - b0r; im[s+2] = a0i - b0i;
    re[s+1] = a1r + b1i; im[s+1] = a1i - b1r; //b1 times -i
    re[s+3] = a1r - b1i; im[s+3] = a1i + b1r;
  }
  for (int size = 8; size <= n; size <<= 1)
  {
    int half = size >> 1;
    const float * tr = tw_re + half, * ti = tw_im + half;
    for (int start = 0; start < n; start += size)
    {
      float * ar = re + start, * ai = im + start, * br = ar + half, * bi = ai + half;
      for (int k = 0; k < half; k += 4)
      {
        v4sf bR = v4_load (br + k), bI = v4_load (bi + k), tR = v4_load (tr + k), tI = v4_load (ti + k);
        v4sf aR = v4_load (ar + k), aI = v4_load (ai + k);
        v4sf xr = bR * tR - bI * tI, xi = bR * tI + bI * tR;
        v4_store (br + k, aR - xr); v4_store (bi + k, aI - xi);
        v4_store (ar + k, aR + xr); v4_store (ai + k, aI + xi);
      }
    }
  }
}

//power spectrum of the n samples from stream position pos, added to the frame (acc); kept in hist too
//when later frames may need it
static void fft_block (uint64_t pos, int n, int keep)
{
  for (int i = 0; i < n; i++)
  {
    size_t idx = (size_t)((pos + i) & RING_MASK);
    int j = brev[i];
    w_re[j] = (float)S.ring[2 * idx]     * win[i];
    w_im[j] = (float)S.ring[2 * idx + 1] * win[i];
  }
  fft_run (w_re, w_im, n);
  const v4sf g = { win_norm, win_norm, win_norm, win_norm };
  float * p = keep ? hist + (size_t)hist_head * n : NULL;
  for (int i = 0; i < n; i += 4)
  {
    v4sf r = v4_load (w_re + i), m = v4_load (w_im + i);
    v4sf pw = (r * r + m * m) * g;
    if (p) v4_store (p + i, pw);
    v4_store (acc + i, v4_load (acc + i) + pw);
  }
  if (!p) return;
  hist_head = (hist_head + 1) % MAX_AVG;
  if (hist_count < MAX_AVG) hist_count++;
}

//--- Called from the RTL demod callback -------------------------------------
void spectrum_push_iq (const int16_t * iq, int n, uint32_t center, uint32_t rate, int rotated)
{
  if (!S.running || n <= 0) return;
  pthread_mutex_lock (&S.ring_mtx);
  if (!S.ring) { pthread_mutex_unlock (&S.ring_mtx); return; }
  if ((uint32_t)n > RING_SAMP / 2) { iq += 2 * (n - RING_SAMP / 2); n = RING_SAMP / 2; }
  uint64_t w = S.wtotal;
  size_t pos = (size_t)(w & RING_MASK);
  size_t first = (size_t)n;
  if (first > RING_SAMP - pos) first = RING_SAMP - pos;
  memcpy (S.ring + 2 * pos, iq, first * 2 * sizeof(int16_t));
  if ((size_t)n > first) memcpy (S.ring, iq + 2 * first, ((size_t)n - first) * 2 * sizeof(int16_t));
  if (w == 0 || center != S.center || rate != S.rate || rotated != S.rotated) S.mark = w; // a retune or a new rate
  S.center = center;
  S.rate = rate;
  S.rotated = rotated;
  S.push_len = (uint32_t)n;
  S.wtime = spectrum_now ();
  S.wtotal = w + (uint64_t)n;
  pthread_mutex_unlock (&S.ring_mtx);
}

//--- Worker -----------------------------------------------------------------
static void publish (const float * lin_power, int n, uint32_t center, uint32_t rate)
{
  // broadcast an 8-bit dB frame: [u8 type][u8 version][u16 bins][u32 center][u32 rate][f32 dB of value 0]
  // header is little-endian, payload is uint8 dB offset from -140 in 0.5 dB steps
  uint8_t * msg = malloc (16 + n);
  if (!msg) return;
  msg[0] = WEB_BIN_FFT;
  msg[1] = 1; // version
  msg[2] = (uint8_t)(n & 0xFF);
  msg[3] = (uint8_t)((n >> 8) & 0xFF);
  uint32_t c = center, r = rate;
  memcpy (msg + 4, &c, 4);
  memcpy (msg + 8, &r, 4);
  float dbmin = -140.0f;
  memcpy (msg + 12, &dbmin, 4);
  for (int i = 0; i < n; i++)
  {
    float db = 10.0f * log10f (lin_power[i] + 1e-30f);
    float q = (db - dbmin) * 2.0f;        // 0.5 dB per step
    if (q < 0) q = 0; else if (q > 255) q = 255;
    msg[16 + i] = (uint8_t)(q + 0.5f);
  }
  web_broadcast (WEB_KIND_FFT, WS_OP_BINARY, msg, 16 + (size_t)n, WEB_SUB_FFT);
  free (msg);
}

static void nap_until (double t)
{
  struct timespec ts;
  ts.tv_sec = (time_t)t;
  ts.tv_nsec = (long)((t - (double)ts.tv_sec) * 1e9);
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  clock_nanosleep (CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

static void * worker (void * arg)
{
  UNUSED(arg);
  uint64_t pos = 0;     // stream position of the next block to transform
  uint64_t fend = 0;    // the frame being built takes the blocks that end by this sample
  uint64_t mark = 0;    // S.mark the blocks belong to
  int nblk = 0;         // blocks in the frame being built
  int run_n = 0, run_fps = 0, run_avg = 0, run_full = 0; // settings the frame is built with (none yet)
  uint32_t run_rate = 0;
  while (!S.stop)
  {
    pthread_mutex_lock (&S.mtx);
    int n = S.fft_size, fps = S.fps, avg = S.avg;
    pthread_mutex_unlock (&S.mtx);
    if (n < MIN_FFT) n = MIN_FFT; else if (n > MAX_FFT) n = MAX_FFT;
    if (fps < 1) fps = 1; else if (fps > 60) fps = 60;
    if (avg < 1) avg = 1; else if (avg > MAX_AVG) avg = MAX_AVG;
    double now = spectrum_now ();
    if (fft_prepare (n) < 0) { nap_until (now + 0.1); run_n = 0; continue; }
    // with a page showing the frames, each one covers all the IQ since the last; with nobody watching
    // (the squelch's Auto floor, or a band scan or auto-tune left running) just its newest avg blocks,
    // which is all those need, at a fraction of the CPU
    int full = (web_server_subs () & WEB_SUB_FFT) != 0;

    // one consistent snapshot of what the RTL thread wrote last (the 64-bit counter can tear
    // on 32-bit CPUs); the samples themselves are read below without the lock
    pthread_mutex_lock (&S.ring_mtx);
    uint64_t avail = S.wtotal, smark = S.mark;
    uint32_t center = S.center, rate = S.rate, push = S.push_len;
    double wtime = S.wtime;
    int rotated = S.rotated;
    pthread_mutex_unlock (&S.ring_mtx);
    if (!rate || avail < (uint64_t)n) { nap_until (now + 0.05); continue; }

    uint64_t step = rate / (uint64_t)fps; // samples per frame
    uint64_t hop = (uint64_t)n / 2;       // 50 % overlap, more when a frame is shorter than half a block
    if (hop > step) hop = step;
    uint64_t span = (uint64_t)n + (uint64_t)(avg - 1) * hop; // the first frame of a run holds avg blocks too
    if (span < step) span = step;
    int keep = (uint64_t)avg > step / hop; // frames can hold fewer than avg blocks: keep them for the next
    // the RTL thread can add a transfer while blocks are read, so they stay within half the ring of
    // its newest sample (3.2 MSPS is 160 ms of it)
    uint64_t safe = RING_SAMP / 2 - MAX_FFT;
    // a frame is shown a transfer after its newest sample arrived: by then the transfer holding that
    // sample has landed, and the frames of one transfer come out evenly spaced until the next lands
    double hold = (double)push / rate;
    // while samples are missing: until the next transfer should have landed
    double next = wtime + hold + 0.002;
    if (next < now + 0.002) next = now + 0.002; else if (next > now + 0.05) next = now + 0.05;

    // start over: on a new frequency or rate from its first sample (the backlog is the old one),
    // on new settings from here, and when the frames fell behind (a stall, or a CPU too slow for
    // the settings) from the newest samples
    int late = avail - pos > safe;
    if (!late && run_n && pos + n > fend && avail >= fend)
      late = now > wtime - (double)(avail - fend) / rate + hold + 0.25 + 2.0 / fps;
    if (late || smark != mark || n != run_n || fps != run_fps || avg != run_avg || rate != run_rate || full != run_full)
    {
      uint64_t at = smark != mark ? smark : late ? (avail > span ? avail - span : 0) : pos;
      if (at < smark) at = smark;
      if (avail - at > safe) at = avail - safe;
      pos = at;
      fend = at + span;
      nblk = 0;
      hist_count = 0;
      memset (acc, 0, sizeof(float) * n);
      mark = smark;
      run_n = n; run_fps = fps; run_avg = avg; run_rate = rate; run_full = full;
    }

    // transform the frame's blocks as their samples arrive (only the newest avg with nobody watching)
    while (pos + n <= fend && pos + n <= avail)
    {
      if (full || pos + n + (uint64_t)avg * hop > fend)
      {
        fft_block (pos, n, keep);
        nblk++;
      }
      pos += hop;
    }
    if (pos + n <= fend || avail < fend) { nap_until (next); continue; }
    double data_time = wtime - (double)(avail - fend) / rate; // when the frame's newest sample arrived
    double due = data_time + hold;
    if (now < due) { nap_until (due < now + 0.05 ? due : now + 0.05); continue; }

    // the frame averages its own blocks, or the newest avg ones when it holds fewer (never none:
    // a hop is at most a frame, and the first frame of a run holds avg)
    int k = nblk;
    const float * src = acc;
    if (k < avg)
    {
      k = avg < hist_count ? avg : hist_count;
      memcpy (sum, hist + (size_t)((hist_head - 1 + MAX_AVG) % MAX_AVG) * n, sizeof(float) * n);
      for (int j = 2; j <= k; j++)
      {
        const float * p = hist + (size_t)((hist_head - j + MAX_AVG) % MAX_AVG) * n;
        for (int i = 0; i < n; i += 4) v4_store (sum + i, v4_load (sum + i) + v4_load (p + i));
      }
      src = sum;
    }
    float inv = 1.0f / k;
    // the k blocks averaged are the ones before pos, so its oldest sample is k hops back
    double start_time = wtime - (double)(avail - (pos - (uint64_t)k * hop)) / rate;

    // reorder so bin 0 = lowest freq (fftshift); undo the +fs/4 rotation if present
    int half = n / 2;
    int shift = rotated ? (n / 4) : 0;
    pthread_mutex_lock (&S.mtx);
    if (!S.latest || S.latest_bins != n)
    {
      free (S.latest);
      S.latest = malloc (sizeof(float) * n);
      S.latest_bins = S.latest ? n : 0;
    }
    if (S.latest)
    {
      for (int i = 0; i < n; i++)
      {
        int s = (i + half + shift) & (n - 1); // negative freqs first
        S.latest[i] = src[s] * inv;
      }
      S.latest_center = center;
      S.latest_rate = rate;
      S.latest_seq++;
      S.latest_time = data_time;
      S.latest_start = start_time;
      S.latest_unit = win_unit;
      S.latest_avg = k;
    }
    pthread_mutex_unlock (&S.mtx);

    if (S.latest && full)
      publish (S.latest, n, center, rate);

    fend += step;
    nblk = 0;
    memset (acc, 0, sizeof(float) * n);
  }
  return NULL;
}

//--- Public API -------------------------------------------------------------
void spectrum_start (void)
{
  if (S.running) return;
  memset (&S, 0, sizeof(S));
  pthread_mutex_init (&S.mtx, NULL);
  pthread_mutex_init (&S.ring_mtx, NULL);
  S.ring = calloc (RING_SAMP * 2, sizeof(int16_t));
  S.fft_size = 4096;
  S.fps = 15;
  S.avg = 2;
  if (!S.ring) return;
  S.running = 1;
  if (pthread_create (&S.thread, NULL, worker, NULL) != 0)
  {
    S.running = 0;
    free (S.ring);
    S.ring = NULL;
  }
}

void spectrum_stop (void)
{
  if (!S.running) return;
  S.stop = 1;
  pthread_join (S.thread, NULL);
  S.running = 0;
  pthread_mutex_lock (&S.ring_mtx);
  free (S.ring); S.ring = NULL;
  pthread_mutex_unlock (&S.ring_mtx);
  free (S.latest); S.latest = NULL;
  fft_free ();
}

void spectrum_configure (int fft_size, int fps, int averaging)
{
  pthread_mutex_lock (&S.mtx);
  if (fft_size > 0)
  {
    int n = MIN_FFT;
    while (n < fft_size && n < MAX_FFT) n <<= 1; // snap up to a power of two
    S.fft_size = n;
  }
  if (fps > 0) S.fps = fps;
  if (averaging > 0) S.avg = averaging;
  pthread_mutex_unlock (&S.mtx);
}

void spectrum_get_config (int * fft_size, int * fps, int * averaging)
{
  pthread_mutex_lock (&S.mtx);
  if (fft_size) *fft_size = S.fft_size;
  if (fps) *fps = S.fps;
  if (averaging) *averaging = S.avg;
  pthread_mutex_unlock (&S.mtx);
}

int spectrum_copy_latest (spectrum_frame * out, int max_bins)
{
  int ok = 0;
  pthread_mutex_lock (&S.mtx);
  if (S.latest && S.latest_bins > 0 && out->power && max_bins >= S.latest_bins)
  {
    memcpy (out->power, S.latest, sizeof(float) * S.latest_bins);
    out->bins = S.latest_bins;
    out->center = S.latest_center;
    out->rate = S.latest_rate;
    out->seq = S.latest_seq;
    out->data_time = S.latest_time;
    out->start_time = S.latest_start;
    out->bin_unit = S.latest_unit;
    out->avg = S.latest_avg;
    ok = 1;
  }
  pthread_mutex_unlock (&S.mtx);
  return ok;
}
