/*-------------------------------------------------------------------------------
 * dsd_spectrum.c
 * IQ spectrum (FFT) for the DSD-FME web interface
 *
 * The RTL callback hands raw IQ to spectrum_push_iq, which stores it in a ring
 * buffer with light metadata. A worker thread turns that into averaged power
 * spectra at a fixed frame rate and keeps the newest linear-power frame for the
 * scanner / snap logic, while also broadcasting an 8-bit dB version to clients.
 *-----------------------------------------------------------------------------*/

#include "dsd_web.h"
#include <pthread.h>

#define RING_SAMP   (1u << 20)   // complex samples held in the ring
#define RING_MASK   (RING_SAMP - 1)
#define MAX_FFT     16384
#define MIN_FFT     256

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
  uint32_t center;         // most recent hardware center freq (Hz)
  uint32_t rate;           // most recent span (Hz)
  int rotated;             // IQ rotated by +fs/4 before storage

  int fft_size, fps, avg;  // config (guarded by mtx)

  float * latest;          // newest linear-power frame, low->high freq
  int latest_bins;
  uint32_t latest_center, latest_rate;
  uint64_t latest_seq;
  double latest_time;
} S;

//--- FFT state (worker thread only) -----------------------------------------
static int fn = 0;
static float * w_re, * w_im, * win, * tw_re, * tw_im, * acc;
static int * brev;
static float win_norm = 1.0f;

static void fft_prepare (int n)
{
  if (n == fn) return;
  free (w_re); free (w_im); free (win); free (tw_re); free (tw_im); free (acc); free (brev);
  w_re = malloc (sizeof(float) * n); w_im = malloc (sizeof(float) * n);
  win  = malloc (sizeof(float) * n); acc  = malloc (sizeof(float) * n);
  tw_re = malloc (sizeof(float) * (n / 2)); tw_im = malloc (sizeof(float) * (n / 2));
  brev  = malloc (sizeof(int) * n);
  int bits = 0; while ((1 << bits) < n) bits++;
  for (int i = 0; i < n; i++)
  {
    int r = 0;
    for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
    brev[i] = r;
  }
  for (int k = 0; k < n / 2; k++)
  {
    tw_re[k] = (float)cos (-2.0 * M_PI * k / n);
    tw_im[k] = (float)sin (-2.0 * M_PI * k / n);
  }
  double s = 0;
  for (int i = 0; i < n; i++)
  {
    double x = 2.0 * M_PI * i / n;
    double v = 0.35875 - 0.48829 * cos (x) + 0.14128 * cos (2 * x) - 0.01168 * cos (3 * x);
    win[i] = (float)v; s += v;
  }
  win_norm = (float)(1.0 / ((s * 128.0) * (s * 128.0))); // full-scale tone -> ~0 dBFS
  fn = n;
}

static void fft_run (float * re, float * im, int n)
{
  for (int i = 0; i < n; i++)
  {
    int j = brev[i];
    if (j > i) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (int size = 2; size <= n; size <<= 1)
  {
    int half = size >> 1, step = n / size;
    for (int start = 0; start < n; start += size)
      for (int k = 0; k < half; k++)
      {
        float tr = tw_re[k * step], ti = tw_im[k * step];
        int a = start + k, b = a + half;
        float xr = re[b] * tr - im[b] * ti;
        float xi = re[b] * ti + im[b] * tr;
        re[b] = re[a] - xr; im[b] = im[a] - xi;
        re[a] += xr;        im[a] += xi;
      }
  }
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
  S.center = center;
  S.rate = rate;
  S.rotated = rotated;
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

static void * worker (void * arg)
{
  UNUSED(arg);
  uint64_t done = 0; // samples up to which frames have been published
  while (!S.stop)
  {
    pthread_mutex_lock (&S.mtx);
    int n = S.fft_size, fps = S.fps, avg = S.avg;
    pthread_mutex_unlock (&S.mtx);
    if (n < MIN_FFT) n = MIN_FFT; else if (n > MAX_FFT) n = MAX_FFT;
    if (fps < 1) fps = 1; else if (fps > 60) fps = 60;
    if (avg < 1) avg = 1; else if (avg > 32) avg = 32;
    fft_prepare (n);

    // one consistent snapshot of what the RTL thread wrote last (the 64-bit counter can tear
    // on 32-bit CPUs); the samples themselves are read below without the lock
    pthread_mutex_lock (&S.ring_mtx);
    uint64_t avail = S.wtotal;
    uint32_t center = S.center, rate = S.rate;
    double wtime = S.wtime;
    int rotated = S.rotated;
    pthread_mutex_unlock (&S.ring_mtx);

    // frames step through the stream at the display rate, but never fall more than a quarter
    // second behind: the RTL hands over ~130 ms of samples at a time, and after a retune the
    // frames must show the new frequency, not a backlog from the old one. The lag also stays
    // under a quarter of the ring, so with need <= half of it the frame never reaches into
    // samples the writer is about to overwrite (3.2 MSPS is 800k samples per quarter second)
    uint64_t need = (uint64_t)n * avg;
    uint64_t step = (rate ? rate : 1000000) / (uint64_t)fps;
    if (avail < need || avail < done + step)
    {
      struct timespec ts = { 0, (long)(1000000000L / (fps * 4)) };
      nanosleep (&ts, NULL);
      continue;
    }
    uint64_t end = done + step;
    uint64_t lag = rate / 4;
    if (lag > RING_SAMP / 4) lag = RING_SAMP / 4;
    if (avail - end > lag) end = avail;
    if (end < need) end = need;
    done = end;
    uint64_t rpos = end - need;
    double data_time = rate ? wtime - (double)(avail - end) / rate : wtime;
    for (int i = 0; i < n; i++) acc[i] = 0.0f;

    for (int a = 0; a < avg; a++)
    {
      uint64_t base = rpos + (uint64_t)a * n;
      for (int i = 0; i < n; i++)
      {
        size_t idx = (size_t)((base + i) & RING_MASK);
        w_re[i] = (float)S.ring[2 * idx]     * win[i];
        w_im[i] = (float)S.ring[2 * idx + 1] * win[i];
      }
      fft_run (w_re, w_im, n);
      for (int i = 0; i < n; i++)
        acc[i] += (w_re[i] * w_re[i] + w_im[i] * w_im[i]) * win_norm;
    }
    float inv = 1.0f / avg;

    // reorder so bin 0 = lowest freq (fftshift); undo the +fs/4 rotation if present
    int half = n / 2;
    int shift = rotated ? (n / 4) : 0;
    pthread_mutex_lock (&S.mtx);
    if (!S.latest || S.latest_bins != n)
    {
      free (S.latest);
      S.latest = malloc (sizeof(float) * n);
      S.latest_bins = n;
    }
    for (int i = 0; i < n; i++)
    {
      int src = (i + half + shift) & (n - 1); // negative freqs first
      S.latest[i] = acc[src] * inv;
    }
    S.latest_center = center;
    S.latest_rate = rate;
    S.latest_seq++;
    S.latest_time = data_time; // when the newest sample in the frame arrived
    pthread_mutex_unlock (&S.mtx);

    if (web_server_subs () & WEB_SUB_FFT)
      publish (S.latest, n, center, rate);

    struct timespec ts = { 0, (long)(1000000000L / fps) };
    nanosleep (&ts, NULL);
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
  free (w_re); free (w_im); free (win); free (tw_re); free (tw_im); free (acc); free (brev);
  w_re = w_im = win = tw_re = tw_im = acc = NULL; brev = NULL; fn = 0;
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
    ok = 1;
  }
  pthread_mutex_unlock (&S.mtx);
  return ok;
}
