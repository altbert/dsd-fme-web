# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

A fork of [DSD-FME](https://github.com/lwvmobile/dsd-fme), a C/C++ decoder for digital voice and data radio (DMR, P25 Phase 1/2, NXDN, dPMR, YSF, D-STAR, M17, EDACS, ProVoice, X2-TDMA). It targets Linux desktops first. The fork adds a browser interface (`--web`) and keeps merging upstream `lwvmobile/dsd-fme` branch `audio_work`, so edits to upstream files should stay small and easy to carry through merges.

Remotes: `origin` is altbert/DSD-FME-web, `upstream` is lwvmobile/dsd-fme. Merge with `git fetch upstream && git merge upstream/audio_work`.

CLI usage is in examples/Example_Usage.md, install steps in examples/Install_Notes.md, the web UI in examples/Web_Interface.md.

Open bugs and planned work are in TODO.md: add what you find, remove what you fix.

## Build

```
cmake -B build && cmake --build build -j"$(nproc)"    # binary: build/dsd-fme
```

- Required: libsndfile, mbelib (lwvmobile's fork, see Install_Notes.md), ncursesw, PulseAudio. Optional: librtlsdr (`-i rtl` and every RTL feature of the web UI; defines `USE_RTLSDR`) and codec2 (M17 voice).
- Sources are globbed (`src/*.c src/*.cpp`): re-run cmake after adding a file and after upstream merges, which add files.
- Feature switches are the cmake `-D` options at the top of CMakeLists.txt.
- There is no test suite or linter; changes are checked by running the binary (see below).

## Architecture

**Decoder (upstream).** `dsd_main.c` parses options into `dsd_opts` and runs `liveScanner`. `getFrameSync` (dsd_frame_sync.c) reads symbols through dsd_symbol.c (from the RTL-SDR demod in rtl_sdr_fm.cpp, PulseAudio, TCP, files…), finds a sync pattern and hands the frame to the protocol code (`dmr_*`, `p25p1_*`, `p25p2_*`, `nxdn_*`, `dpmr_*`, `ysf.c`, `dstar.c`, `m17.c`, `edacs-fme.c`, `provoice.c`, `x2tdma_*`). Everything shares two large structs from include/dsd.h, passed by pointer everywhere: `dsd_opts` (settings) and `dsd_state` (runtime). Voice goes through mbelib (dsd_mbe.c) to the outputs in dsd_audio*.c; the ncurses terminal UI is dsd_ncurses_*.c.

**Web layer (this fork):** `src/dsd_web*.c`, `src/dsd_spectrum.c`, `include/dsd_web.h`.
- `dsd_web_server.c` runs its own thread: HTTP, WebSocket, the optional password (`--web-pass`) with a session token for downloads, static files and download areas (per-call WAVs). Browser commands are only queued here.
- `dsd_web.c` runs on the decoder thread through `web_poll()`, which is called from the frame-sync loop and from the protocols' own long loops (dmr_bs.c, dmr_ms.c, dstar.c, edacs-fme.c, p25p2_frame.c). It pops the queued commands, applies them to opts/state and broadcasts the `status`/`system` JSON, so the web layer only changes opts/state from the decoder thread.
- `dsd_web_data.c` is the activity module: call tracking, decoded packet data (LRRP, ARS, text, P25 packet data), the P25 site picture from TSBKs, trunking follow/skip and the radios list. Taps in the decoder files feed it.
- `dsd_spectrum.c`: the RTL demod thread pushes IQ (`spectrum_push_iq`) and a worker thread turns it into FFT frames.
- Inputs: the RTL features (spectrum, band scan, auto-tune, the squelch's Auto floor, the RF tab's RTL settings) check `opts->audio_in_type == 3`, which is `-i rtl` and also upstream's `-i rtltcp`, so device calls must go through upstream's `rtlsdr_or_tcp_*` wrappers (rtl_sdr_tcp.c) to reach an rtl_tcp server. Tuning (`do_tune`) and trunking also drive SDR++ over rigctl (`-U`). `-i tcp` and `-i pulse` carry audio only: decoding works, there is no IQ. The user-facing table is the Receivers section of examples/Web_Interface.md.
- `dsd_web_json.c` is a small JSON parser (`jv_*`) and builder (`jb_*`). `dsd_web_bookmarks.c` keeps bookmarks in `~/.config/dsd-fme/web_bookmarks.json` (`--web-bookmarks` overrides).
- Wire format: text messages are JSON with a `type` (`hello`, `auth`, `status`, `activity`, `system`, `groups`, `files`, `bookmarks`…); binary frames start with a type byte, 1 = FFT, 2 = audio. The browser sends `{"cmd": ...}` objects.

**Taps in upstream files** are guarded by `opts->web_enable == 1` and kept to a line or two; the logic belongs in `dsd_web_*.c`. Console output without `--web` must stay byte-identical to upstream: record a capture with `-c file.bin` while decoding, replay it with `-i file.bin -o null -Z` through an upstream build and this one, and compare the logs.

## Web UI

- `web/` (index.html, app.js, fonts) is embedded into `src/dsd_web_assets.c` by `tools/gen_web_assets.py`. The generated file is committed so a build needs no Python: after editing web/, run `make -C build web_assets` and commit both. Everything in web/ ends up in the binary, so keep other files (screenshots go in docs/screenshots/) out of it.
- `--web-root ./web` serves the files from disk, so edits show on reload without rebuilding.
- Fonts: Barlow Semi Condensed as a Latin subset with hinting kept (unhinted subsets space letters unevenly in Chrome on Linux) and a 7-segment face generated by `tools/gen_segment_font.py`.
- Design rules: one meaning per color (amber readouts; green live, sync, on air; red failures; violet encryption; blue links and bookmarks). Nothing on the page moves when calls start or data arrives (fixed-size boxes, ellipsis, fixed column widths). Layouts work down to 320 px wide.

## Running it for checks

- The user often has their own `dsd-fme -i rtl --web <port>` running and holding the RTL-SDR. Never stop it; start a second instance on another port.
- Start test instances from a scratch directory (the Files tab lists `./WAV` of the working directory), with `-o null` so nothing plays on the speakers, and with their own `--web-bookmarks` file.
- Without hardware, `LD_PRELOAD` a stand-in librtlsdr that synthesizes signals; dsd-fme only calls about 13 `rtlsdr_*` functions.
- Stop instances by PID (`ss -ltnp`), not `pkill -f "--web ..."`, which also matches the shell running the command.
- Drive and screenshot the UI with headless Chromium over the DevTools protocol (`--remote-debugging-port`).

## Data hygiene

Captures and recordings hold real third-party traffic. Never commit `build/WAV/`, `.bin`/`.wav` captures, key files, or real frequencies, IDs or positions; screenshots use demo data. `.gitignore` ignores `examples/`, `*.csv` and `*.sh`, so new files there need `git add -f`.
