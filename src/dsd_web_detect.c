/*-------------------------------------------------------------------------------
 * dsd_web_detect.c
 * DSD-FME Web Interface: listen for every protocol's frame sync at once
 *
 * Auto-decode and the band scan used to find what a channel carries by trying
 * one decoder after another, a few seconds each. This listens for the sync
 * words of all of them at once in the decoder's no-sync audio (what
 * web_analog_tap gets, before any filtering), so the one decoder can be
 * switched straight to the protocol on the channel.
 *
 * Every symbol rate (2400, 4800, 6000 and 9600 Bd: 20, 10, 8 and 5 samples per
 * symbol at 48 kHz) slices the sign of a short moving average at every sample
 * into one shift register per timing phase, so all the phases are tried at
 * once instead of recovering the symbol clock. Each register is compared with
 * that rate's sync words, DSD-FME's own ('1' positive, '3' negative, the way
 * getFrameSync slices them), so what is heard here is what the decoder it is
 * handed to syncs on. Trying every phase gives random matches many more
 * chances than the decoder gets, so a 4FSK word must also have its symbols at
 * the levels the protocol sends them (one in eight outer ones may dip): most
 * sync symbols are outer (+3 or -3), while a random 4FSK symbol is inner half
 * the time and noise rarely stays over its own mean. Long words (20 symbols
 * and up) count after two within 1.5 s (D-STAR's a frame-sync period apart).
 * The short ones (M17 8, NXDN 10, dPMR 12) count after four in a row a frame
 * apart, and only while the channel is open, as the decoders only look for
 * them then. When syncs of more than one protocol come, the one with the most
 * of them lately is the one heard. Decoder thread only.
 *-----------------------------------------------------------------------------*/

#include "dsd.h"
#include "dsd_web.h"
#include <math.h>

#define FS        48000
#define LONG_WIN  (FS * 3 / 2) //two long syncs within 1.5 s
#define NOW_WIN   (FS * 3 / 2) //a protocol is heard "now" while its last sync is this recent
#define OUTER     1.0f         //an outer symbol is at least this times the mean level (outer ~1.5, inner ~0.5)
#define RECENT_N  32           //recent syncs kept per protocol, to tell which one is heard most
#define CHAIN_N   16           //syncs kept per protocol and polarity to chain the next one onto
#define MAX_PAT   16

enum { R2400, R4800, R6000, R9600, R_COUNT };
static const int rate_sps[R_COUNT] = { 20, 10, 8, 5 };

enum { F_P25P1, F_P25P2, F_DMR, F_X2, F_YSF, F_DSTAR, F_NXDN48, F_NXDN96, F_DPMR, F_M17, F_PV, F_EDACS, F_COUNT };

typedef struct
{
  const char * name;  //for the log and the page
  const char * key;   //decoder it is handed to ("auto" takes it as it is)
  int rate;
  int period;         //symbols from one frame's sync to the next one's; 0: any two syncs within 1.5 s
  int kmax;           //two syncs of a chain may be up to this many periods apart (a sync missed)
  int need;           //syncs in a row that make a find
  int gate;           //only while the channel is open (the short words, as in getFrameSync)
} fam_def;

static const fam_def fam[F_COUNT] =
{
  [F_P25P1]  = { "P25p1",    "auto",     R4800,    0, 0, 2, 0 },
  [F_P25P2]  = { "P25p2",    "p25p2",    R6000,    0, 0, 2, 0 },
  [F_DMR]    = { "DMR",      "auto",     R4800,    0, 0, 2, 0 },
  [F_X2]     = { "X2-TDMA",  "auto",     R4800,    0, 0, 2, 0 },
  [F_YSF]    = { "YSF",      "auto",     R4800,    0, 0, 2, 0 },
  [F_DSTAR]  = { "D-STAR",   "auto",     R4800, 2016, 2, 2, 0 }, //frame sync every 21st 20 ms voice frame
  [F_NXDN48] = { "NXDN48",   "nxdn48",   R2400,  192, 2, 4, 1 }, //192-symbol frames at either rate: 80 ms
  [F_NXDN96] = { "NXDN96",   "nxdn96",   R4800,  192, 2, 4, 1 }, //and 40 ms
  [F_DPMR]   = { "dPMR",     "dpmr",     R2400,  384, 2, 4, 1 }, //FS2 every other 80 ms frame (the decoder reads 384 symbols on one)
  [F_M17]    = { "M17",      "m17",      R4800,  192, 2, 4, 1 }, //40 ms frames
  [F_PV]     = { "ProVoice", "provoice", R9600,    0, 0, 2, 0 },
  [F_EDACS]  = { "EDACS",    "provoice", R9600,    0, 0, 2, 0 },
};

//the sync words getFrameSync looks for, upright. Inverted, a word is all its bits flipped; for DMR
//and X2-TDMA that is the voice sync of the same kind, so either way it is that protocol (both).
//lv marks the symbols the protocol sends at an outer level ('o'; '.' inner), from the word's dibits
//(01 +3, 00 +1, 10 -1, 11 -3): every one of P25's, DMR's, M17's and dPMR's; NXDN's FSW (0xCDF59)
//has two inner ones, which is why getFrameSync also takes it with those two flipped, and YSF's
//(0xD471C9634D) seven. NULL: no level test (2-level D-STAR, ProVoice and EDACS; X2-TDMA unknown)
#define ALL24 "oooooooooooooooooooooooo"
typedef struct { int f; const char * s; int both; const char * lv; } pat_def;

static const pat_def pat_list[] =
{
  { F_P25P1, P25P1_SYNC, 0, ALL24 },
  { F_P25P2, P25P2_SYNC, 0, ALL24 },
  { F_DMR, DMR_BS_DATA_SYNC, 1, ALL24 }, { F_DMR, DMR_MS_DATA_SYNC, 1, ALL24 },
  { F_DMR, DMR_DIRECT_MODE_TS1_DATA_SYNC, 1, ALL24 }, { F_DMR, DMR_DIRECT_MODE_TS2_DATA_SYNC, 1, ALL24 },
  { F_X2, X2TDMA_BS_DATA_SYNC, 1, NULL }, { F_X2, X2TDMA_MS_DATA_SYNC, 1, NULL },
  { F_YSF, FUSION_SYNC, 0, "ooo.oo.oo..oo..oo.oo" },
  { F_DSTAR, DSTAR_SYNC, 0, NULL }, { F_DSTAR, DSTAR_HD, 0, NULL },
  //the FSW forms getFrameSync takes (the inverted ones are these flipped)
  { F_NXDN48, "3131331131", 0, "o.oooooo.o" }, { F_NXDN48, "3331331131", 0, "o.oooooo.o" }, { F_NXDN48, "3131331111", 0, "o.oooooo.o" },
  { F_NXDN96, "3131331131", 0, "o.oooooo.o" }, { F_NXDN96, "3331331131", 0, "o.oooooo.o" }, { F_NXDN96, "3131331111", 0, "o.oooooo.o" },
#ifdef NXDN_OLD_SYNC
  { F_NXDN48, "3331331111", 0, "o.oooooo.o" }, { F_NXDN48, "3131311131", 0, "o.oooooo.o" },
  { F_NXDN96, "3331331111", 0, "o.oooooo.o" }, { F_NXDN96, "3131311131", 0, "o.oooooo.o" },
#endif
  { F_DPMR, DPMR_FRAME_SYNC_2, 0, ALL24 }, //the only dPMR sync the decoder acts on (0x5FF77D; FS1 0x57FF5F75D577 is all outer too)
  { F_M17, M17_STR, 0, ALL24 },            //likewise (an inverted STR is the LSF word)
  { F_PV, PROVOICE_SYNC, 0, NULL }, { F_PV, PROVOICE_EA_SYNC, 0, NULL },
  { F_EDACS, EDACS_SYNC, 0, NULL },
};
#define PAT_N (int)(sizeof(pat_list) / sizeof(pat_list[0]))

typedef struct
{
  uint64_t bits, mask; //newest symbol in bit 0, 1 = negative ('3')
  uint64_t outer;      //the symbols that must be at an outer level, same order; 0 no level test
  int miss;            //how many of them may dip under it (noise on a real word)
  int len, f, both;
} pat;

typedef struct
{
  int sps, ma;         //samples per symbol, moving average length (the middle of a symbol)
  float inv_ma;
  int16_t ring[16];    //the last ma samples
  int rp;
  int32_t sum;
  int ph;              //timing phase of the next sample
  uint64_t reg[20];    //signs per phase, newest symbol in bit 0
  uint8_t cnt[20];     //symbols in it, up to 64
  float mag[20][32];   //and the last 32 symbols' magnitudes per phase
  uint8_t mp[20];      //where the next one goes
  float amp;           //mean magnitude over all phases (the level an outer symbol is told by)
  pat p[MAX_PAT];
  int np;
} rate_st;

typedef struct
{
  uint64_t t[CHAIN_N]; //syncs heard (the first sample matching), and the chain each one ends
  uint8_t len[CHAIN_N];
  int n, head;
  uint64_t last;       //last sample the word matched: the neighbouring phases match the same sync
} chain;

static struct
{
  int ready;
  rate_st r[R_COUNT];
  float dc;              //slow mean of the input: a carrier off the channel centre shows as DC
  int open;              //the channel is open (this block)
  uint64_t t;            //samples examined since the reset
  uint64_t listen;       //of those, with the channel open
  unsigned long frames;  //decoder frames accepted when the last block came
  int restart;           //registers and chains start over with the next block
  chain ch[F_COUNT][2];  //upright, inverted
  int pass[F_COUNT];     //heard since the reset (its need of syncs in a row)
  int syncs[F_COUNT];    //syncs of it in passing chains
  int pol[F_COUNT];      //polarity of its last one, 1 inverted
  uint64_t last[F_COUNT];
  uint64_t recent[F_COUNT][RECENT_N]; //its last passing syncs
  int rhead[F_COUNT];
} D;

static void det_init (void)
{
  memset (&D, 0, sizeof(D));
  for (int r = 0; r < R_COUNT; r++)
  {
    D.r[r].sps = rate_sps[r];
    D.r[r].ma = (rate_sps[r] + 1) / 2;
    D.r[r].inv_ma = 1.0f / D.r[r].ma;
  }
  for (int i = 0; i < PAT_N; i++)
  {
    const pat_def * d = &pat_list[i];
    rate_st * R = &D.r[fam[d->f].rate];
    int len = (int)strlen (d->s);
    if (R->np >= MAX_PAT || len > 64) continue;
    pat * p = &R->p[R->np++];
    p->len = len;
    p->f = d->f;
    p->both = d->both;
    p->mask = len == 64 ? ~0ULL : (1ULL << len) - 1;
    p->outer = 0;
    for (int k = 0; d->lv && k < len && k < 32; k++)
      if (d->lv[k] == 'o') p->outer |= 1ULL << (len - 1 - k);
    p->miss = __builtin_popcountll (p->outer) / 8;
    p->bits = 0;
    for (int k = 0; k < len; k++)
      if (d->s[k] == '3') p->bits |= 1ULL << (len - 1 - k);
  }
  D.ready = 1;
}

//registers and chains start over (a gap in the audio); what was heard stays
static void det_restart (void)
{
  for (int r = 0; r < R_COUNT; r++)
  {
    rate_st * R = &D.r[r];
    memset (R->ring, 0, sizeof(R->ring));
    memset (R->cnt, 0, sizeof(R->cnt));
    R->rp = 0;
    R->sum = 0;
    R->amp = 0;
  }
  memset (D.ch, 0, sizeof(D.ch));
}

//a sync word matched at the newest sample
static void hit (int f, int inv, int sps, int len)
{
  const fam_def * F = &fam[f];
  if (F->gate && !D.open) return; //the decoders don't look for the short ones then either
  chain * c = &D.ch[f][inv];
  uint64_t t = D.t;
  if (c->last && t - c->last <= (uint64_t)sps) { c->last = t; return; } //the same sync at the next phase
  c->last = t;
  int L = 1;
  for (int i = 0; i < c->n; i++)
  {
    uint64_t dt = t - c->t[i];
    if (F->period)
    {
      //a frame (or two) after one of the chain, within a symbol
      uint64_t P = (uint64_t)F->period * sps, tol = (uint64_t)sps + 2;
      for (int k = 1; k <= F->kmax; k++)
        if (dt + tol >= k * P && dt <= k * P + tol && c->len[i] + 1 > L) L = c->len[i] + 1;
    }
    else if (dt >= (uint64_t)len * sps && dt <= LONG_WIN) L = 2; //another one of its syncs, not this one again
  }
  c->t[c->head] = t;
  c->len[c->head] = (uint8_t)(L < 255 ? L : 255);
  c->head = (c->head + 1) % CHAIN_N;
  if (c->n < CHAIN_N) c->n++;
  if (L < F->need) return;
  D.pass[f] = 1;
  D.syncs[f]++;
  D.pol[f] = inv;
  D.last[f] = t;
  D.recent[f][D.rhead[f]] = t;
  D.rhead[f] = (D.rhead[f] + 1) % RECENT_N;
}

//the symbols of the word that the protocol sends outer are at an outer level at phase ph, but for
//the few that may dip
static int outer_word (const rate_st * R, int ph, const pat * p)
{
  if (!(R->amp > 0)) return 0;
  float min = OUTER * R->amp;
  int dips = 0;
  for (int j = 0; p->outer >> j; j++)
    if ((p->outer >> j & 1) && R->mag[ph][(R->mp[ph] - 1 - j) & 31] < min && ++dips > p->miss) return 0;
  return 1;
}

static inline void det_sample (int16_t x)
{
  D.t++;
  D.dc += ((float)x - D.dc) * (1.0f / 2400.0f); //50 ms
  for (int r = 0; r < R_COUNT; r++)
  {
    rate_st * R = &D.r[r];
    R->sum += x - R->ring[R->rp];
    R->ring[R->rp] = x;
    if (++R->rp == R->ma) R->rp = 0;
    int ph = R->ph;
    if (++R->ph == R->sps) R->ph = 0;
    float v = (float)R->sum * R->inv_ma - D.dc, m = fabsf (v);
    R->amp += (m - R->amp) * (1.0f / 2400.0f); //50 ms
    uint64_t reg = R->reg[ph] << 1 | (uint64_t)(v <= 0);
    R->reg[ph] = reg;
    R->mag[ph][R->mp[ph]] = m;
    R->mp[ph] = (R->mp[ph] + 1) & 31;
    if (R->cnt[ph] < 64) R->cnt[ph]++;
    int have = R->cnt[ph];
    for (int k = 0; k < R->np; k++)
    {
      const pat * p = &R->p[k];
      if (have < p->len) continue;
      uint64_t w = reg & p->mask; //the signs match the word exactly, upright or inverted, as in getFrameSync
      int inv = w == p->bits ? 0 : w == (p->bits ^ p->mask) ? 1 : -1;
      if (inv < 0) continue;
      if (p->outer && !outer_word (R, ph, p)) continue;
      hit (p->f, inv && !p->both, R->sps, p->len);
    }
  }
}

void web_detect_reset (void)
{
  det_init ();
}

void web_detect_audio (dsd_opts * opts, dsd_state * state, const short * in, int n, int settling)
{
  UNUSED(state);
  if (!D.ready) det_init ();
  //the decoder read frames since the last block: what it read then never came here, a gap
  unsigned long fr = web_frames_accepted ();
  if (fr != D.frames) { D.frames = fr; D.restart = 1; }
  if (settling || n <= 0) { D.restart = 1; return; } //still the old channel's audio after a retune
  if (D.restart) { det_restart (); D.restart = 0; }
  //open: over the RTL squelch, or (other inputs, after a receiver's own squelch) not silent
  double e = 0;
  for (int i = 0; i < n; i++) e += (double)in[i] * in[i];
  D.open = opts->audio_in_type == 3 ? opts->rtl_rms > opts->rtl_squelch_level : sqrt (e / n) > 50;
  if (D.open) D.listen += (uint64_t)n;
  for (int i = 0; i < n; i++) det_sample (in[i]);
}

void web_detect_get (web_detect_info * out)
{
  if (!D.ready) det_init ();
  memset (out, 0, sizeof(*out));
  out->fam = -1;
  //the protocol with the most passing syncs in the last 1.5 s (the latest one on a tie)
  int best = 0;
  for (int f = 0; f < F_COUNT; f++)
  {
    if (!D.pass[f] || D.t - D.last[f] > NOW_WIN) continue;
    int c = 0;
    for (int i = 0; i < RECENT_N; i++)
      if (D.recent[f][i] && D.t - D.recent[f][i] <= NOW_WIN) c++;
    if (c > best || (c == best && out->fam >= 0 && D.last[f] > D.last[out->fam])) { best = c; out->fam = f; }
  }
  if (out->fam >= 0)
  {
    int f = out->fam;
    snprintf (out->key, sizeof(out->key), "%s", fam[f].key);
    snprintf (out->name, sizeof(out->name), "%s", fam[f].name);
    out->inverted = D.pol[f];
    out->syncs = D.syncs[f];
  }
  out->listen = (double)D.listen / FS;
}

const char * web_detect_name (int f)
{
  return f >= 0 && f < F_COUNT ? fam[f].name : "";
}

//the decoder as set syncs on that protocol (its polarity aside)
int web_detect_covered (dsd_opts * opts, dsd_state * state, int f)
{
  switch (f)
  {
    case F_P25P1:  return opts->frame_p25p1;
    case F_P25P2:  return opts->frame_p25p2 && state->samplesPerSymbol == 8; //6000 Bd
    case F_DMR:    return opts->frame_dmr;
    case F_X2:     return opts->frame_x2tdma;
    case F_YSF:    return opts->frame_ysf;
    case F_DSTAR:  return opts->frame_dstar && !opts->frame_provoice;
    case F_NXDN48: return opts->frame_nxdn48;
    case F_NXDN96: return opts->frame_nxdn96;
    case F_DPMR:   return opts->frame_dpmr;
    case F_M17:    return opts->frame_m17;
    case F_PV:
    case F_EDACS:  return opts->frame_provoice;
  }
  return 0;
}

//1 when the decoders take the find's polarity as set: NXDN and dPMR look for one polarity, chosen
//by the Inverted setting, and M17 only for upright; the rest take both
int web_detect_polarity_ok (dsd_opts * opts, const web_detect_info * d)
{
  switch (d->fam)
  {
#ifndef NXDN_OLD_SYNC
    case F_NXDN48:
    case F_NXDN96: return d->inverted == (opts->inverted_nxdn != 0);
#endif
    case F_DPMR:   return d->inverted == (opts->inverted_dpmr != 0);
    case F_M17:    return !d->inverted;
  }
  return 1;
}

