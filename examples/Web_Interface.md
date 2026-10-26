# DSD-FME Web Interface

DSD-FME includes an optional browser-based client: a live FFT spectrum +
waterfall (like GQRX/SDR++), frequency scanning, conventional auto-tune and
auto-decode, decoded-audio streaming, the full decoder control surface, and
encrypted-comms key entry. It is served by a small embedded HTTP/WebSocket
server inside `dsd-fme` itself — no extra dependencies, nothing else to run.

## Enabling it

```
dsd-fme -i rtl --web 8080
```

Then open `http://localhost:8080/` in a browser.

| Flag | Meaning |
|------|---------|
| `--web [bind:]port` | Enable the server. Default bind is `127.0.0.1`, default port `8080`. |
| `--web 0.0.0.0:8080` | Listen on all interfaces (LAN access). **Set a password if you do this.** |
| `--web-pass <pw>` | Require a password before any control works. |
| `--web-root <dir>` | Serve the UI from a directory on disk (for UI development) instead of the copy embedded in the binary. |

The web interface runs alongside everything else — you can use it on its own,
or together with the `-N` NCurses terminal and the usual audio output.

### Examples

```
# Localhost only, RTL-SDR input, pick a frequency / mode in the browser
dsd-fme -i rtl --web 8080

# DMR trunking on a LAN, password protected
dsd-fme -fs -i rtl:0:450M:0:-2:12 -T -C connect_plus_chan.csv -G group.csv \
        --web 0.0.0.0:8080 --web-pass hunter2

# Any other SDR through SDR++: audio over TCP, tuning over rigctl (no spectrum, see below)
dsd-fme -i tcp -U 4532 --web 8080
```

## Receivers: RTL-SDR and other SDRs

DSD-FME opens RTL-SDR dongles itself. Any other radio works through SDR
software that demodulates the signal and sends DSD-FME the audio.

| Input | Radio | What the web page can do |
|-------|-------|--------------------------|
| `-i rtl` | RTL-SDR dongle on this computer | Everything |
| `-i rtltcp:host:port:...` | RTL-SDR dongle on another machine running `rtl_tcp` | Everything in principle, but untested; the capture rate, bandwidth and RTL AGC controls don't work over rtl_tcp yet ([TODO](../TODO.md)) |
| `-i tcp -U 4532` | Anything SDR++ supports: HackRF, Airspy, SDRplay, LimeSDR, PlutoSDR, … | Decode and tune; no spectrum, waterfall, band scan or RTL settings |
| `-i tcp` or `-i pulse` alone | Any radio, any SDR program | Decode; you tune in the SDR program |

**Other SDRs through SDR++.** In SDR++, set the VFO's audio sink to
*Network* (TCP, port 7355, where `-i tcp` connects by default) and enable the
*Rigctl Server* module (port 4532). Then run `dsd-fme -i tcp -U 4532 --web 8080`.
DSD-FME decodes what SDR++ demodulates, and retunes SDR++ when you type a
frequency, pick a bookmark or follow trunked calls (this rigctl path hasn't
been tested from the web page yet). All decoding and its views work:
readouts, slots, activity log, packet data, radios, map, keys, browser audio
and recordings. The spectrum and waterfall (with click-to-tune and
auto-tune), the band scan, the squelch's Auto floor and the RTL settings in the
RF tab need raw IQ samples, which DSD-FME only gets from an RTL-SDR; SDR++ has
its own waterfall and squelch.
DSD-FME's notes say SDR++ copes with weak signals better than the built-in RTL
demodulator, so this setup is worth a try with an RTL-SDR too.

Instead of TCP, any SDR program's audio can come in through PulseAudio
(`-i pulse`; pick the program's output as DSD-FME's source in pavucontrol, see
[Audio Plumbing](Audio_Plumbing.md)).

## What you can do from the browser

**Spectrum / Waterfall** — real-time FFT of the RTL-SDR IQ stream. Configurable
FFT size (1k–16k), frame rate, averaging and dynamic range. Click anywhere on
the spectrum or waterfall to tune the dongle there.

**Tuning** — type a frequency (`851.0125M`, `154987500`, `446M`…) or click the
spectrum. **Auto-tune** snaps the dongle onto the strongest nearby peak and
keeps it centred. **Auto-decode** cycles the decoders that cannot be passively
detected (NXDN48/96, dPMR, M17, ProVoice) until one gets a frame sync and then
holds there — useful once you have fixed onto a signal.

**Decode** — pick the decoder class (AUTO, TDMA trunk, P25 P1/P2, DMR, NXDN,
dPMR, YSF, D-STAR, M17, ProVoice, EDACS), modulation, signal inversion, TDMA
slot on/off, digital/analog gain and the audio filters. Live call info is shown
per slot (TG/SRC, group/private, burst type, errors, ENC alg/key, talker alias,
GPS), plus site/system context (P25 WACN/SYS/NAC/RFSS/SITE, DMR colour code and
branding, NXDN RAN/cipher) and the event history.

**RF** — RTL gain / PPM / sample volume / bandwidth / sample rate, the RMS
squelch, and the RIGCTL SetMod bandwidth. The squelch gates NXDN, dPMR and M17
sync and the analog monitor. **Auto floor**, on by default with an RTL-SDR,
keeps it a margin (+4 dB unless you pick another) over the noise floor measured
across the whole spectrum, so it follows gain and AGC changes and holds on a
channel that never goes quiet. A squelch given on the command line
(`-i rtl:…:sq`) starts with Auto floor off. Off, the level stays where you put
it: type one and press *Set*, or press *Channel floor* to take the tuned
frequency's own quiet level plus the margin.

**Trunk** — enable trunking or the conventional scanner, toggle tuning of
group/private/data/encrypted calls, allow-list mode, hang time, TG hold, return
to control channel, manual channel cycle. Paste a channel-map CSV, a group CSV,
or a scanner frequency list and apply it live.

**Keys** — enter keys for the encryption schemes DSD-FME supports (DMR Basic
Privacy, NXDN/dPMR scrambler, RC4/DES, AES-128/256, Hytera, the TYT/Retevis/
Baofeng/Anytone/Kenwood/Connect-Systems/Vertex/Auctus vendor modes, straight
XOR, Hytera RAS) or import a key-list CSV. Keys are applied to the running
decoder; only decode traffic you are authorised to.

**Auto-find BP key** (key brute-force) — DMR Basic Privacy uses a 1-byte key
number (a 256-entry space), so it can be searched. Enable *Auto-find BP key*
and play some encrypted BP voice: DSD-FME buffers the ciphered AMBE codewords,
then tries every key offline and scores each by the MBE voice-decode error rate
(the Golay-protected part of the codeword only passes FEC when the keystream is
right). It locks the key only on a clear, well-separated winner and shows the
number it found so you can save it. This works **only** for Basic Privacy —
strong ciphers (AES, DES, RC4) and the larger scrambler key spaces are not
brute-forceable and are deliberately excluded.

**Audio** — click **🔊 Audio** to stream the decoded voice (and the analog
monitor, when enabled) to the browser via the Web Audio API. Independent of the
PulseAudio / UDP / WAV outputs, which keep working as usual.

**Files** — per-call WAV recordings (`-7`/the Rec WAV toggle) are listed for
download straight from the browser.

## Security notes

* The server binds to `127.0.0.1` by default, so it is only reachable from the
  same machine unless you pass a different bind address.
* Exposing it on a network (`0.0.0.0`) with no password gives anyone on that
  network full control of the decoder and read access to your recordings. Use
  `--web-pass` (and ideally a reverse proxy with TLS) if you do.
* The browser `Origin` is checked on the WebSocket upgrade to block cross-site
  connections, and downloads are confined to the per-call WAV directory.

## Developing the UI

The UI lives in `web/` (`index.html`, `app.js`). It is embedded into the binary
at build time via `src/dsd_web_assets.c`, which is generated by
`tools/gen_web_assets.py`. After editing `web/`, regenerate it with:

```
make web_assets    # or: python3 tools/gen_web_assets.py web src/dsd_web_assets.c
```

or just run with `--web-root ./web` to serve the files directly off disk while
iterating.
