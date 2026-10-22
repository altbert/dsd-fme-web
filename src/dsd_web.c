/*-------------------------------------------------------------------------------
 * dsd_web.c
 * DSD-FME web interface glue (runs on the decoder thread)
 *
 * web_poll is called once per getFrameSync pass. It drains the command queue
 * filled by the server thread, applies changes to opts/state (safe, since only
 * this thread touches the decoder), builds and broadcasts a status snapshot,
 * and runs the scanner, auto-tune and auto-decode state machines.
 *-----------------------------------------------------------------------------*/

#include "dsd_web.h"
#include "git_ver.h"
#include "synctype.h"
#include "bp.h"      //BPK[256] Basic Privacy lookup table
#include <dirent.h>
#include <errno.h>

#ifdef USE_RTLSDR
#include <rtl-sdr.h>
#endif

//candidate decoder modes the auto-decode machine cycles through when a fixed
//frequency produces no sync (the ones DSD-FME cannot passively auto-detect)
enum { AD_AUTO = 0, AD_NXDN48, AD_NXDN96, AD_DPMR, AD_M17, AD_PROVOICE, AD_COUNT };

//decoder keys the web UI knows, "autodecode" (bookmarks) hands the channel to the auto-decode machine
static const char * web_modes[] = { "auto", "tdma", "p25p1", "p25p2", "dmr", "nxdn48", "nxdn96", "dpmr",
  "ysf", "dstar", "m17", "provoice", "edacs", "analog", "autodecode", NULL };

//band scan
#define BS_MAX_RANGES 16
#define BS_MAX_PEAKS  48
#define BS_MAX_SKIP   256
#define BS_MAX_EXCL   64
#define BS_EXCL_TOL   3000   //Hz around an excluded frequency (carrier centers are measured, not exact)
#define BS_SETTLE     0.35   //seconds after a retune before the spectrum shows the new window (USB transfers are ~130 ms)
#define BS_SKIP_SECS  1800.0 //how long a carrier no decoder locked on is left alone
enum { BS_OFF = 0, BS_TUNE, BS_PROBE, BS_FOUND, BS_FIXED };

typedef struct
{
  long long freq; //Hz, power weighted center
  float snr;      //dB over the noise floor
  int width;      //Hz at -6 dB
} bs_peak;

//an unbroken sync being watched: when it began and the frame counters at that point
typedef struct
{
  double since;          //0 = no sync
  unsigned long frames;
  unsigned long good;
} sync_hold;

static struct
{
  dsd_opts * opts;
  dsd_state * state;
  int inited;

  double last_status;
  double status_interval;

  //auto tune (snap the dongle onto the strongest nearby peak on a fixed freq)
  int auto_tune;
  double last_autotune;

  //auto decode (cycle non-detectable modes until a sync holds)
  int auto_decode;
  int ad_idx;
  int ad_tried;          //candidates that had their window on a live signal without a sync
  int ad_analog;         //verdict: analog FM (AUTO keeps watching for P25/DMR/YSF/D-STAR/X2)
  int ad_locked;         //sync has held long enough to trust it
  double ad_last_switch;
  double ad_last_sync;
  double ad_last_active; //last time the squelch was open
  sync_hold ad_hold;     //the current unbroken sync
  unsigned long frames;  //frames the decoder accepted (web_frame_tap)
  unsigned long good;    //frames whose FEC checked out (web_frame_good)

  //analog (non digital) audio passthrough
  int auto_analog;       //analog fallback in AUTO and auto-decode
  int user_monitor;      //Monitor toggle (or -8)
  int gain_manual;       //last manual RTL LNA gain (dB), where the slider sits while hardware AGC runs
  int udpa_tried;        //UDP analog socket is opened on demand, once
  double analog_last;    //last time analog audio went out
  int cosine_default;    //RRC filter setting to restore after M17

  //band scan: sweep the RTL, try every carrier with the digital decoders, bookmark what locks
  int bs_state;
  long long bs_lo[BS_MAX_RANGES], bs_hi[BS_MAX_RANGES];
  int bs_nranges;
  int bs_range, bs_win_k, bs_win_n;   //range being swept, window in it, windows in it
  int bs_win_idx, bs_win_total, bs_pass;
  long long bs_span;                  //usable width of one window
  long long bs_rate;                  //capture rate
  long long bs_win, bs_plo, bs_phi;   //window center and edges
  long long bs_tuned;                 //where the dongle sits for that window (between channels)
  float bs_noise;                     //channel level between carriers, sets the probe squelch
  float bs_thr_db;
  int bs_skip_known, bs_keep_going;
  double bs_t;                        //last retune, or the lock while in BS_FOUND
  uint64_t bs_seq;
  int bs_frames, bs_nbins;
  float bs_acc[16384];                //summed spectra of the window
  bs_peak bs_peaks[BS_MAX_PEAKS];     //carriers queued in the window
  int bs_npeaks, bs_peak_idx;
  int bs_order[AD_COUNT], bs_cand;    //decoders to try on the carrier, current one
  double bs_cand_t, bs_gone_since;
  sync_hold bs_hold;
  int bs_lock_sync;                   //sync type that locked
  char bs_desc[96];                   //name for the bookmark, refined while it decodes
  int bs_found;
  long long bs_last_freq;
  char bs_last_name[96];
  long long bs_skip_f[BS_MAX_SKIP];   //live carriers no decoder locked on
  double bs_skip_t[BS_MAX_SKIP];
  int bs_nskip;
  long long bs_ex_lo[BS_MAX_EXCL];   //user exclusions, tolerance included
  long long bs_ex_hi[BS_MAX_EXCL];
  int bs_nex;
  int bs_saved_fps, bs_saved_avg, bs_saved_sql;
  char bs_saved_mode[16];
  char bs_msg[128];

  //scanner list edited from the web (mirrors trunk_lcn_freq via scan apply)
  long scan_list[1024];
  int scan_count;

  //console log ring (text lines pushed to subscribers)
  spectrum_frame sf;
  float sf_power[16384];

  //auto key-hunt (DMR Basic Privacy): match ciphered frames against the AMBE+2 silence frame
  int auto_key;         //enabled
  int ak_locked;        //a key was found and applied
  int ak_found;         //key confirmed by the decoder tap, applied on the next poll
  int ak_cand;          //key the last silence match pointed at
  int ak_hits;          //silence frames that pointed at it
  unsigned long ak_checked; //ciphered BP voice frames examined
  int ak_pdu;           //the key came from a packet data header (web_offer_bp_key)

  char bind[128];
  int port;
  char password[256];
} W;

//----------------------------------------------------------------------------
// Small helpers
//----------------------------------------------------------------------------

static void web_log (const char * fmt, ...)
{
  char line[1024];
  va_list ap;
  va_start (ap, fmt);
  vsnprintf (line, sizeof(line), fmt, ap);
  va_end (ap);
  web_broadcast (WEB_KIND_CONSOLE, WS_OP_TEXT, line, strlen (line), WEB_SUB_CONSOLE);
}

//the live decoder frequency the dongle / rig is parked on
static long current_freq (dsd_opts * opts)
{
  return (long)opts->rtlsdr_center_freq;
}

static void do_tune (dsd_opts * opts, dsd_state * state, long freq)
{
  if (freq <= 0) return;
  if (opts->use_rigctl == 1)
  {
    if (opts->setmod_bw != 0) SetModulation (opts->rigctl_sockfd, opts->setmod_bw);
    SetFreq (opts->rigctl_sockfd, freq);
  }
#ifdef USE_RTLSDR
  if (opts->audio_in_type == 3) rtl_dev_tune (opts, freq);
#endif
  opts->rtlsdr_center_freq = (uint32_t)freq;
  state->last_cc_sync_time = time (NULL);
}

//----------------------------------------------------------------------------
// Decoder mode selection (mirrors the ncurses menu choices)
//----------------------------------------------------------------------------

static void mode_common (dsd_opts * opts, dsd_state * state)
{
  opts->frame_dstar = 0; opts->frame_x2tdma = 0; opts->frame_p25p1 = 0;
  opts->frame_p25p2 = 0; opts->frame_nxdn48 = 0; opts->frame_nxdn96 = 0;
  opts->frame_dmr = 0; opts->frame_dpmr = 0; opts->frame_provoice = 0;
  opts->frame_ysf = 0; opts->frame_m17 = 0;
  opts->mod_c4fm = 1; opts->mod_qpsk = 0; opts->mod_gfsk = 0;
  state->rf_mod = 0; opts->dmr_stereo = 0; state->dmr_stereo = 0; opts->dmr_mono = 0;
  state->samplesPerSymbol = 10; state->symbolCenter = 4;
  opts->pulse_digi_rate_out = 8000;
  opts->analog_only = 0;                       //a -fA start can switch to digital decoders
  opts->use_cosine_filter = W.cosine_default;  //M17 turns the RRC filter off, the rest want it back
  opts->ssize = 128; opts->msize = 1024;       //startup defaults, only P25p1 uses the short ones (as -f1)
}

static void set_mode (dsd_opts * opts, dsd_state * state, const char * m)
{
  if (!m) return;
  if (!strcmp (m, "auto") || !strcmp (m, "AUTO"))
  {
    mode_common (opts, state);
    opts->frame_dstar = 1; opts->frame_x2tdma = 1; opts->frame_p25p1 = 1;
    opts->frame_p25p2 = 1; opts->frame_dmr = 1; opts->frame_ysf = 1;
    opts->dmr_stereo = 1; opts->pulse_digi_out_channels = 2;
    sprintf (opts->output_name, "AUTO");
  }
  else if (!strcmp (m, "tdma"))
  {
    mode_common (opts, state);
    opts->frame_p25p1 = 1; opts->frame_p25p2 = 1; opts->frame_dmr = 1;
    opts->dmr_stereo = 1; opts->pulse_digi_out_channels = 2;
    sprintf (opts->output_name, "TDMA");
  }
  else if (!strcmp (m, "p25p1"))
  {
    mode_common (opts, state);
    opts->frame_p25p1 = 1; opts->pulse_digi_out_channels = 1;
    opts->ssize = 36; opts->msize = 15;
    sprintf (opts->output_name, "P25p1");
  }
  else if (!strcmp (m, "p25p2"))
  {
    mode_common (opts, state);
    opts->frame_p25p2 = 1; opts->dmr_stereo = 1; opts->pulse_digi_out_channels = 2;
    state->samplesPerSymbol = 8; state->symbolCenter = 3;
    sprintf (opts->output_name, "P25p2");
  }
  else if (!strcmp (m, "dmr"))
  {
    mode_common (opts, state);
    opts->frame_dmr = 1; opts->dmr_stereo = 1; opts->pulse_digi_out_channels = 2;
    sprintf (opts->output_name, "DMR");
  }
  else if (!strcmp (m, "nxdn48"))
  {
    mode_common (opts, state);
    opts->frame_nxdn48 = 1; opts->pulse_digi_out_channels = 1;
    state->samplesPerSymbol = 20; state->symbolCenter = 10;
    sprintf (opts->output_name, "NXDN48");
  }
  else if (!strcmp (m, "nxdn96"))
  {
    mode_common (opts, state);
    opts->frame_nxdn96 = 1; opts->pulse_digi_out_channels = 1;
    sprintf (opts->output_name, "NXDN96");
  }
  else if (!strcmp (m, "dpmr"))
  {
    mode_common (opts, state);
    opts->frame_dpmr = 1; opts->pulse_digi_out_channels = 1;
    state->samplesPerSymbol = 20; state->symbolCenter = 10;
    sprintf (opts->output_name, "dPMR");
  }
  else if (!strcmp (m, "ysf"))
  {
    mode_common (opts, state);
    opts->frame_ysf = 1; opts->pulse_digi_out_channels = 1;
    sprintf (opts->output_name, "YSF");
  }
  else if (!strcmp (m, "dstar"))
  {
    mode_common (opts, state);
    opts->frame_dstar = 1; opts->pulse_digi_out_channels = 1;
    sprintf (opts->output_name, "DSTAR");
  }
  else if (!strcmp (m, "m17"))
  {
    mode_common (opts, state);
    opts->frame_m17 = 1; opts->pulse_digi_out_channels = 1;
    opts->use_cosine_filter = 0;
    sprintf (opts->output_name, "M17");
  }
  else if (!strcmp (m, "provoice"))
  {
    mode_common (opts, state);
    opts->frame_provoice = 1; opts->mod_c4fm = 0; opts->mod_gfsk = 1;
    state->rf_mod = 2; state->samplesPerSymbol = 5; state->symbolCenter = 2;
    opts->pulse_digi_out_channels = 1; //the RTL bandwidth stays as set (24 kHz suits ProVoice)
    state->ea_mode = -1; //back to the startup default after "edacs"
    sprintf (opts->output_name, "EDACS/PV");
  }
  else if (!strcmp (m, "edacs"))
  {
    mode_common (opts, state);
    opts->frame_provoice = 1; opts->mod_c4fm = 0; opts->mod_gfsk = 1;
    state->rf_mod = 2; state->samplesPerSymbol = 5; state->symbolCenter = 2;
    opts->pulse_digi_out_channels = 1;
    state->ea_mode = 1; state->esk_mask = 0;
    sprintf (opts->output_name, "EDACS/PV");
  }
  else if (!strcmp (m, "analog"))
  {
    //like -fA: no digital decoder at all, update_analog turns the passthrough on
    mode_common (opts, state);
    opts->pulse_digi_out_channels = 1;
    sprintf (opts->output_name, "Analog Monitor");
  }
}

int web_mode_valid (const char * key)
{
  if (!key || !key[0]) return 1; //keep current
  for (int i = 0; web_modes[i]; i++)
    if (!strcmp (web_modes[i], key)) return 1;
  return 0;
}

//AUTO frame detection (P25p1/DMR/YSF/D-STAR/X2-TDMA at once)
static int mode_is_auto (dsd_opts * opts)
{
  return opts->frame_dstar && opts->frame_x2tdma && opts->frame_p25p1 && opts->frame_dmr && opts->frame_ysf
      && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_dpmr && !opts->frame_m17 && !opts->frame_provoice;
}

//no digital decoder enabled at all (-fA / "analog")
static int mode_is_analog (dsd_opts * opts)
{
  return !opts->frame_dstar && !opts->frame_x2tdma && !opts->frame_p25p1 && !opts->frame_p25p2 && !opts->frame_dmr
      && !opts->frame_ysf && !opts->frame_nxdn48 && !opts->frame_nxdn96 && !opts->frame_dpmr && !opts->frame_m17
      && !opts->frame_provoice;
}

//the web decoder key matching the current frame settings (however they were set), "" if none fits
static const char * mode_key (dsd_opts * opts, dsd_state * state)
{
  if (opts->frame_provoice) return state->ea_mode == 1 ? "edacs" : "provoice";
  if (opts->frame_m17) return "m17";
  if (opts->frame_nxdn48) return "nxdn48";
  if (opts->frame_nxdn96) return "nxdn96";
  if (opts->frame_dpmr) return "dpmr";
  if (mode_is_auto (opts)) return "auto";
  if (mode_is_analog (opts)) return "analog";
  if (opts->frame_p25p1 && opts->frame_p25p2 && opts->frame_dmr && !opts->frame_dstar) return "tdma";
  int n = opts->frame_p25p1 + opts->frame_p25p2 + opts->frame_dmr + opts->frame_ysf + opts->frame_dstar + opts->frame_x2tdma;
  if (n != 1) return "";
  if (opts->frame_p25p1) return "p25p1";
  if (opts->frame_p25p2) return "p25p2";
  if (opts->frame_dmr) return "dmr";
  if (opts->frame_ysf) return "ysf";
  if (opts->frame_dstar) return "dstar";
  return "";
}

static const char * ad_mode_name (int i)
{
  switch (i)
  {
    case AD_AUTO: return "auto";
    case AD_NXDN48: return "nxdn48";
    case AD_NXDN96: return "nxdn96";
    case AD_DPMR: return "dpmr";
    case AD_M17: return "m17";
    case AD_PROVOICE: return "provoice";
  }
  return "auto";
}

//----------------------------------------------------------------------------
// Audio devices and the analog (non digital) passthrough
//----------------------------------------------------------------------------

//is there a signal on the channel? RF power on RTL input, audio RMS otherwise (only measured while monitoring)
static int channel_active (dsd_opts * opts)
{
  if (opts->audio_in_type != 3 && opts->monitor_input_audio == 0) return 1; //no measurement, assume a signal
  return opts->rtl_rms > opts->rtl_squelch_level;
}

//analog passthrough is wanted for the Monitor toggle, the Analog decoder, and in AUTO / auto-decode
//while the analog fallback is on -- but never while trunking, where it would hold on any carrier
//and stop the control channel hunt
static int analog_wanted (dsd_opts * opts)
{
  if (W.bs_state != BS_OFF) return 0; //band scan hunts digital signals, the carriers it probes would just buzz
  if (W.user_monitor || mode_is_analog (opts)) return 1;
  return W.auto_analog && opts->p25_trunk == 0 && (W.auto_decode || mode_is_auto (opts));
}

//the 48 kHz analog output only opens at startup for -8 / ProVoice, open it on demand
static void ensure_analog_out (dsd_opts * opts, dsd_state * state)
{
  if (opts->audio_out == 1 && opts->audio_out_type == 0 && opts->pulse_raw_dev_out == NULL)
    openPulseOutput (opts); //only opens the streams that are missing
  if (opts->audio_out_type == 8 && opts->udp_sockfdA == 0 && !W.udpa_tried)
  {
    W.udpa_tried = 1;
    if (udp_socket_connectA (opts, state) < 0)
    {
      opts->udp_sockfdA = 0;
      web_log ("Unable to open the UDP analog audio output (port %d).", opts->udp_portno + 2);
    }
  }
}

static void update_analog (dsd_opts * opts, dsd_state * state)
{
  opts->monitor_input_audio = analog_wanted (opts);
  if (opts->monitor_input_audio || opts->frame_provoice) ensure_analog_out (opts, state);
}

//the digital output is opened for a fixed channel count and rate, a decoder switch that changes
//them has to reopen it (as the ncurses menu does), otherwise mono voice plays at double speed
static void reopen_audio (dsd_opts * opts, int old_ch, int old_rate)
{
  if (opts->audio_out == 0) return;
  int changed = old_ch != opts->pulse_digi_out_channels || old_rate != opts->pulse_digi_rate_out;
  if (opts->audio_out_type == 0)
  {
    if (changed && opts->pulse_digi_dev_out != NULL)
    {
      pa_simple_free (opts->pulse_digi_dev_out);
      opts->pulse_digi_dev_out = NULL;
    }
    openPulseOutput (opts); //also opens the digital stream after a -fA start
  }
#ifndef __APPLE__
  else if (changed && (opts->audio_out_type == 2 || opts->audio_out_type == 5))
  {
    close (opts->audio_out_fd);
    openOSSOutput (opts);
  }
#endif
}

//switch decoder, keeping the audio devices and the analog passthrough in step
static void apply_mode (dsd_opts * opts, dsd_state * state, const char * m)
{
  int ch = opts->pulse_digi_out_channels, rate = opts->pulse_digi_rate_out;
  set_mode (opts, state, m);
  noCarrier (opts, state);
  reopen_audio (opts, ch, rate);
  update_analog (opts, state);
}

static void hold_start (sync_hold * h, double now)
{
  h->since = now;
  h->frames = W.frames;
  h->good = W.good;
}

//a sync only counts once it holds. The short sync words (M17, dPMR, NXDN) can chain false
//syncs on noise or analog audio for a second or two, so those also need a real frame rate
//(a genuine signal brings a frame every 40-80 ms, a false chain only a few). M17 matches
//just 8 symbol signs, so its frames also have to pass the LICH Golay check
static int hold_ok (dsd_opts * opts, const sync_hold * h, double now)
{
  if (!(opts->frame_m17 || opts->frame_dpmr || opts->frame_nxdn48 || opts->frame_nxdn96))
    return now - h->since >= 1.5;
  if (now - h->since < 2.0 || W.frames - h->frames < 8) return 0;
  return !opts->frame_m17 || W.good - h->good >= 6;
}

static void set_modulation (dsd_opts * opts, dsd_state * state, const char * m)
{
  if (!strcmp (m, "c4fm")) { opts->mod_c4fm = 1; opts->mod_qpsk = 0; opts->mod_gfsk = 0; state->rf_mod = 0; }
  else if (!strcmp (m, "qpsk")) { opts->mod_c4fm = 0; opts->mod_qpsk = 1; opts->mod_gfsk = 0; state->rf_mod = 1; }
  else if (!strcmp (m, "gfsk")) { opts->mod_c4fm = 0; opts->mod_qpsk = 0; opts->mod_gfsk = 1; state->rf_mod = 2; }
}

//----------------------------------------------------------------------------
// Auto decode control, channel changes and bookmarks
//----------------------------------------------------------------------------

//index of the current decoder among the auto-decode candidates, -1 if it is not one
static int ad_current (dsd_opts * opts, dsd_state * state)
{
  const char * k = mode_key (opts, state);
  for (int i = 0; i < AD_COUNT; i++)
    if (!strcmp (k, ad_mode_name (i))) return i;
  return -1;
}

//probe again from scratch; a new channel always starts at AUTO since it covers the common modes
static void ad_restart (dsd_opts * opts, dsd_state * state, int from_auto)
{
  W.ad_tried = 0;
  W.ad_analog = 0;
  W.ad_locked = 0;
  W.ad_last_switch = spectrum_now ();
  W.ad_last_sync = 0;
  W.ad_last_active = 0;
  W.ad_hold.since = 0;
  int cur = ad_current (opts, state);
  W.ad_idx = (from_auto || cur < 0) ? AD_AUTO : cur;
  if (W.ad_idx != cur) apply_mode (opts, state, ad_mode_name (W.ad_idx));
}

static void ad_enable (dsd_opts * opts, dsd_state * state, int on)
{
  if (on == W.auto_decode) return;
  W.auto_decode = on;
  if (on) ad_restart (opts, state, 0);
  update_analog (opts, state);
  web_log ("Auto-decode %s", on ? "enabled" : "disabled");
}

//decoder chosen from the dropdown or a bookmark; "autodecode" hands the channel to auto-decode
static void select_decoder (dsd_opts * opts, dsd_state * state, const char * key)
{
  if (!key || !key[0] || !web_mode_valid (key)) return;
  if (!strcmp (key, "autodecode")) { ad_enable (opts, state, 1); return; }
  if (W.auto_decode)
  {
    W.auto_decode = 0;
    web_log ("Auto-decode off (decoder chosen manually).");
  }
  apply_mode (opts, state, key);
  web_log ("Decoder: %s", opts->output_name);
}

//move to a new channel (auto-tune's small nudges use do_tune and keep the probe going)
static void retune (dsd_opts * opts, dsd_state * state, long freq)
{
  do_tune (opts, state, freq);
  noCarrier (opts, state);
  web_act_retuned ();
  if (W.auto_decode) ad_restart (opts, state, 1);
  web_log ("Tuned to %.6f MHz", freq / 1e6);
}

static void tune_bookmark (dsd_opts * opts, dsd_state * state, const web_bookmark * bm)
{
  select_decoder (opts, state, bm->mode);
  retune (opts, state, (long)bm->freq);
  if (bm->mod[0]) set_modulation (opts, state, bm->mod);
  web_log ("Bookmark: %s", bm->name);
}

//----------------------------------------------------------------------------
// Band scan: sweep the RTL over frequency ranges one capture window at a time,
// pick the narrowband carriers out of the spectrum, try each with the digital
// decoders, and stop on (and bookmark) the first one that locks. Carriers no
// decoder locks on (analog, paging, unsupported digital) are left alone for a
// while, so later passes only spend time on new activity.
//----------------------------------------------------------------------------

//k-th smallest value (quickselect, reorders a)
static float select_kth (float * a, int n, int k)
{
  int lo = 0, hi = n - 1;
  while (lo < hi)
  {
    float pivot = a[(lo + hi) / 2];
    int i = lo, j = hi;
    while (i <= j)
    {
      while (a[i] < pivot) i++;
      while (a[j] > pivot) j--;
      if (i <= j) { float t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
    }
    if (k <= j) hi = j;
    else if (k >= i) lo = i;
    else break;
  }
  return a[k];
}

//narrowband carriers whose center is in [flo, fhi], strongest first
static int bs_find_peaks (const float * p, int n, long long center, uint32_t rate, long long flo, long long fhi,
                          float thr_db, bs_peak * out, int max)
{
  static float sm[16384], tmp[16384], ped[4096];
  if (n < 64 || n > 16384 || rate == 0) return 0;
  double hz = (double)rate / n;

  //~1.5 kHz smoothing evens out noise and the ripple on top of 4FSK carriers
  int h = (int)(750.0 / hz);
  for (int i = 0; i < n; i++)
  {
    double s = 0;
    int c = 0;
    for (int j = i - h; j <= i + h; j++)
      if (j >= 0 && j < n) { s += p[j]; c++; }
    sm[i] = (float)(s / c);
  }
  memcpy (tmp, sm, sizeof(float) * n);
  float nf = select_kth (tmp, n, n / 2); //noise floor: median bin
  if (!(nf > 0)) return 0;
  float thr = nf * powf (10.0f, thr_db / 10.0f);
  int sep = (int)(6250.0 / hz); //at most one carrier per 6.25 kHz
  if (sep < 1) sep = 1;

  //only -15%..+45% of the span around the tuned frequency: the left quarter is the top of the
  //capture wrapped around, and the capture edges (-25% / +75%) roll off
  int lo = (int)(n / 2 + (flo - center) / hz), hi = (int)(n / 2 + (fhi - center) / hz) + 1;
  if (lo < n / 2 - (int)(0.15 * n)) lo = n / 2 - (int)(0.15 * n);
  if (hi > n / 2 + (int)(0.45 * n)) hi = n / 2 + (int)(0.45 * n);

  int k = 0;
  for (int i = lo; i < hi && k < max; i++)
  {
    if (sm[i] < thr) continue;
    int peak = 1;
    for (int j = i - sep; j <= i + sep && peak; j++)
      if (j >= 0 && j < n && j != i && (sm[j] > sm[i] || (sm[j] == sm[i] && j < i))) peak = 0;
    if (!peak) continue;

    //-6 dB extent, stopping where the slope turns back up into a neighbor
    float edge = sm[i] * 0.25f;
    int a = i, b = i;
    while (a > 0 && sm[a-1] > edge && sm[a-1] <= sm[a] * 1.05f) a--;
    while (b < n - 1 && sm[b+1] > edge && sm[b+1] <= sm[b] * 1.05f) b++;
    int width = (int)((b - a + 1) * hz);
    if (width < 2500 || width > 40000) continue; //spurs and DC are narrower, broadcast and wideband data wider

    //stand clear of the surroundings (median, so an active neighbor channel doesn't count):
    //the lumps on top of a wideband signal like broadcast FM don't
    int w10 = (int)(10000.0 / hz), w50 = (int)(50000.0 / hz), np = 0;
    for (int j = i - w50; j <= i + w50 && np < 4096; j++)
      if (j >= 0 && j < n && abs (j - i) > w10) ped[np++] = sm[j];
    if (np && sm[i] < 4.0f * select_kth (ped, np, np / 2)) continue;

    //an RTL image sits mirrored around the hardware DC (+rate/4 on this display), far under the real carrier
    int m = (3 * n / 2 - i) % n;
    if (sm[m] > sm[i] * 100.0f) continue;

    double s = 0, sf = 0;
    for (int j = a; j <= b; j++) { s += p[j]; sf += (double)p[j] * j; }
    long long f = center + llround ((sf / s - n / 2.0) * hz);
    if (f < flo || f > fhi) continue;
    //DC tones: the IQ conversion leaves one right at the tuned frequency (windows are tuned
    //between channels for that reason), and the dongle has its own at its center, +rate/4
    if (llabs (f - center) < 2000 || llabs (f - (center + (long long)rate / 4)) < 2000) continue;
    out[k].freq = f;
    out[k].snr = 10.0f * log10f (sm[i] / nf);
    out[k].width = width;
    k++;
  }
  for (int i = 1; i < k; i++)
    for (int j = i; j > 0 && out[j].snr > out[j-1].snr; j--)
    {
      bs_peak t = out[j]; out[j] = out[j-1]; out[j-1] = t;
    }
  return k;
}

//level at the tuned carrier in dB over the noise floor from the newest spectrum, -999 until a fresh one
static float bs_center_snr (void)
{
  static float tmp[16384];
  static uint64_t seq;
  static float last = -999;
  W.sf.power = W.sf_power;
  if (!spectrum_copy_latest (&W.sf, 16384) || W.sf.data_time < W.bs_t + BS_SETTLE) return -999;
  if (W.sf.seq == seq) return last;
  seq = W.sf.seq;
  int n = W.sf.bins;
  double hz = (double)W.sf.rate / n;
  int h = (int)(1500.0 / hz);
  double s = 0;
  for (int i = n / 2 - h; i <= n / 2 + h; i++) s += W.sf.power[i];
  memcpy (tmp, W.sf.power, sizeof(float) * n);
  float nf = select_kth (tmp, n, n / 2);
  last = nf > 0 ? 10.0f * log10f ((float)(s / (2 * h + 1)) / nf) : -999;
  return last;
}

static int bs_skipped (long long f, double now)
{
  for (int i = 0; i < W.bs_nskip; i++)
    if (llabs (W.bs_skip_f[i] - f) < 3000 && now - W.bs_skip_t[i] < BS_SKIP_SECS) return 1;
  return 0;
}

//[lo, hi] lies wholly inside one user exclusion
static int bs_excluded (long long lo, long long hi)
{
  for (int i = 0; i < W.bs_nex; i++)
    if (lo >= W.bs_ex_lo[i] && hi <= W.bs_ex_hi[i]) return 1;
  return 0;
}

static void bs_skip_add (long long f, double now)
{
  int slot = -1;
  for (int i = 0; i < W.bs_nskip && slot < 0; i++)
    if (llabs (W.bs_skip_f[i] - f) < 3000) slot = i; //same channel again
  if (slot < 0 && W.bs_nskip < BS_MAX_SKIP) slot = W.bs_nskip++;
  if (slot < 0)                                      //list full: reuse the oldest entry
  {
    slot = 0;
    for (int i = 1; i < W.bs_nskip; i++)
      if (W.bs_skip_t[i] < W.bs_skip_t[slot]) slot = i;
  }
  W.bs_skip_f[slot] = f;
  W.bs_skip_t[slot] = now;
}

//decoder key for what actually locked (AUTO tells the protocols apart by sync type)
static const char * bs_sync_key (dsd_opts * opts, dsd_state * state, int st)
{
  if (st == 0 || st == 1) return "p25p1";
  if (st == 35 || st == 36) return "p25p2";
  if ((st >= 10 && st <= 13) || (st >= 32 && st <= 34)) return "dmr";
  if (st == 30 || st == 31) return "ysf";
  if (st == 6 || st == 7 || st == 18 || st == 19) return "dstar";
  if (st == 8 || st == 9 || st == 16 || st == 17 || st == 86 || st == 87 || st == 98 || st == 99) return "m17";
  if (st >= 20 && st <= 27) return "dpmr";
  return mode_key (opts, state); //NXDN48/96, ProVoice/EDACS, X2-TDMA: the decoder that is running
}

//callsign style field without the padding
static void bs_trim (char * out, size_t n, const char * in)
{
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < n; i++)
    if ((unsigned char)in[i] > ' ' || (o && in[i] == ' ')) out[o++] = in[i];
  while (o && out[o-1] == ' ') o--;
  out[o] = 0;
}

//a new carrier is likely a different system: forget the ids learned on the last one
static void bs_forget_ids (dsd_state * state)
{
  state->dmr_color_code = 16;
  state->nxdn_last_ran = -1;
  state->dpmr_color_code = -1;
  state->edacs_site_id = 0;
  if (!state->p2_hardset) { state->p2_wacn = 0; state->p2_sysid = 0; }
  state->ysf_src[0] = 0;
  state->dstar_src[0] = 0;
  state->m17_src_str[0] = 0;
}

//bookmark name from what the decoder has seen on the locked carrier
static void bs_describe (dsd_opts * opts, dsd_state * state)
{
  char id[64] = "", call[48];
  const char * key = bs_sync_key (opts, state, W.bs_lock_sync);
  const char * sys = opts->output_name;
  if (!strcmp (key, "dmr"))
  {
    sys = "DMR";
    if (state->dmr_color_code < 16) snprintf (id, sizeof(id), "CC%u", state->dmr_color_code);
  }
  else if (!strcmp (key, "p25p1") || !strcmp (key, "p25p2"))
  {
    sys = key[3] == '2' ? "P25p2" : "P25";
    if (state->p2_sysid) snprintf (id, sizeof(id), "WACN %llX SYS %llX", state->p2_wacn, state->p2_sysid);
    else if (state->nac) snprintf (id, sizeof(id), "NAC %03X", state->nac);
  }
  else if (!strcmp (key, "nxdn48") || !strcmp (key, "nxdn96"))
  {
    sys = key[4] == '4' ? "NXDN48" : "NXDN96";
    if (state->nxdn_last_ran < 64) snprintf (id, sizeof(id), "RAN %u", state->nxdn_last_ran);
  }
  else if (!strcmp (key, "dpmr"))
  {
    sys = "dPMR";
    if (state->dpmr_color_code >= 0) snprintf (id, sizeof(id), "CC %d", state->dpmr_color_code);
  }
  else if (!strcmp (key, "ysf")) { sys = "YSF"; bs_trim (id, sizeof(id), state->ysf_src); }
  else if (!strcmp (key, "dstar")) { sys = "D-STAR"; bs_trim (id, sizeof(id), state->dstar_src); }
  else if (!strcmp (key, "m17"))
  {
    sys = "M17";
    bs_trim (call, sizeof(call), state->m17_src_str); //only set once the LSF decodes, CAN comes with it
    if (call[0]) snprintf (id, sizeof(id), "CAN %u %s", state->m17_can, call);
  }
  else if (!strcmp (key, "provoice") || !strcmp (key, "edacs"))
  {
    sys = state->ea_mode == 1 ? "EDACS EA" : "ProVoice/EDACS";
    if (state->edacs_site_id) snprintf (id, sizeof(id), "site %llu", state->edacs_site_id);
  }
  else if (W.bs_lock_sync >= 0 && W.bs_lock_sync <= 40) sys = SyncTypes[W.bs_lock_sync];
  snprintf (W.bs_desc, sizeof(W.bs_desc), "%s%s%s", sys, id[0] ? " " : "", id);
}

static void bs_save (dsd_opts * opts, dsd_state * state)
{
  bs_peak * pk = &W.bs_peaks[W.bs_peak_idx];
  web_bookmark bm;
  memset (&bm, 0, sizeof(bm));
  //land on the 6.25 kHz channel raster when the measurement is that close, otherwise keep what decoded
  long long ch = llround (pk->freq / 6250.0) * 6250;
  bm.freq = llabs (ch - pk->freq) <= 1000 ? ch : llround (pk->freq / 100.0) * 100;
  snprintf (bm.name, sizeof(bm.name), "%s", W.bs_desc);
  snprintf (bm.group, sizeof(bm.group), "Band scan");
  snprintf (bm.mode, sizeof(bm.mode), "%s", bs_sync_key (opts, state, W.bs_lock_sync));
  int existed = bm_near (bm.freq, 2500);
  int id = bm_add (&bm);
  W.bs_found++;
  W.bs_last_freq = bm.freq;
  snprintf (W.bs_last_name, sizeof(W.bs_last_name), "%s", W.bs_desc);
  if (existed) web_log ("Band scan: %s on %.6f MHz (already bookmarked).", W.bs_desc, bm.freq / 1e6);
  else if (id) web_log ("Band scan: found %s on %.6f MHz, bookmarked.", W.bs_desc, bm.freq / 1e6);
  else web_log ("Band scan: found %s on %.6f MHz, but the bookmark list is full.", W.bs_desc, bm.freq / 1e6);
}

static int bs_windows (int r)
{
  long long w = W.bs_hi[r] - W.bs_lo[r];
  int n = (int)((w + W.bs_span - 1) / W.bs_span);
  return n < 1 ? 1 : n;
}

//edges of the current window
static void bs_window (long long * plo, long long * phi)
{
  long long lo = W.bs_lo[W.bs_range], hi = W.bs_hi[W.bs_range];
  double step = (double)(hi - lo) / W.bs_win_n;
  *plo = lo + (long long)(W.bs_win_k * step);
  *phi = lo + (long long)((W.bs_win_k + 1) * step);
}

static int bs_window_excluded (void)
{
  long long plo, phi;
  bs_window (&plo, &phi);
  return bs_excluded (plo, phi);
}

static void bs_tune_window (dsd_opts * opts, dsd_state * state)
{
  bs_window (&W.bs_plo, &W.bs_phi);
  W.bs_win = (W.bs_plo + W.bs_phi) / 2;
  //the RTL captures [tuned - rate/4, tuned + 3 rate/4] and the spectrum shows the top quarter of
  //that wrapped onto its left edge, so the window goes in the clean -15%..+45% stretch. The tuned
  //spot carries a small DC tone from the IQ conversion: keep it halfway between 6.25 kHz channels
  long long t = W.bs_plo + (long long)(0.15 * W.bs_rate) - 4000;
  W.bs_tuned = llround ((t - 3125) / 6250.0) * 6250 + 3125;
  do_tune (opts, state, (long)W.bs_tuned);
  W.bs_t = spectrum_now ();
  W.bs_frames = 0;
  W.bs_npeaks = W.bs_peak_idx = 0;
  W.bs_state = BS_TUNE;
}

//step to the next window, through the ranges in order; wrapping around starts a new pass
static void bs_step_window (void)
{
  W.bs_win_idx++;
  if (++W.bs_win_k >= W.bs_win_n)
  {
    W.bs_win_k = 0;
    if (++W.bs_range >= W.bs_nranges)
    {
      W.bs_range = 0;
      W.bs_win_idx = 0;
      W.bs_pass++;
    }
    W.bs_win_n = bs_windows (W.bs_range);
  }
}

//windows wholly inside an exclusion aren't worth a retune (all of them excluded: tune the next one anyway)
static void bs_next_window (dsd_opts * opts, dsd_state * state)
{
  bs_step_window ();
  for (int i = 0; i < W.bs_win_total && bs_window_excluded (); i++) bs_step_window ();
  bs_tune_window (opts, state);
}

//tune a queued carrier and start on the decoders most likely to fit its occupied width
static void bs_probe (dsd_opts * opts, dsd_state * state)
{
  static const int narrow[AD_COUNT] = { AD_NXDN48, AD_DPMR, AD_AUTO, AD_NXDN96, AD_M17, AD_PROVOICE };
  static const int mid[AD_COUNT]    = { AD_AUTO, AD_NXDN96, AD_NXDN48, AD_DPMR, AD_M17, AD_PROVOICE };
  static const int wide[AD_COUNT]   = { AD_PROVOICE, AD_AUTO, AD_NXDN96, AD_NXDN48, AD_DPMR, AD_M17 };
  bs_peak * pk = &W.bs_peaks[W.bs_peak_idx];
  memcpy (W.bs_order, pk->width < 5000 ? narrow : (pk->width > 14000 ? wide : mid), sizeof(W.bs_order));
  W.bs_cand = 0;
  do_tune (opts, state, (long)pk->freq);
  bs_forget_ids (state);
  //NXDN/dPMR/M17 only look for sync above the RMS squelch: just over the noise lets weak
  //carriers through but keeps noise from feeding their short sync words
  opts->rtl_squelch_level = (int)(W.bs_noise * 1.6f) + 1;
  apply_mode (opts, state, ad_mode_name (W.bs_order[0]));
  W.bs_t = W.bs_cand_t = spectrum_now ();
  W.bs_hold.since = 0;
  W.bs_gone_since = 0;
  W.bs_desc[0] = 0;
  W.bs_state = BS_PROBE;
}

static void bs_next_peak (dsd_opts * opts, dsd_state * state)
{
  if (++W.bs_peak_idx < W.bs_npeaks) bs_probe (opts, state);
  else bs_next_window (opts, state);
}

static void bs_fail (const char * msg)
{
  snprintf (W.bs_msg, sizeof(W.bs_msg), "%s", msg);
  web_log ("Band scan %s", msg);
}

static void bs_stop (dsd_opts * opts, dsd_state * state, const char * why)
{
  if (W.bs_state == BS_OFF) return;
  int parked = W.bs_state == BS_FOUND || W.bs_state == BS_FIXED;
  if (W.bs_state == BS_FOUND) bs_save (opts, state); //it decoded: keep the find
  W.bs_state = BS_OFF;
  spectrum_configure (0, W.bs_saved_fps, W.bs_saved_avg);
  opts->rtl_squelch_level = W.bs_saved_sql;
  //stopped mid sweep: back to the decoder from before the scan (a find keeps the one that decodes it)
  if (!parked && W.bs_saved_mode[0] && strcmp (mode_key (opts, state), W.bs_saved_mode) != 0)
    apply_mode (opts, state, W.bs_saved_mode);
  update_analog (opts, state);
  web_log ("Band scan stopped%s.", why);
}

static double jv_val (jv * e)
{
  if (!e) return 0;
  if (e->t == JV_NUM) return e->num;
  if (e->t == JV_STR && e->str) return strtod (e->str, NULL);
  return 0;
}

//exclusions from the web: [lo, hi] pairs in Hz, a single frequency comes as [f, f]
static void bs_set_excl (jv * arr)
{
  W.bs_nex = 0;
  for (jv * r = jv_first (arr); r && W.bs_nex < BS_MAX_EXCL; r = r->next)
  {
    long long a = llround (jv_val (jv_at (r, 0))), b = llround (jv_val (jv_at (r, 1)));
    if (a <= 0 || b <= 0) continue;
    if (b < a) { long long t = a; a = b; b = t; }
    W.bs_ex_lo[W.bs_nex] = a - BS_EXCL_TOL;
    W.bs_ex_hi[W.bs_nex] = b + BS_EXCL_TOL;
    W.bs_nex++;
  }
}

static void bs_start (dsd_opts * opts, dsd_state * state, jv * cmd)
{
  W.bs_msg[0] = 0;
  if (W.bs_state != BS_OFF) return;
#ifdef USE_RTLSDR
  if (opts->audio_in_type != 3)
  {
    bs_fail ("needs the RTL-SDR input (-i rtl) to sweep and to see the spectrum.");
    return;
  }
  long long lo[BS_MAX_RANGES], hi[BS_MAX_RANGES];
  int n = 0;
  jv * arr = jv_get (cmd, "ranges");
  for (jv * r = jv_first (arr); r && n < BS_MAX_RANGES; r = r->next)
  {
    long long a = llround (jv_val (jv_at (r, 0))), b = llround (jv_val (jv_at (r, 1)));
    if (a < 24000000) a = 24000000;           //RTL-SDR tuning range
    if (b > 1800000000LL) b = 1800000000LL;
    if (b - a < 10000) continue;
    lo[n] = a; hi[n] = b; n++;
  }
  if (!n) { bs_fail ("needs a frequency range between 24 and 1800 MHz."); return; }

  //the same bands as last time pick up where that run stopped
  int same = n == W.bs_nranges && W.bs_pass > 0;
  for (int i = 0; same && i < n; i++) same = lo[i] == W.bs_lo[i] && hi[i] == W.bs_hi[i];

  float thr = (float)jv_num (cmd, "threshold", 10);
  W.bs_thr_db = thr < 4 ? 4 : (thr > 40 ? 40 : thr);
  W.bs_skip_known = jv_bool (cmd, "skip_known", 1);
  W.bs_keep_going = jv_bool (cmd, "keep_going", 0);
  bs_set_excl (jv_get (cmd, "exclude"));

  //the scan owns the tuner and the decoder choice
  if (opts->p25_trunk || opts->scanner_mode || W.auto_decode || W.auto_tune)
    web_log ("Band scan: trunking, scanner, auto-decode and auto-tune switched off.");
  opts->p25_trunk = 0; opts->scanner_mode = 0;
  W.auto_decode = 0; W.auto_tune = 0;

  snprintf (W.bs_saved_mode, sizeof(W.bs_saved_mode), "%s", mode_key (opts, state));
  int fft, fps, avg;
  spectrum_get_config (&fft, &fps, &avg);
  W.bs_saved_fps = fps;
  W.bs_saved_avg = avg;
  spectrum_configure (0, 30, 8);          //faster and steadier spectra, shorter windows
  W.bs_saved_sql = opts->rtl_squelch_level; //probes set their own from the measured noise
  W.bs_noise = 0;

  //a window is the clean 60% of the capture span (see bs_tune_window), less room for the tuning raster
  W.sf.power = W.sf_power;
  uint32_t rate = spectrum_copy_latest (&W.sf, 16384) ? W.sf.rate : 1008000;
  W.bs_rate = rate;
  W.bs_span = (long long)(rate * 0.6) - 8000;

  if (!same)
  {
    memcpy (W.bs_lo, lo, sizeof(long long) * n);
    memcpy (W.bs_hi, hi, sizeof(long long) * n);
    W.bs_nranges = n;
    W.bs_range = W.bs_win_k = W.bs_win_idx = 0;
    W.bs_pass = 1;
    W.bs_nskip = 0;
    W.bs_found = 0;
  }
  W.bs_win_total = 0;
  for (int i = 0; i < W.bs_nranges; i++) W.bs_win_total += bs_windows (i);
  W.bs_win_n = bs_windows (W.bs_range);
  if (W.bs_win_k >= W.bs_win_n) W.bs_win_k = 0;
  if (bs_window_excluded ()) bs_next_window (opts, state);
  else bs_tune_window (opts, state);
  update_analog (opts, state); //no analog passthrough while scanning
  web_log ("Band scan %s: %d range%s, %d windows per pass, threshold %.0f dB, %d exclusion%s.", same ? "resumed" : "started",
           W.bs_nranges, W.bs_nranges == 1 ? "" : "s", W.bs_win_total, W.bs_thr_db, W.bs_nex, W.bs_nex == 1 ? "" : "s");
#else
  UNUSED2 (opts, state);
  UNUSED (cmd);
  bs_fail ("needs DSD-FME built with RTL-SDR support.");
#endif
}

static void run_band_scan (dsd_opts * opts, dsd_state * state)
{
  if (W.bs_state == BS_OFF || W.bs_state == BS_FIXED) return;
  double now = spectrum_now ();

  if (W.bs_state == BS_TUNE)
  {
    //sum a few spectra taken after the retune settled, then queue the carriers in the window
    if (now - W.bs_t < BS_SETTLE) return;
    W.sf.power = W.sf_power;
    if (!spectrum_copy_latest (&W.sf, 16384) || W.sf.seq == W.bs_seq || W.sf.data_time < W.bs_t + BS_SETTLE
        || (long long)W.sf.center != W.bs_tuned)
    {
      if (now - W.bs_t > 5.0) bs_next_window (opts, state); //no spectrum for this window, move on
      return;
    }
    W.bs_seq = W.sf.seq;
    if (W.bs_frames == 0 || W.bs_nbins != W.sf.bins)
    {
      memset (W.bs_acc, 0, sizeof(W.bs_acc));
      W.bs_nbins = W.sf.bins;
      W.bs_frames = 0;
    }
    for (int i = 0; i < W.sf.bins; i++) W.bs_acc[i] += W.sf.power[i];
    if (++W.bs_frames < 3) return;

    //the tuned spot sits between channels, so the channel level there is a noise reference
    //(lowest seen, drifting up slowly in case one window had a carrier next door)
    float rms = (float)opts->rtl_rms;
    if (W.bs_noise <= 0 || rms < W.bs_noise) W.bs_noise = rms;
    else W.bs_noise += 0.1f * (rms - W.bs_noise);

    bs_peak found[BS_MAX_PEAKS];
    int n = bs_find_peaks (W.bs_acc, W.bs_nbins, W.bs_tuned, W.sf.rate, W.bs_plo, W.bs_phi,
                           W.bs_thr_db, found, BS_MAX_PEAKS);
    W.bs_npeaks = 0;
    for (int i = 0; i < n; i++)
    {
      if (W.bs_skip_known && bm_near (found[i].freq, 3000)) continue;
      if (bs_skipped (found[i].freq, now)) continue;
      if (bs_excluded (found[i].freq, found[i].freq)) continue;
      W.bs_peaks[W.bs_npeaks++] = found[i];
    }
    W.bs_peak_idx = 0;
    if (W.bs_npeaks) bs_probe (opts, state);
    else bs_next_window (opts, state);
    return;
  }

  if (W.bs_state == BS_PROBE)
  {
    bs_peak * pk = &W.bs_peaks[W.bs_peak_idx];
    float snr = bs_center_snr ();
    if (snr > -900)
    {
      if (snr < W.bs_thr_db - 4.0f) { if (W.bs_gone_since == 0) W.bs_gone_since = now; }
      else W.bs_gone_since = 0;
    }
    if (state->carrier == 1)
    {
      //short sync words false trigger on noise and analog audio, only a sync that holds counts
      if (W.bs_hold.since == 0) hold_start (&W.bs_hold, now);
      if (!hold_ok (opts, &W.bs_hold, now)) return;
      W.bs_lock_sync = state->lastsynctype;
      bs_describe (opts, state);
      W.bs_t = now;
      W.bs_state = BS_FOUND;
      return;
    }
    W.bs_hold.since = 0;
    if (W.bs_gone_since && now - W.bs_gone_since > 1.0)
    {
      bs_next_peak (opts, state); //the transmission ended, it gets another look on a later pass
      return;
    }
    if (now - W.bs_cand_t < 2.0) return; //this decoder's turn (the first one also covers the retune)
    if (++W.bs_cand >= AD_COUNT)
    {
      bs_skip_add (pk->freq, now); //live carrier no decoder locks on: analog, paging, unsupported digital
      bs_next_peak (opts, state);
      return;
    }
    apply_mode (opts, state, ad_mode_name (W.bs_order[W.bs_cand]));
    W.bs_cand_t = now;
    return;
  }

  if (W.bs_state == BS_FOUND)
  {
    //a few seconds of decoding pick up ids (color code, NAC, RAN, callsign) for the name
    if (state->carrier == 1) bs_describe (opts, state);
    if (now - W.bs_t < 3.0 && state->carrier == 1) return;
    bs_save (opts, state);
    if (W.bs_keep_going) bs_next_peak (opts, state);
    else
    {
      W.bs_state = BS_FIXED;
      web_log ("Band scan: holding on %.6f MHz, Continue keeps looking.", W.bs_last_freq / 1e6);
    }
  }
}

//----------------------------------------------------------------------------
// Key / encryption setup (reuses the decoder's own keystream builders)
//----------------------------------------------------------------------------

//longest ':' separated field of a key string (strtok based parsers can take any of them as the hex part)
static size_t key_field_max (const char * s)
{
  size_t best = 0;
  while (*s)
  {
    size_t n = strcspn (s, ":");
    if (n > best) best = n;
    s += n;
    if (*s) s++;
  }
  return best;
}

static void set_key (dsd_opts * opts, dsd_state * state, jv * cmd)
{
  const char * type = jv_str (cmd, "keytype", "");
  const char * val = jv_str (cmd, "value", "");
  char buf[1024];
  snprintf (buf, sizeof(buf), "%s", val ? val : "");

  //the straight XOR and Vertex 256-bit parsers unpack their hex field into fixed buffers
  //(112 and 32 bytes) without checking its length
  if ((!strcmp (type, "straight") && key_field_max (buf) > 224) || (!strcmp (type, "vertex") && key_field_max (buf) > 64))
  {
    web_log ("Key not set: the %s key value is too long.", type);
    return;
  }

  if (!strcmp (type, "force"))   { state->forced_alg_id = 1; web_log ("Forcing key priority over ENC identifiers."); return; }
  if (!strcmp (type, "unforce")) { state->forced_alg_id = 0; web_log ("Key priority forcing off."); return; }

  if (!strcmp (type, "clear"))
  {
    state->K = state->K1 = state->K2 = state->K3 = state->K4 = 0;
    state->R = state->RR = state->H = 0;
    memset (state->A1, 0, sizeof(state->A1)); memset (state->A2, 0, sizeof(state->A2));
    memset (state->A3, 0, sizeof(state->A3)); memset (state->A4, 0, sizeof(state->A4));
    memset (state->aes_key_loaded, 0, sizeof(state->aes_key_loaded));
    state->tyt_ap = state->tyt_bp = state->tyt_ep = state->retevis_ap = 0;
    state->baofeng_ap = state->csi_ee = state->ken_sc = state->any_bp = 0;
    state->straight_ks = state->straight_mod = state->straight_discard = 0;
    state->forced_alg_id = 0; state->keyloader = 0;
    opts->dmr_mute_encL = 1; opts->dmr_mute_encR = 1;
    web_log ("All keys cleared.");
    return;
  }

  if (!strcmp (type, "bp"))              // Motorola/DMR Basic Privacy (decimal)
  {
    state->K = strtoull (buf, NULL, 10);
    if (state->K > 255) state->K = 255; //index into BPK[256]
    state->keyloader = 0;
  }
  else if (!strcmp (type, "scrambler"))  // NXDN / dPMR scrambler (decimal)
  {
    state->R = strtoull (buf, NULL, 10);
    if (state->R > 0x7FFF) state->R = 0x7FFF;
    state->keyloader = 0;
  }
  else if (!strcmp (type, "rc4") || !strcmp (type, "des")) // hex
  {
    state->R = strtoull (buf, NULL, 16);
    state->RR = state->R;
    state->keyloader = 0;
  }
  else if (!strcmp (type, "hytera"))     // 10/32/64 char hex
  {
    char * p = buf;
    state->K1 = strtoull (p, &p, 16); state->K2 = strtoull (p, &p, 16);
    state->K3 = strtoull (p, &p, 16); state->K4 = strtoull (p, &p, 16);
    state->H = state->K1; state->keyloader = 0;
  }
  else if (!strcmp (type, "aes"))        // AES-128/256 hex
  {
    char * p = buf;
    state->A1[0] = state->A1[1] = strtoull (p, &p, 16);
    state->A2[0] = state->A2[1] = strtoull (p, &p, 16);
    state->A3[0] = state->A3[1] = strtoull (p, &p, 16);
    state->A4[0] = state->A4[1] = strtoull (p, &p, 16);
    state->aes_key_loaded[0] = state->aes_key_loaded[1] =
      (state->A1[0] || state->A2[0] || state->A3[0] || state->A4[0]) ? 1 : 0;
    state->keyloader = 0;
  }
  else if (!strcmp (type, "tyt_bp"))     { state->tyt_bp = 1; state->H = strtoull (buf, NULL, 16) & 0xFFFF; }
  else if (!strcmp (type, "tyt_ap"))     { tyt_ap_pc4_keystream_creation (state, buf); }
  else if (!strcmp (type, "tyt_ep"))     { tyt_ep_aes_keystream_creation (state, buf); }
  else if (!strcmp (type, "retevis"))    { retevis_rc2_keystream_creation (state, buf); }
  else if (!strcmp (type, "baofeng"))    { baofeng_ap_pc5_keystream_creation (state, buf); }
  else if (!strcmp (type, "anytone"))    { anytone_bp_keystream_creation (state, buf); }
  else if (!strcmp (type, "kenwood"))    { ken_dmr_scrambler_keystream_creation (state, buf); }
  else if (!strcmp (type, "straight"))   { straight_mod_xor_keystream_creation (state, buf); }
  else if (!strcmp (type, "auctus"))     { auctus_keystream_creation (state, buf); }
  else if (!strcmp (type, "vertex"))     { vertex_keystream_setup (state, buf); state->keyloader = 0; }
  else if (!strcmp (type, "csi"))        { parse_raw_user_string (buf, state->static_ks_bits[0]); state->csi_ee = 1; }
  else if (!strcmp (type, "ras"))        { ras_effective_key_creation (state, buf); }
  else { web_log ("Unknown key type '%s'", type); return; }

  //unmute only once a key was actually loaded (an unknown type leaves the mute alone)
  opts->dmr_mute_encL = 0;
  opts->dmr_mute_encR = 0;
  if (state->K == 0 && state->K1 == 0 && state->K2 == 0 && state->K3 == 0 && state->K4 == 0
      && state->R == 0 && state->A1[0] == 0 && !state->tyt_ap && !state->tyt_ep && !state->tyt_bp
      && !state->retevis_ap && !state->baofeng_ap && !state->any_bp && !state->ken_sc
      && !state->straight_ks && !state->csi_ee)
  {
    opts->dmr_mute_encL = 1; opts->dmr_mute_encR = 1;
  }
  web_log ("Key set: %s", type);
}

//----------------------------------------------------------------------------
// CSV import from uploaded text (written to a temp file, reusing importers)
//----------------------------------------------------------------------------

static int write_temp (const char * dir, const char * name, const char * text, char * out, size_t outlen)
{
  struct stat st;
  if (!dir || !dir[0]) dir = ".";
  if (stat (dir, &st) == -1) mkdir (dir, 0700); //the WAV directory only exists once recording has started
  snprintf (out, outlen, "%s/%s", dir, name);
  FILE * f = fopen (out, "w");
  if (!f)
  {
    web_log ("Import failed: unable to write %s (%s).", out, strerror (errno));
    return 0;
  }
  if (text) fputs (text, f);
  fclose (f);
  return 1;
}

static void load_csv (dsd_opts * opts, dsd_state * state, jv * cmd)
{
  const char * kind = jv_str (cmd, "kind", "");
  const char * text = jv_str (cmd, "text", "");
  char path[1200];
  const char * dir = W.opts->wav_out_dir;

  if (!strcmp (kind, "channel"))
  {
    if (write_temp (dir, "web_channel_map.csv", text, path, sizeof(path)))
    {
      snprintf (opts->chan_in_file, sizeof(opts->chan_in_file), "%s", path);
      memset (state->trunk_chan_map, 0, sizeof(state->trunk_chan_map));
      memset (state->trunk_lcn_freq, 0, sizeof(state->trunk_lcn_freq));
      state->lcn_freq_count = 0;
      csvChanImport (opts, state);
      int n = 0; //channels in the map (the LCN list next to it stops at 26)
      for (size_t i = 0; i < sizeof(state->trunk_chan_map) / sizeof(state->trunk_chan_map[0]); i++)
        if (state->trunk_chan_map[i]) n++;
      web_log ("Imported channel map (%d channels).", n);
    }
  }
  else if (!strcmp (kind, "group"))
  {
    if (write_temp (dir, "web_group.csv", text, path, sizeof(path)))
    {
      snprintf (opts->group_in_file, sizeof(opts->group_in_file), "%s", path);
      state->group_tally = 0;
      csvGroupImport (opts, state);
      web_log ("Imported group list (%u groups).", state->group_tally);
    }
  }
  else if (!strcmp (kind, "keys_dec"))
  {
    if (write_temp (dir, "web_keys_dec.csv", text, path, sizeof(path)))
    {
      snprintf (opts->key_in_file, sizeof(opts->key_in_file), "%s", path);
      csvKeyImportDec (opts, state);
      state->keyloader = 1;
      opts->dmr_mute_encL = 0; opts->dmr_mute_encR = 0;
      web_log ("Imported decimal key list.");
    }
  }
  else if (!strcmp (kind, "keys_hex"))
  {
    if (write_temp (dir, "web_keys_hex.csv", text, path, sizeof(path)))
    {
      snprintf (opts->key_in_file, sizeof(opts->key_in_file), "%s", path);
      csvKeyImportHex (opts, state);
      state->keyloader = 1;
      opts->dmr_mute_encL = 0; opts->dmr_mute_encR = 0;
      web_log ("Imported hex key list.");
    }
  }
}

static void send_hello (dsd_opts * opts, int client)
{
  jb b; jb_init (&b);
  jb_obj (&b, NULL);
  jb_str (&b, "type", "hello");
  jb_str (&b, "version", GIT_TAG);
  jb_bool (&b, "password", W.password[0] != 0);
  if (web_client_authed (client)) jb_str (&b, "token", web_session_token ()); //unlocks downloads, not before the password
  jb_bool (&b, "rtl", opts->audio_in_type == 3);
  jb_str (&b, "wav_dir", opts->wav_out_dir);
  jb_obj_end (&b);
  web_send (client, WEB_KIND_CTRL, WS_OP_TEXT, b.buf, b.len);
  jb_free (&b);
}

static void send_file_list (dsd_opts * opts, int client, const char * area)
{
  //only the per-call WAV directory is offered (the one download area web_init registers)
  const char * dir = !strcmp (area, "wav") ? opts->wav_out_dir : NULL;
  jb b; jb_init (&b);
  jb_obj (&b, NULL);
  jb_str (&b, "type", "files");
  jb_str (&b, "area", area);
  jb_arr (&b, "files");
  DIR * d = dir ? opendir (dir) : NULL;
  if (d)
  {
    struct dirent * de;
    int n = 0;
    while ((de = readdir (d)) != NULL && n < 500)
    {
      if (de->d_name[0] == '.') continue;
      if (!web_path_component_ok (de->d_name)) continue;
      char full[2048];
      struct stat st;
      snprintf (full, sizeof(full), "%s/%s", dir, de->d_name);
      if (stat (full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
      jb_obj (&b, NULL);
      jb_str (&b, "name", de->d_name);
      jb_int (&b, "size", (long long)st.st_size);
      jb_int (&b, "mtime", (long long)st.st_mtime);
      jb_obj_end (&b);
      n++;
    }
    closedir (d);
  }
  jb_arr_end (&b);
  jb_obj_end (&b);
  web_send (client, WEB_KIND_CTRL, WS_OP_TEXT, b.buf, b.len);
  jb_free (&b);
}

//----------------------------------------------------------------------------
// Apply one command (decoder thread)
//----------------------------------------------------------------------------

static void apply_command (dsd_opts * opts, dsd_state * state, int client, jv * cmd)
{
  const char * c = jv_str (cmd, "cmd", "");

  if (!strcmp (c, "_connect"))
  {
    send_hello (opts, client);
    web_log ("Client connected.");
    return;
  }
  if (!strcmp (c, "_disconnect")) return;

  if (!strcmp (c, "auth"))
  {
    const char * pw = jv_str (cmd, "password", "");
    int ok = (W.password[0] == 0) || (strcmp (pw, W.password) == 0);
    web_set_authed (client, ok);
    char msg[128];
    int n = snprintf (msg, sizeof(msg), "{\"type\":\"auth\",\"ok\":%s}", ok ? "true" : "false");
    web_send (client, WEB_KIND_CTRL, WS_OP_TEXT, msg, (size_t)n);
    if (ok) send_hello (opts, client);
    return;
  }

  //everything past here requires auth
  if (!web_client_authed (client)) return;

  if (!strcmp (c, "subscribe"))
  {
    uint32_t s = 0;
    if (jv_bool (cmd, "fft", 0)) s |= WEB_SUB_FFT;
    if (jv_bool (cmd, "audio", 0)) s |= WEB_SUB_AUDIO;
    if (jv_bool (cmd, "console", 0)) s |= WEB_SUB_CONSOLE;
    if (jv_bool (cmd, "events", 0)) s |= WEB_SUB_EVENTS;
    web_set_subs (client, s);
  }
  else if (!strcmp (c, "tune") || !strcmp (c, "snap"))
  {
    long f = (long)jv_int (cmd, "freq", 0);
    if (f > 0)
    {
      bs_stop (opts, state, " (tuned by hand)");
      retune (opts, state, f);
    }
  }
  else if (!strcmp (c, "mode"))
  {
    bs_stop (opts, state, " (decoder chosen by hand)");
    select_decoder (opts, state, jv_str (cmd, "mode", "auto"));
  }
  else if (!strcmp (c, "modulation"))
  {
    set_modulation (opts, state, jv_str (cmd, "mod", "c4fm"));
  }
  else if (!strcmp (c, "inversion"))
  {
    int inv = jv_bool (cmd, "inverted", 0);
    opts->inverted_dmr = inv; opts->inverted_dpmr = inv;
    opts->inverted_x2tdma = inv; opts->inverted_ysf = inv; opts->inverted_nxdn = inv;
  }
  else if (!strcmp (c, "gain"))
  {
    if (jv_has (cmd, "digital"))
    {
      float g = (float)jv_num (cmd, "digital", 0);
      opts->audio_gain = g; opts->audio_gainR = g;
      if (g > 0) { state->aout_gain = g; state->aout_gainR = g; }
      else { state->aout_gain = 25; state->aout_gainR = 25; }
    }
    if (jv_has (cmd, "analog"))
    {
      float g = (float)jv_num (cmd, "analog", 0);
      if (g < 0) g = 0;
      if (g > 100) g = 100;
      opts->audio_gainA = g;
    }
  }
  else if (!strcmp (c, "rtl"))
  {
    if (jv_has (cmd, "gain"))
    {
      //0 = hardware AGC (the tuner runs its own LNA / mixer gain), otherwise a manual LNA gain in dB;
      //get_rtlsdr_sample applies it, the tuner snaps a manual gain to the nearest step it supports
      int g = (int)jv_int (cmd, "gain", 0);
      if (g < 0) g = 0;
      if (g > 60) g = 60;
      if (g) W.gain_manual = g;
      if (g != opts->rtl_gain_value)
      {
        if (g) web_log ("RTL LNA gain %d dB (manual).", g);
        else web_log ("RTL hardware AGC on.");
      }
      opts->rtl_gain_value = g;
      opts->rtl_gain_actual = 0;
    }
    if (jv_has (cmd, "ppm")) opts->rtlsdr_ppm_error = (int)jv_int (cmd, "ppm", 0);
    if (jv_has (cmd, "vol")) { int v = (int)jv_int (cmd, "vol", 2); if (v < 1) v = 1; if (v > 3) v = 3; opts->rtl_volume_multiplier = v; }
    if (jv_has (cmd, "squelch")) opts->rtl_squelch_level = (int)jv_int (cmd, "squelch", 0);
    if (jv_has (cmd, "bw"))
    {
      //the bandwidths -i rtl takes; the stream reads it once when it opens, so apply it live
      int b = (int)jv_int (cmd, "bw", 12);
      if (b == 4 || b == 6 || b == 8 || b == 12 || b == 16 || b == 24)
      {
#ifdef USE_RTLSDR
        if (opts->audio_in_type == 3 && opts->rtl_started == 1 && b != opts->rtl_bandwidth)
          rtl_dev_set_bandwidth (opts, b);
#endif
        opts->rtl_bandwidth = b;
      }
    }
#ifdef USE_RTLSDR
    if (jv_has (cmd, "rate") && opts->audio_in_type == 3)
    {
      long long r = (long long)jv_int (cmd, "rate", 0);
      if (r < 0 || r > 3200000) r = 0;
      bs_stop (opts, state, " (sample rate changed)"); //its windows are sized for the old span
      uint32_t got = rtl_dev_set_rate (opts, (uint32_t)r);
      web_log ("RTL sample rate %.3f MSPS%s", got / 1e6, r ? "" : " (default)");
    }
    if (jv_has (cmd, "dagc"))
    {
      //RTL2832 digital AGC, independent of the tuner AGC; it raises noise to the same level as
      //signals, so the RMS squelch can't tell them apart while it is on
      int on = jv_bool (cmd, "dagc", 0);
      if (on != opts->rtl_digital_agc)
      {
        if (opts->audio_in_type == 3 && opts->rtl_started == 1) rtl_dev_set_agc (opts, on);
        else opts->rtl_digital_agc = on; //applied when the stream opens
        web_log ("RTL digital AGC %s.", on ? "on" : "off");
      }
    }
#endif
  }
  else if (!strcmp (c, "trunk"))
  {
    opts->p25_trunk = jv_bool (cmd, "enable", 0);
    if (opts->p25_trunk) bs_stop (opts, state, " (trunking enabled)");
    if (opts->p25_trunk) opts->scanner_mode = 0;
    update_analog (opts, state); //no analog fallback while trunking
    web_log ("Trunking %s", opts->p25_trunk ? "enabled" : "disabled");
  }
  else if (!strcmp (c, "scanner"))
  {
    opts->scanner_mode = jv_bool (cmd, "enable", 0);
    if (opts->scanner_mode) bs_stop (opts, state, " (scanner enabled)");
    if (opts->scanner_mode) opts->p25_trunk = 0;
    web_log ("Scanner %s", opts->scanner_mode ? "enabled" : "disabled");
  }
  else if (!strcmp (c, "trunk_opts"))
  {
    if (jv_has (cmd, "group")) opts->trunk_tune_group_calls = jv_bool (cmd, "group", 1);
    if (jv_has (cmd, "private")) opts->trunk_tune_private_calls = jv_bool (cmd, "private", 1);
    if (jv_has (cmd, "data")) opts->trunk_tune_data_calls = jv_bool (cmd, "data", 0);
    if (jv_has (cmd, "enc")) opts->trunk_tune_enc_calls = jv_bool (cmd, "enc", 1);
    if (jv_has (cmd, "allow_list")) opts->trunk_use_allow_list = jv_bool (cmd, "allow_list", 0);
  }
  else if (!strcmp (c, "hangtime"))
  {
    float t = (float)jv_num (cmd, "seconds", 1.0);
    if (t < 0) t = 0;
    if (t > 30) t = 30;
    opts->trunk_hangtime = t;
  }
  else if (!strcmp (c, "tg_hold"))
  {
    state->tg_hold = (uint32_t)jv_int (cmd, "tg", 0);
  }
  else if (!strcmp (c, "slots"))
  {
    if (jv_has (cmd, "slot1")) opts->slot1_on = jv_bool (cmd, "slot1", 1);
    if (jv_has (cmd, "slot2")) opts->slot2_on = jv_bool (cmd, "slot2", 1);
  }
  else if (!strcmp (c, "filters"))
  {
    if (jv_has (cmd, "lpf")) opts->use_lpf = jv_bool (cmd, "lpf", 0);
    if (jv_has (cmd, "hpf")) opts->use_hpf = jv_bool (cmd, "hpf", 1);
    if (jv_has (cmd, "pbf")) opts->use_pbf = jv_bool (cmd, "pbf", 1);
    if (jv_has (cmd, "hpfd")) opts->use_hpf_d = jv_bool (cmd, "hpfd", 1);
  }
  else if (!strcmp (c, "payload"))
  {
    opts->payload = jv_bool (cmd, "enable", 0);
  }
  else if (!strcmp (c, "monitor"))
  {
    W.user_monitor = jv_bool (cmd, "enable", 0);
    update_analog (opts, state);
  }
  else if (!strcmp (c, "setmod_bw"))
  {
    int b = (int)jv_int (cmd, "hz", 0);
    if (b > 25000) b = 25000;
    if (b < 0) b = 0;
    opts->setmod_bw = b;
  }
  else if (!strcmp (c, "p2_params"))
  {
    state->p2_wacn = (unsigned long long)jv_int (cmd, "wacn", 0) & 0xFFFFF;
    state->p2_sysid = (unsigned long long)jv_int (cmd, "sysid", 0) & 0xFFF;
    state->p2_cc = (unsigned long long)jv_int (cmd, "cc", 0) & 0xFFF;
    state->p2_hardset = (state->p2_wacn && state->p2_sysid && state->p2_cc) ? 1 : 0;
  }
  else if (!strcmp (c, "key"))
  {
    set_key (opts, state, cmd);
  }
  else if (!strcmp (c, "csv"))
  {
    load_csv (opts, state, cmd);
  }
  else if (!strcmp (c, "reset_events"))
  {
    for (uint8_t i = 0; i < 2; i++) init_event_history (&state->event_history_s[i], 0, 255);
  }
  else if (!strcmp (c, "activity"))
  {
    web_act_send (client);
  }
  else if (!strcmp (c, "activity_clear"))
  {
    web_act_clear ();
  }
  else if (!strcmp (c, "return_cc"))
  {
    if (opts->p25_trunk && state->p25_cc_freq) { return_to_cc (opts, state); web_log ("Return to control channel."); }
  }
  else if (!strcmp (c, "cycle"))
  {
    bs_stop (opts, state, " (tuned by hand)");
    //manual forward channel cycle (scanner / trunk list)
    if (state->lcn_freq_roll >= state->lcn_freq_count) state->lcn_freq_roll = 0;
    if (state->trunk_lcn_freq[state->lcn_freq_roll] != 0)
      do_tune (opts, state, state->trunk_lcn_freq[state->lcn_freq_roll]);
    state->lcn_freq_roll++;
  }
  else if (!strcmp (c, "per_call_wav"))
  {
    //wav_out_f is the -w file when static_wav_file is set, and a second enable would
    //reopen (and leak) the files of the recording already running
    int on = jv_bool (cmd, "enable", 0);
    if (opts->static_wav_file == 1)
      web_log ("Per-call WAV recording is not available while writing a single WAV file (-w).");
    else if (on == opts->dmr_stereo_wav)
      web_log ("Per-call WAV recording is already %s.", on ? "on" : "off");
    else if (on)
    {
      struct stat st;
      if (stat (opts->wav_out_dir, &st) == -1) mkdir (opts->wav_out_dir, 0700);
      srand (time (NULL));
      opts->wav_out_f = open_wav_file (opts->wav_out_dir, opts->wav_out_file, 8000, 0);
      opts->wav_out_fR = open_wav_file (opts->wav_out_dir, opts->wav_out_fileR, 8000, 0);
      opts->dmr_stereo_wav = 1;
      web_log ("Per-call WAV recording enabled.");
    }
    else
    {
      if (opts->wav_out_f) opts->wav_out_f = close_and_rename_wav_file (opts->wav_out_f, opts->wav_out_file, opts->wav_out_dir, opts->wav_custom_tag, &state->event_history_s[0]);
      if (opts->wav_out_fR) opts->wav_out_fR = close_and_rename_wav_file (opts->wav_out_fR, opts->wav_out_fileR, opts->wav_out_dir, opts->wav_custom_tag, &state->event_history_s[1]);
      opts->wav_out_file[0] = 0; opts->wav_out_fileR[0] = 0; opts->dmr_stereo_wav = 0;
      web_log ("Per-call WAV recording disabled.");
    }
  }
  else if (!strcmp (c, "spectrum"))
  {
    int fft = (int)jv_int (cmd, "fft", 0);
    int fps = (int)jv_int (cmd, "fps", 0);
    int avg = (int)jv_int (cmd, "avg", 0);
    spectrum_configure (fft, fps, avg);
  }
  else if (!strcmp (c, "auto_tune"))
  {
    W.auto_tune = jv_bool (cmd, "enable", 0);
    if (W.auto_tune) bs_stop (opts, state, " (auto-tune enabled)");
    web_log ("Auto-tune %s", W.auto_tune ? "enabled" : "disabled");
  }
  else if (!strcmp (c, "auto_decode"))
  {
    int on = jv_bool (cmd, "enable", 0);
    if (on) bs_stop (opts, state, " (auto-decode enabled)");
    ad_enable (opts, state, on);
  }
  else if (!strcmp (c, "bscan_start"))
  {
    bs_start (opts, state, cmd);
  }
  else if (!strcmp (c, "bscan_stop"))
  {
    bs_stop (opts, state, "");
  }
  else if (!strcmp (c, "bscan_continue"))
  {
    if (W.bs_state == BS_FIXED)
    {
      web_log ("Band scan: continuing.");
      bs_next_peak (opts, state);
    }
  }
  else if (!strcmp (c, "bscan_opts"))
  {
    //settings that can change while a scan runs
    if (jv_has (cmd, "keep_going")) W.bs_keep_going = jv_bool (cmd, "keep_going", 0);
    if (jv_has (cmd, "skip_known")) W.bs_skip_known = jv_bool (cmd, "skip_known", 1);
    if (jv_has (cmd, "exclude"))
    {
      bs_set_excl (jv_get (cmd, "exclude"));
      //the carrier it is on just got excluded: drop it (unsaved) and move on
      int on_carrier = W.bs_state == BS_PROBE || W.bs_state == BS_FOUND || W.bs_state == BS_FIXED;
      long long f = !on_carrier ? 0 : (W.bs_state == BS_FIXED ? W.bs_last_freq : W.bs_peaks[W.bs_peak_idx].freq);
      if (on_carrier && bs_excluded (f, f))
      {
        web_log ("Band scan: %.6f MHz excluded, moving on.", f / 1e6);
        bs_next_peak (opts, state);
      }
    }
    if (jv_has (cmd, "threshold"))
    {
      float t = (float)jv_num (cmd, "threshold", 10);
      W.bs_thr_db = t < 4 ? 4 : (t > 40 ? 40 : t);
    }
    //Exclude & continue after the scan was stopped (any manual tune or decoder change stops it):
    //start it again, on the same bands it picks up at the window where it stopped
    if (jv_bool (cmd, "resume", 0) && W.bs_state == BS_OFF) bs_start (opts, state, cmd);
  }
  else if (!strcmp (c, "auto_analog"))
  {
    W.auto_analog = jv_bool (cmd, "enable", 1);
    if (!W.auto_analog) { W.ad_analog = 0; W.ad_tried = 0; } //resume probing digital decoders
    update_analog (opts, state);
    web_log ("Analog fallback in AUTO / auto-decode %s", W.auto_analog ? "enabled" : "disabled");
  }
  else if (!strcmp (c, "bm_tune"))
  {
    web_bookmark bm;
    if (bm_get ((int)jv_int (cmd, "id", 0), &bm))
    {
      bs_stop (opts, state, " (bookmark tuned)");
      tune_bookmark (opts, state, &bm);
    }
  }
  else if (bm_command (client, c, cmd))
  {
    //bookmark list / save / delete / import
  }
  else if (!strcmp (c, "auto_key"))
  {
    W.auto_key = jv_bool (cmd, "enable", 0);
    W.ak_locked = 0; W.ak_found = 0; W.ak_cand = 0; W.ak_hits = 0; W.ak_checked = 0; W.ak_pdu = 0;
    if (W.auto_key)
    {
      state->keyloader = 0; //don't let the CSV keyloader overwrite our candidate
      opts->dmr_mute_encL = 1; opts->dmr_mute_encR = 1; //stay muted until we lock a key
      web_log ("Auto-key: hunting the DMR Basic Privacy key (it shows in the pauses of encrypted voice and in Motorola data packet headers)...");
    }
    else web_log ("Auto-key disabled.");
  }
  else if (!strcmp (c, "scan_set"))
  {
    //packed, so the cycle (which wraps at lcn_freq_count) reaches every entry
    jv * arr = jv_get (cmd, "freqs");
    const int max = (int)(sizeof(state->trunk_lcn_freq) / sizeof(state->trunk_lcn_freq[0]));
    int dropped = 0;
    memset (state->trunk_lcn_freq, 0, sizeof(state->trunk_lcn_freq));
    state->lcn_freq_count = 0;
    for (jv * e = jv_first (arr); e; e = e->next)
    {
      long f = (long)jv_val (e);
      if (f <= 0) continue;
      if (state->lcn_freq_count < max) state->trunk_lcn_freq[state->lcn_freq_count++] = f;
      else dropped++;
    }
    state->lcn_freq_roll = 0;
    if (dropped) web_log ("Scan list set (%d freqs, %d past the limit of %d left out).", state->lcn_freq_count, dropped, max);
    else web_log ("Scan list set (%d freqs).", state->lcn_freq_count);
  }
  else if (!strcmp (c, "list_files"))
  {
    send_file_list (opts, client, jv_str (cmd, "area", "wav"));
  }
  else if (!strcmp (c, "get_status"))
  {
    W.last_status = 0; //force a push this poll
  }
}

//----------------------------------------------------------------------------
// Auto key-hunt for DMR Basic Privacy (small 255-key space)
//
// Basic Privacy XORs the first 48 bits of every AMBE frame with one fixed mask
// derived from a 1-byte key number via the BPK[] table. The AMBE FEC is checked
// on the ciphered bits, before that, so voice decode errors say nothing about
// the key. Known plaintext does: in speech pauses the vocoder sends the AMBE+2
// silence frame, which BP ciphers like any other, so ciphertext ^ silence is
// the mask itself. A key is applied once two such frames point at the same one
// (a 48-bit match by chance is out of the question). Strong ciphers (AES/DES/
// RC4) are not searchable and are intentionally excluded.
//----------------------------------------------------------------------------

#define AK_SILENCE 0xF801A99F8CE080ULL //AMBE+2 silence, ambe_d[0..48] from bit 55 down (as in dsd_mbe.c)

//does the given slot currently carry an undecrypted Basic Privacy voice frame? (the same
//test processMbeFrame applies before it deciphers BP)
static int slot_is_bp_enc (dsd_state * state, int slot)
{
  if (slot == 0)
    return (state->dmrburstL == 16 && (state->dmr_so & 0x40) && state->dmr_fid == 0x10 && state->payload_keyid == 0);
  return (state->dmrburstR == 16 && (state->dmr_soR & 0x40) && state->dmr_fidR == 0x10 && state->payload_keyidR == 0);
}

//no key or scrambler of any kind currently applied (so ambe_d is still ciphered)
static int no_key_applied (dsd_state * state)
{
  return (state->K == 0 && state->any_bp == 0 && state->ken_sc == 0 && state->straight_ks == 0 &&
          state->tyt_ep == 0 && state->tyt_ap == 0 && state->tyt_bp == 0 && state->retevis_ap == 0 &&
          state->baofeng_ap == 0 && state->csi_ee == 0 && state->vtx_key_loaded == 0 &&
          state->K1 == 0 && state->R == 0);
}

//48-bit Basic Privacy mask for key number k, as processMbeFrame builds it
static uint64_t bp_mask (int k)
{
  uint64_t kk = BPK[k & 0xFF];
  return ((kk & 0xFF0F) << 32) + (kk << 16) + kk;
}

void web_key_capture (dsd_opts * opts, dsd_state * state, int slot, char * ambe_d)
{
  UNUSED(opts);
  if (!W.inited || !W.auto_key || W.ak_locked || W.ak_found) return;
  if (!no_key_applied (state)) return;      //only genuinely ciphered frames
  if (!slot_is_bp_enc (state, slot)) return;
  W.ak_checked++;

  //bit 48 goes out in the clear, it already has to match the silence frame
  if ((ambe_d[48] & 1) != (int)((AK_SILENCE >> 7) & 1)) return;
  uint64_t c = 0;
  for (int j = 0; j < 48; j++) c = (c << 1) | (uint64_t)(ambe_d[j] & 1);
  uint64_t m = c ^ (AK_SILENCE >> 8); //the mask this frame was ciphered with, if it is silence
  for (int k = 1; k < 256; k++)
  {
    if (bp_mask (k) != m) continue;
    if (k == W.ak_cand) W.ak_hits++;
    else { W.ak_cand = k; W.ak_hits = 1; }
    if (W.ak_hits >= 2) W.ak_found = k; //applied on the next poll
    break;
  }
}

//a key the activity log worked out from an encrypted Motorola packet header (only one key fits)
int web_offer_bp_key (int key)
{
  if (!W.inited || !W.auto_key || key < 1 || key > 255) return 0;
  if (W.ak_locked) return W.state && W.state->K == (unsigned long long)key;
  if (!no_key_applied (W.state)) return 0;
  W.ak_found = key;
  W.ak_pdu = 1;
  return 1;
}

static void run_key_hunt (dsd_opts * opts, dsd_state * state)
{
  if (!W.ak_found || W.ak_locked) return;
  state->K = W.ak_found;
  opts->dmr_mute_encL = 0;
  opts->dmr_mute_encR = 0;
  W.ak_locked = 1;
  if (W.ak_pdu)
    web_log ("Auto-key: found DMR Basic Privacy key %d (from the header of an encrypted data packet).", W.ak_found);
  else
    web_log ("Auto-key: found DMR Basic Privacy key %d (%d ciphered silence frames matched, %lu frames checked).",
             W.ak_found, W.ak_hits, W.ak_checked);
}

//----------------------------------------------------------------------------
// Auto tune: snap the dongle onto the strongest peak near the band center
//----------------------------------------------------------------------------

static void run_auto_tune (dsd_opts * opts, dsd_state * state)
{
#ifdef USE_RTLSDR
  if (!W.auto_tune || opts->audio_in_type != 3) return;
  if (opts->p25_trunk || opts->scanner_mode) return; //don't fight trunk/scan tuners
  double now = spectrum_now ();
  if (now - W.last_autotune < 2.0) return;

  W.sf.power = W.sf_power;
  if (!spectrum_copy_latest (&W.sf, 16384)) return;
  int n = W.sf.bins;
  if (n < 16) return;

  //search a window around center, skip the DC spike
  int lo = n / 2 - n / 6, hi = n / 2 + n / 6;
  int guard = n / 128 + 1;
  int best = -1;
  float bestv = 0, noise = 0;
  for (int i = lo; i < hi; i++) noise += W.sf.power[i];
  noise /= (hi - lo);
  for (int i = lo; i < hi; i++)
  {
    if (abs (i - n / 2) < guard) continue;
    if (W.sf.power[i] > bestv) { bestv = W.sf.power[i]; best = i; }
  }
  if (best < 0 || bestv < noise * 8.0f) return; //need a clear peak (~9 dB)

  double hz_per_bin = (double)W.sf.rate / n;
  long offset = (long)((best - n / 2) * hz_per_bin);
  if (labs (offset) < (long)hz_per_bin * 2) { W.last_autotune = now; return; } //already centered
  long target = (long)W.sf.center + offset;
  do_tune (opts, state, target);
  W.last_autotune = now;
  web_log ("Auto-tune: snapped %+ld kHz to %.6f MHz", offset / 1000, target / 1e6);
#else
  UNUSED2(opts, state);
#endif
}

//----------------------------------------------------------------------------
// Auto decode: cycle candidate modes until a sync holds on a fixed frequency
//
// Candidates only advance while the squelch is open, so each one gets a fair
// window on a live signal. A decoder only counts as locked once its sync holds
// unbroken for a while: the short sync words (M17, dPMR, NXDN) false-trigger
// on analog audio, but those syncs drop again within a second.
//
// With the analog fallback on, the passthrough plays any unsynced signal right
// away (no waiting for the probe), and once every candidate has failed on a
// live signal the channel is called analog FM: the probe stops churning
// decoders and parks on AUTO, which still picks up P25/DMR/YSF/D-STAR/X2
// instantly, so mixed analog/digital repeaters keep the verdict. It lasts
// until the next retune.
//----------------------------------------------------------------------------

static void run_auto_decode (dsd_opts * opts, dsd_state * state)
{
  if (!W.auto_decode) return;
  if (opts->p25_trunk || opts->scanner_mode) return;
  double now = spectrum_now ();

  //activity is an open squelch or a sync being held; when it starts after a quiet spell
  //the current decoder gets a fresh window
  int active = channel_active (opts) || state->carrier == 1;
  if (active)
  {
    if (now - W.ad_last_active > 1.0) W.ad_last_switch = now;
    W.ad_last_active = now;
  }

  W.ad_locked = 0;
  if (state->carrier == 1)
  {
    if (W.ad_hold.since == 0) hold_start (&W.ad_hold, now);
    if (!hold_ok (opts, &W.ad_hold, now)) return; //not trusted yet, but don't switch away mid-sync either
    W.ad_locked = 1;
    W.ad_last_sync = now; //locked, hold this mode
    W.ad_tried = 0;
    return;
  }
  W.ad_hold.since = 0;

  if (!active) return; //nothing to probe without a signal

  if (W.ad_analog) return;                  //analog FM: hold
  if (now - W.ad_last_sync < 6.0) return;   //recently had sync, wait a bit longer
  if (now - W.ad_last_switch < 4.0) return; //give each candidate a few seconds

  if (W.ad_tried < AD_COUNT) W.ad_tried++;
  if (W.auto_analog && W.ad_tried >= AD_COUNT)
  {
    W.ad_analog = 1;
    W.ad_idx = AD_AUTO;
    if (ad_current (opts, state) != AD_AUTO) apply_mode (opts, state, "auto");
    web_log ("Auto-decode: no digital sync on an active channel, treating it as analog FM.");
    return;
  }
  W.ad_idx = (W.ad_idx + 1) % AD_COUNT;
  apply_mode (opts, state, ad_mode_name (W.ad_idx));
  W.ad_last_switch = now;
  web_log ("Auto-decode: trying %s", opts->output_name);
}

//----------------------------------------------------------------------------
// Status snapshot
//----------------------------------------------------------------------------

static const char * burst_label (unsigned int b)
{
  static const char * t[32] = {
    "PI","VLC","TLC","CSBK","MBCH","MBCC","DATA","R12D","R34D","IDLE","R1_D","ERR",
    "DUID ERR","R-S ERR","CRC ERR","NULL","VOICE","","INIT","INIT","PTT","VOICE",
    "HANGTIME","PTT END","IDLE","HDU","VOICE","VOICE","TDU/LC","TSBK","SIGNAL","SIGNAL" };
  if (b < 32) return t[b];
  return "";
}

static void add_slot (jb * b, const char * key, dsd_state * state, int slot, int on)
{
  char enc[64];
  web_enc_name (state, slot, enc, sizeof(enc));
  jb_obj (b, key);
  jb_str (b, "enc", enc);
  if (slot == 0)
  {
    jb_int (b, "tg", state->lasttg);
    jb_int (b, "src", state->lastsrc);
    jb_str (b, "burst", burst_label (state->dmrburstL));
    jb_int (b, "alg", state->payload_algid);
    jb_hex (b, "key", (unsigned long long)state->payload_keyid, 4);
    jb_str (b, "call", state->call_string[0]);
    jb_str (b, "gps", web_call_gps (0));
    jb_str (b, "data", state->dmr_lrrp_gps[0]);
    jb_str (b, "alias", state->generic_talker_alias[0]);
    jb_int (b, "gi", state->gi[0]);
    jb_int (b, "err", state->errs & 0xF);
    jb_int (b, "err2", state->errs2 & 0xF);
    jb_bool (b, "on", on);
  }
  else
  {
    jb_int (b, "tg", state->lasttgR);
    jb_int (b, "src", state->lastsrcR);
    jb_str (b, "burst", burst_label (state->dmrburstR));
    jb_int (b, "alg", state->payload_algidR);
    jb_hex (b, "key", (unsigned long long)state->payload_keyidR, 4);
    jb_str (b, "call", state->call_string[1]);
    jb_str (b, "gps", web_call_gps (1));
    jb_str (b, "data", state->dmr_lrrp_gps[1]);
    jb_str (b, "alias", state->generic_talker_alias[1]);
    jb_int (b, "gi", state->gi[1]);
    jb_int (b, "err", state->errsR & 0xF);
    jb_int (b, "err2", state->errs2R & 0xF);
    jb_bool (b, "on", on);
  }
  jb_obj_end (b);
}

static void build_status (dsd_opts * opts, dsd_state * state, jb * b)
{
  jb_reset (b);
  jb_obj (b, NULL);
  jb_str (b, "type", "status");
  jb_dbl (b, "t", spectrum_now (), 3);

  //input
  jb_obj (b, "input");
  const char * itype = "pulse";
  if (opts->audio_in_type == 3) itype = "rtl";
  else if (opts->audio_in_type == 8) itype = "tcp";
  else if (opts->audio_in_type == 2) itype = "wav";
  else if (opts->audio_in_type == 4) itype = "bin";
  jb_str (b, "type", itype);
  jb_int (b, "freq", current_freq (opts));
  jb_int (b, "gain", opts->rtl_gain_value);
  jb_int (b, "gain_actual", opts->rtl_gain_actual);
  jb_bool (b, "agc", opts->rtl_gain_value == 0);
  jb_int (b, "gain_manual", W.gain_manual);
  jb_int (b, "ppm", opts->rtlsdr_ppm_error);
  jb_int (b, "vol", opts->rtl_volume_multiplier);
  jb_int (b, "squelch", opts->rtl_squelch_level);
  jb_int (b, "rms", opts->rtl_rms);
  jb_int (b, "bw", opts->rtl_bandwidth);
#ifdef USE_RTLSDR
  if (opts->audio_in_type == 3)
  {
    jb_int (b, "rate", rtl_dev_rate ());
    jb_int (b, "rate_target", rtl_dev_rate_target ());
    jb_bool (b, "dagc", opts->rtl_digital_agc);
  }
#endif
  jb_bool (b, "rigctl", opts->use_rigctl);
  jb_int (b, "setmod_bw", opts->setmod_bw);
  jb_obj_end (b);

  //decoder
  jb_obj (b, "decode");
  jb_str (b, "name", opts->output_name);
  jb_bool (b, "carrier", state->carrier);
  jb_str (b, "ftype", state->ftype);
  jb_int (b, "synctype", state->lastsynctype);
  jb_str (b, "sys", (state->lastsynctype >= 0 && state->lastsynctype < 44) ? SyncTypes[state->lastsynctype] : "");
  const char * mod = opts->mod_qpsk ? "QPSK" : (opts->mod_gfsk ? "GFSK" : "C4FM");
  jb_str (b, "mod", mod);
  jb_int (b, "rate", (48000 * opts->wav_interpolator) / (state->samplesPerSymbol ? state->samplesPerSymbol : 10));
  int level = 0;
  if (state->carrier)
  {
    level = (int)(state->max / (opts->mod_qpsk ? 328 : 164));
    if (opts->audio_in_type == 4) level = 50;
    if (level > 100) level = 100;
    if (level < 0) level = 0;
  }
  jb_int (b, "level", level);
  jb_str (b, "mode", mode_key (opts, state));
  jb_bool (b, "analog", spectrum_now () - W.analog_last < 0.6); //analog audio is playing
  jb_bool (b, "monitor", opts->monitor_input_audio);           //analog passthrough armed
  jb_bool (b, "stereo", opts->dmr_stereo);
  jb_int (b, "channels", opts->pulse_digi_out_channels);
  jb_int (b, "digi_rate", opts->pulse_digi_rate_out);
  jb_bool (b, "floating_point", opts->floating_point);
  jb_dbl (b, "digi_gain", state->aout_gain * 2, 0);
  jb_dbl (b, "analog_gain", opts->audio_gainA, 0);
  jb_obj_end (b);

  //trunking / scanner
  jb_obj (b, "trunk");
  jb_bool (b, "enabled", opts->p25_trunk);
  jb_bool (b, "scanner", opts->scanner_mode);
  jb_bool (b, "tuned", opts->p25_is_tuned);
  jb_int (b, "cc_freq", state->p25_cc_freq);
  jb_int (b, "vc_freq", state->p25_vc_freq[0]);
  jb_bool (b, "group", opts->trunk_tune_group_calls);
  jb_bool (b, "private", opts->trunk_tune_private_calls);
  jb_bool (b, "data", opts->trunk_tune_data_calls);
  jb_bool (b, "enc", opts->trunk_tune_enc_calls);
  jb_bool (b, "allow_list", opts->trunk_use_allow_list);
  jb_dbl (b, "hangtime", opts->trunk_hangtime, 2);
  jb_int (b, "tg_hold", state->tg_hold);
  jb_int (b, "scan_count", state->lcn_freq_count);
  jb_int (b, "scan_roll", state->lcn_freq_roll);
  jb_obj_end (b);

  //auto features + filters + keys
  jb_obj (b, "flags");
  jb_bool (b, "auto_tune", W.auto_tune);
  jb_bool (b, "auto_decode", W.auto_decode);
  jb_bool (b, "lpf", opts->use_lpf);
  jb_bool (b, "hpf", opts->use_hpf);
  jb_bool (b, "pbf", opts->use_pbf);
  jb_bool (b, "hpfd", opts->use_hpf_d);
  jb_bool (b, "payload", opts->payload);
  jb_bool (b, "monitor", W.user_monitor);
  jb_bool (b, "auto_analog", W.auto_analog);
  jb_bool (b, "inverted", opts->inverted_dmr);
  jb_bool (b, "key_loaded", state->keyloader || state->K || state->R || state->A1[0] || state->K1 || state->tyt_ap || state->tyt_ep);
  jb_int (b, "forced_alg", state->forced_alg_id);
  jb_bool (b, "auto_key", W.auto_key);
  jb_bool (b, "rec_wav", opts->dmr_stereo_wav);
  jb_obj_end (b);

  //auto-decode progress
  jb_obj (b, "auto");
  const char * ad = "off";
  if (W.auto_decode)
  {
    if (opts->p25_trunk || opts->scanner_mode) ad = "paused";
    else if (W.ad_locked && state->carrier) ad = "sync";
    else if (W.ad_analog) ad = "analog";
    else if (!channel_active (opts)) ad = "idle";
    else ad = "probe";
  }
  jb_str (b, "state", ad);
  jb_int (b, "tried", W.ad_tried);
  jb_int (b, "total", AD_COUNT);
  jb_obj_end (b);

  //band scan progress
  jb_obj (b, "bscan");
  static const char * bs_names[] = { "off", "sweep", "probe", "found", "fixed" };
  jb_str (b, "state", bs_names[W.bs_state]);
  if (W.bs_state != BS_OFF)
  {
    int probing = W.bs_state != BS_TUNE && W.bs_peak_idx < W.bs_npeaks;
    jb_int (b, "freq", probing ? W.bs_peaks[W.bs_peak_idx].freq : W.bs_win);
    jb_int (b, "win", W.bs_win_idx + 1);
    jb_int (b, "wins", W.bs_win_total);
    jb_int (b, "pass", W.bs_pass);
    jb_int (b, "left", probing ? W.bs_npeaks - W.bs_peak_idx - 1 : 0);
    jb_int (b, "cand", W.bs_cand + 1);
    jb_int (b, "ncand", AD_COUNT);
    jb_str (b, "desc", W.bs_desc);
  }
  int skipping = 0;
  for (int i = 0; i < W.bs_nskip; i++) skipping += spectrum_now () - W.bs_skip_t[i] < BS_SKIP_SECS;
  jb_int (b, "found", W.bs_found);
  jb_int (b, "skipping", skipping);
  jb_int (b, "last_freq", W.bs_last_freq);
  jb_str (b, "last_name", W.bs_last_name);
  jb_bool (b, "skip_known", W.bs_skip_known);
  jb_bool (b, "keep_going", W.bs_keep_going);
  jb_int (b, "excluded", W.bs_nex);
  jb_dbl (b, "threshold", W.bs_thr_db, 0);
  jb_str (b, "msg", W.bs_msg);
  jb_obj_end (b);

  //key-hunt progress
  jb_obj (b, "key_hunt");
  jb_bool (b, "active", W.auto_key);
  jb_bool (b, "locked", W.ak_locked);
  jb_int (b, "checked", (long long)W.ak_checked);
  jb_int (b, "found_key", W.ak_locked ? (int)state->K : 0);
  jb_obj_end (b);

  //per-slot call info
  add_slot (b, "slot1", state, 0, opts->slot1_on);
  add_slot (b, "slot2", state, 1, opts->slot2_on);

  //DMR / P25 / NXDN site context
  jb_obj (b, "site");
  jb_int (b, "dmr_cc", state->dmr_color_code);
  jb_str (b, "dmr_brand", state->dmr_branding);
  jb_str (b, "dmr_brand_sub", state->dmr_branding_sub);
  jb_str (b, "dmr_site", state->dmr_site_parms);
  jb_uint (b, "p2_wacn", state->p2_wacn);
  jb_uint (b, "p2_sysid", state->p2_sysid);
  jb_uint (b, "p2_cc", state->p2_cc);
  jb_uint (b, "p2_rfss", state->p2_rfssid);
  jb_uint (b, "p2_site", state->p2_siteid);
  jb_int (b, "nxdn_ran", (int)state->nxdn_last_ran);
  jb_int (b, "nxdn_tg", state->nxdn_last_tg);
  jb_int (b, "nxdn_rid", state->nxdn_last_rid);
  jb_int (b, "nxdn_cipher", state->nxdn_cipher_type);
  jb_obj_end (b);

  //the decoder's own event history (most recent first, both slots), only while a browser shows it
  jb_arr (b, "events");
  if (web_server_subs () & WEB_SUB_EVENTS)
  {
    const char * empty = "BUMBLEBEETUNA";
    int emitted = 0;
    for (int i = 0; i < 30 && emitted < 24; i++)
    {
      for (int slot = 0; slot < 2 && emitted < 24; slot++)
      {
        Event_History * e = &state->event_history_s[slot].Event_History_Items[i];
        if (strncmp (e->event_string, empty, 13) == 0) continue;
        const char * es = e->event_string;
        while (*es == ' ') es++;
        if (!*es) continue; //the in-progress entry of an idle slot
        jb_obj (b, NULL);
        jb_int (b, "slot", slot + 1);
        jb_str (b, "text", e->event_string);
        if (strncmp (e->text_message, empty, 13) != 0) jb_str (b, "msg", e->text_message);
        if (strncmp (e->alias, empty, 13) != 0) jb_str (b, "alias", e->alias);
        if (strncmp (e->gps_s, empty, 13) != 0) jb_str (b, "gps", e->gps_s);
        if (strncmp (e->internal_str, empty, 13) != 0) jb_str (b, "note", e->internal_str);
        jb_int (b, "color", e->color_pair);
        jb_int (b, "time", (long long)e->event_time);
        jb_obj_end (b);
        emitted++;
      }
    }
  }
  jb_arr_end (b);

  //active trunking channel strings
  jb_arr (b, "channels");
  for (int i = 0; i < 31; i++)
    if (state->active_channel[i][0]) jb_str (b, NULL, state->active_channel[i]);
  jb_arr_end (b);

  jb_obj_end (b);
}

//----------------------------------------------------------------------------
// Public entry points
//----------------------------------------------------------------------------

void web_init (dsd_opts * opts, dsd_state * state)
{
  W.opts = opts;
  W.state = state;
  snprintf (W.password, sizeof(W.password), "%s", opts->web_password);
  W.status_interval = 0.2; //5 Hz status
  W.inited = 1;
  //analog fallback on by default; the passthrough flag is set now so the
  //startup code opens the analog output together with the digital one
  W.auto_analog = 1;
  W.bs_thr_db = 10;
  W.bs_skip_known = 1;
  W.cosine_default = opts->frame_m17 ? 1 : opts->use_cosine_filter; //-fz turns it off for M17 only
  W.user_monitor = opts->monitor_input_audio && !mode_is_analog (opts);
  W.gain_manual = opts->rtl_gain_value > 0 ? opts->rtl_gain_value : 0; //0: started on AGC, the page keeps its own
  opts->monitor_input_audio = analog_wanted (opts);
  bm_init (opts->web_bookmarks);
  fprintf (stderr, "Web bookmarks: %s\n", bm_path ());
  web_act_init (opts, state);
  spectrum_start ();
  //expose the per-call WAV directory for browser download (and nothing else: the working
  //directory can be anyone's home, and without a password downloads need no token)
  web_server_set_area ("wav", opts->wav_out_dir);
  web_log ("DSD-FME web interface ready.");
}

void web_poll (dsd_opts * opts, dsd_state * state)
{
  if (!W.inited) return;

  //drain commands queued by the server thread
  int client;
  char * json;
  int budget = 64;
  while (budget-- > 0 && web_pop_command (&client, &json))
  {
    jv * cmd = jv_parse (json, strlen (json));
    if (cmd)
    {
      apply_command (opts, state, client, cmd);
      jv_free (cmd);
    }
    free (json);
  }

  update_analog (opts, state);
  run_auto_tune (opts, state);
  run_auto_decode (opts, state);
  run_band_scan (opts, state);
  run_key_hunt (opts, state);
  web_act_poll (opts, state);

  //broadcast status at a steady rate when someone is connected
  double now = spectrum_now ();
  if (web_server_clients () > 0 && now - W.last_status >= W.status_interval)
  {
    static jb b;
    build_status (opts, state, &b);
    web_broadcast (WEB_KIND_STATUS, WS_OP_TEXT, b.buf, b.len, 0);
    W.last_status = now;
  }
}

void web_shutdown (dsd_opts * opts, dsd_state * state)
{
  UNUSED2(opts, state);
  if (!W.inited) return;
  spectrum_stop ();
  web_server_stop ();
  W.inited = 0;
}

//8 kHz decoded voice (or 48 kHz analog) tap -> web audio subscribers
void web_audio_push (dsd_opts * opts, const void * data, size_t nbytes, int analog)
{
  if (!W.inited || nbytes == 0) return;
  if (analog) W.analog_last = spectrum_now ();
  if (!(web_server_subs () & WEB_SUB_AUDIO)) return;

  uint8_t hdr[8];
  hdr[0] = WEB_BIN_AUDIO;
  hdr[1] = analog ? 1 : (uint8_t)opts->pulse_digi_out_channels; //channels
  hdr[2] = analog ? 0 : (uint8_t)(opts->floating_point ? 1 : 0); //0 int16, 1 float32
  uint16_t rate_div = analog ? 480 : (uint16_t)(opts->pulse_digi_rate_out / 100); //rate/100
  hdr[3] = (uint8_t)(rate_div & 0xFF);
  hdr[4] = (uint8_t)((rate_div >> 8) & 0xFF);
  hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;

  uint8_t * msg = malloc (8 + nbytes);
  if (!msg) return;
  memcpy (msg, hdr, 8);
  memcpy (msg + 8, data, nbytes);
  web_broadcast (WEB_KIND_AUDIO, WS_OP_BINARY, msg, 8 + nbytes, WEB_SUB_AUDIO);
  free (msg);
}

//called as getFrameSync starts: carrier still set means the previous frame was accepted
void web_frame_tap (dsd_state * state)
{
  if (W.inited && state->carrier == 1) W.frames++;
}

//a decoder found a frame whose FEC checked out (M17 LICH)
void web_frame_good (void)
{
  if (W.inited) W.good++;
}

void web_event_pushed (Event_History_I * event_struct)
{
  UNUSED(event_struct);
  //status push carries events; nothing extra needed here
}

void web_symbol_tap (int symbol)
{
  UNUSED(symbol);
}
