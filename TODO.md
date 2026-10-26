# TODO

Open bugs and planned work for this fork, mostly the web interface. Bugs in the original DSD-FME are reported to [lwvmobile/dsd-fme](https://github.com/lwvmobile/dsd-fme) instead of fixed here, so merges stay easy; the ones seen so far are listed at the end.

## Fix

- [ ] **Random crashes in long `-i rtl --web` sessions.** `coredumpctl list dsd-fme` holds eight crashes of `./dsd-fme -i rtl --web 8083` since 2026-10-21; `coredumpctl info <pid>` shows each stack.
  - SIGSEGV 2026-10-26 (build 5caaf7c): `getFrameSync` writing a dibit through `state->dibit_buf_p` (dsd_frame_sync.c:330). The pointer held 0x7fbe00000320, a heap address whose low 32 bits had been replaced by 800: it was overwritten, not run off the buffer (every write checks the bound). `dibit_buf_p` is the second field of `dsd_state`, and `opts` and `state` are locals of `main`, so the stray write came from outside the struct, probably a neighbor on `main`'s stack.
  - SIGSEGV 2026-10-22: `watchdog_event_history` reading through a pointer mangled the same way (0xfffff9fd00000745).
  - SIGSEGV 2026-10-22: in libc under `pa_simple_write`, from `playSynthesizedVoiceMS` (P25 voice, `processLDU1`).
  - SIGFPE 2026-10-22 and 2026-10-25: `getSymbol`, whose only division by a variable is `symbol = sum / count` (dsd_symbol.c:599). `count` stays 0 when no sample lands in the window it averages, which depends on `symbolCenter`, `samplesPerSymbol` and `rf_mod`: a mode switch may leave them inconsistent.
  - SIGFPE ×3 2026-10-21: `dc_block_filter` dividing by a `result_len` of 0 in the RTL demod thread. Not seen since.
  - Next: run the usual session on an ASan/UBSan build, which reports a stray write where it happens.

- [ ] **rtl_tcp: capture rate, bandwidth and RTL AGC from the web.** `rtl_dev_set_rate`, `rtl_dev_set_bandwidth` and `rtl_dev_set_agc` (src/rtl_sdr_fm.cpp) call librtlsdr directly, but with `-i rtltcp` there is no local device. After a rate or bandwidth change the tuner isn't re-centered, so the receiver ends up off frequency, and the AGC toggle fails with a warning. Use `rtlsdr_or_tcp_set_center_freq`; AGC needs an rtl_tcp wrapper (the protocol has a set-AGC-mode command) or the toggle hidden over rtl_tcp. Found by reading the code, not reproduced yet.
- [ ] **Spectrum edge alias.** The dongle captures from f − rate/4 to f + 3·rate/4, so the leftmost quarter of the spectrum and waterfall shows signals from the top of the capture, one sample rate higher than drawn. Clicking there tunes to the wrong frequency. The band scan already skips that part; grey it out or leave it undrawn on the page too.

## Test

- [ ] **`-i rtltcp` with the web page**: spectrum, tuning, gain, band scan, trunking. It goes through the same receive path as `-i rtl`, so it should work, but it hasn't been tried.
- [ ] **SDR++ over rigctl (`-i tcp -U 4532`)**: tuning by frequency and bookmark, and the Trunking card. The code handles rigctl; it hasn't been tried.

## Add

- [ ] **Voice transcription.** Turn decoded calls into text with a local speech-to-text model (Whisper through whisper.cpp, which is MIT-licensed C/C++ and runs on a CPU, or another model that works offline) and show the text on the call's activity item, searchable with the filter.
  - Local only: the audio is other people's traffic, so no cloud services.
  - Off the decoder thread: a worker thread or a helper process, fed one finished call at a time. The per-call WAVs already exist, so a first version can transcribe those.
  - Decoded voice is 8 kHz vocoder audio and Whisper takes 16 kHz, so resample, and expect rough accuracy; mark transcripts as machine-made.
  - Skip calls muted for a missing key. Optional at build time (a cmake switch) and at run time (a flag or a toggle).
- [ ] **Trunking card for DMR and NXDN.** The talkgroup list, the follow reasons and the followed call come from P25 control channels only (the TSBK tap in p25p1_tsbk.c). On DMR (Capacity Plus, Connect Plus, Tier III, XPT) and NXDN systems the decoder still follows calls, but the card shows no grants.
- [ ] **Other SDR hardware directly** (SoapySDR or similar), so HackRF, Airspy, SDRplay… get the spectrum, band scan and RF controls instead of only decoding through SDR++. A big change to the input code, better proposed upstream.
- [ ] **Test harness in the repo.** The stand-in librtlsdr that synthesizes signals (for testing without a dongle) and the activity-module unit tests only exist in scratch directories. Commit them, e.g. under tools/, after replacing real frequencies and IDs with made-up ones.
- [ ] **AM for analog voice.** An "Analog AM" decoder next to "Analog FM (no digital)", for airband (118–137 MHz) and other AM voice.
  - The RTL demod comes from rtl_fm, whose `am_demod` (the envelope, √(I² + Q²)) was left out. Add it back in rtl_sdr_fm.cpp with a switch the web flips while running (FM de-emphasis off meanwhile), next to the other `rtl_dev_*` additions. Choosing any other decoder switches back to FM.
  - The rest exists: the DC blocker removes the carrier, and the analog path has the squelch (Auto floor works as is, it measures RF), audio AGC, filters and browser audio. Bookmarks can store the mode.
  - RTL input only (with SDR++, pick AM there); no digital decoding while it's on. About 50 lines, plus an AM station type in the stand-in librtlsdr to test it.
- [ ] **Sweep the spectrum without retuning.** Step through the carriers inside the current span by moving a software VFO instead of the dongle: a frequency shift (NCO) on the IQ before the channel filter in rtl_sdr_fm.cpp, so the demod listens at center + offset while the capture stays where it is. No PLL settling or samples from the old frequency between steps, and the spectrum doesn't jump. The band scan's peak finder (`bs_find_peaks`) can supply the carriers; the page needs a VFO mark on the spectrum. Only the clean part of the capture is usable (see the spectrum edge alias item).
- [ ] **Scan bookmarks.** Scan the bookmarks themselves: the ones ticked in the list, or all of a group or tag (bookmarks have a group but no tags yet), each with its own decoder. Today "From bookmarks" only copies the first 26 filtered frequencies into the conventional scan list, which holds 26 and runs one decoder for all of them. Needs a way to pick bookmarks, a dwell and hang time, and skip and hold keys like the Trunking card's.

## Upstream bugs seen (report to lwvmobile/dsd-fme)

- `getFrameSync` (dsd_frame_sync.c) copies the 24 symbols before `synctest_p` (48 for EDACS) without checking that many have been read, so it reads before `synctest_buf` right after entry for the short frame types (M17, NXDN, dPMR) and after the buffer wraps. AddressSanitizer flags it without `--web` too.
- `-b` clamps the key number to 256, but `BPK` (include/bp.h) has entries 0–255, so `-b 256` reads past the end of the table.
- `rtl_return_rms` (rtl_sdr_fm.cpp) smooths with `(prev_rms * 3 + sr) / 4` in integers, which truncates: any value up to 3 under the input is a resting point, so the RMS reads 2–3 counts low (about 10 % on a quiet channel at low gain). Minor; the web's Auto floor squelch just sits that much further over it.
- P25 response packets that carry a block (SACK) go through `p25_decode_pdu_data`, which reads their status byte as a SAP, so the console reports them as encrypted or unknown IP data. The web log decodes them correctly.
- AddressSanitizer on 2026-10-21 also reported a stack overflow in `ysf_soft_decision_viterbi` (AUTO mode on noise), a memcpy over-read in dmr_dburst.c and undefined shifts in dmr_block.c and `dmr_locn`. Re-check on current upstream before reporting.
