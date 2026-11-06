# README screenshots

`shoot.mjs` retakes the screenshots in `docs/screenshots/` from a dsd-fme
instance on a simulated receiver, with made-up demo data. It needs no dongle,
reads nothing from your config and shows no real traffic.

```
cmake -B build && cmake --build build -j"$(nproc)"
node tools/screenshots/shoot.mjs                         # writes build/screenshots/web-*.webp
node tools/screenshots/shoot.mjs --out docs/screenshots  # replaces the README's
node tools/screenshots/shoot.mjs --shots desktop,phone   # just those
```

You need Node 22 or newer, a C compiler, Chromium or Chrome (`--chromium` if
it isn't on the PATH as `chromium` or `google-chrome`), and ImageMagick for
WebP (without it you get PNGs). `nm` (binutils) is optional. The map shot
loads OpenStreetMap tiles, so it needs internet. A run takes about a minute.

## What it does

1. Builds `fakertl.c`, a stand-in for librtlsdr, into a temporary directory
   and checks with `nm` that it defines every `rtlsdr_*` function dsd-fme
   calls.
2. Starts `build/dsd-fme -i rtl:0:433.475M:0:0:12 -fs -o null` with the stand-in
   loaded through `LD_PRELOAD`. The web server listens on a free port on
   127.0.0.1. The bookmarks and config live in the temporary directory, which
   is also dsd-fme's working directory.
3. Opens the page in headless Chromium and sets the RTL sample rate to
   0.25 MSPS. At the default ~1 MSPS, the 12 kHz channel band would be about
   12 px wide.
4. Runs `demo_page.js` in the page. It replaces what the decoder reports
   (status, slots, activity log, system) with made-up radios, talkgroups,
   positions and messages. The spectrum and waterfall stay live.
5. Takes the desktop shot (1440×900), the activity log and radios, the map,
   and the phone shot (390×844 at 2×). The spectrum shots are taken a few
   seconds into a transmission of the bursty M17 hotspot, so it shows in the
   trace and in the waterfall.
6. Converts them to WebP with `-strip`, checks they carry no metadata, then
   stops dsd-fme and Chromium and deletes the temporary directory (`--keep`
   keeps it, with the dsd-fme log).

The simulated band is the tuned "Demo repeater" at 433.475 MHz, an analog
simplex at 433.425, the M17 hotspot at 433.525 (on for 6 s of every 20) and a
carrier at 433.575. All of it is made up, like everything else on the page.
Stations have to sit between -rate/4 and +rate/2 of the tuned frequency,
because the left quarter of the spectrum shows the wrapped top of the capture.

`fakertl.c` also works on its own for testing the RTL path without hardware;
its header lists the station types (`FAKERTL_STATIONS`).

## Keep it demo data

Captures and recordings hold real third-party traffic: don't point this at
them, and don't put real frequencies, radio IDs or positions in
`demo_page.js`, the bookmarks or the station list.
