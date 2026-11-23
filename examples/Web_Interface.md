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
frequency, pick or scan bookmarks, or follow trunked calls (this rigctl path
hasn't been tested from the web page yet). All decoding and its views work:
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
FFT size (1k–16k), frame rate, averaging and dynamic range. Every frame
averages all the IQ since the one before (FFTs overlapping by half), so a short
burst between two frames still shows. *Average* is the least number of FFTs a
frame averages; it only matters at high frame rates or with large FFTs, when a
frame holds fewer. The RTL hands over its samples a USB transfer at a time, and
the spectrum runs one transfer behind the receiver to keep its frames evenly
spaced: about 0.13 s at the default sample rate, 0.5 s at 0.25 MSPS. The
shaded band around the tuned frequency is the channel the demodulator takes
in (Bandwidth in the RF tab). Click anywhere on the spectrum or waterfall to
tune the dongle there.

**Tuning** — type a frequency (`851.0125M`, `154987500`, `446M`…) or click the
spectrum. Or tune on the Decode card's frequency readout: click a digit, then
type it or step it with the up and down arrow keys, and the digits after it go
to 0; left and right pick another digit, `.` the first one after the point.
Typing moves on to the next digit, so a whole frequency can be typed from the
left. It tunes once you stop (0.4 s after an arrow, 1.5 s after a typed digit);
Enter tunes at once, Escape undoes the change. **Auto-tune** snaps the dongle onto the strongest nearby peak and
keeps it centred. **Auto-decode** listens on the tuned channel for the frame
syncs of every protocol DSD-FME decodes at once (P25 Phase 1 and 2, DMR,
X2-TDMA, YSF, D-STAR, NXDN48/96, dPMR, M17, ProVoice and EDACS) and switches
the decoder straight to the one it hears, usually within a second, then holds
there — useful once you have fixed onto a signal. Meanwhile it waits on AUTO,
which takes P25 Phase 1, DMR, YSF, D-STAR and X2-TDMA by itself. A sync only
counts once it holds for 2 s at a real frame rate (for M17, with at least 15
frames in 2 s and a third of them passing the LICH check): the short sync words
of NXDN, dPMR and M17 also match by chance on noise or on any other signal over
the squelch, so the listener only takes them four frames in a row, with every
symbol at full deviation, and only while the squelch is open. A decoder whose
sync doesn't hold in 4 s of signal goes back to AUTO and isn't tried again until
you retune; after 4 s of signal with no sync of any kind the channel counts as
analog FM (with Analog on). NXDN and dPMR decode in the polarity the Inverted
toggle sets, M17 only upright: a sync heard the other way round is reported in
the console. The band scan listens the same way on every carrier it finds.

**Bookmarks** — kept on the DSD-FME host (`~/.config/dsd-fme/web_bookmarks.json`,
or the file `--web-bookmarks` names), so every browser sees the same list. A
bookmark has a name, a group, and optionally the decoder and modulation that
clicking it applies. Under its name each row shows when a call was last heard
on it, from the Activity log. The key at the start of a row selects the
bookmark for the **scan**: *Scan* steps through the selected bookmarks by
frequency, from the tuned one up, each with its own decoder (one that keeps the current decoder
gets the one that ran when the scan started), and stops on each active one.
Active is what that decoder plays: a digital sync or, for a decoder that plays
analog FM (Analog, AUTO or Auto-detect with Analog on, or Monitor on), the
channel open over the squelch. An open channel gives a digital decoder the hang
time, at least 1.5 s, to find its sync, or the scan moves on. It keeps
listening *Hang* seconds after the bookmark goes quiet (2 s unless you set
another), and stays as long as the bookmark is active. A bookmark left while it
is still on (*Skip*, or a signal its decoder doesn't take) is passed over until
the scan finds it quiet;
*Hold* keeps the scan on the bookmark until you release it. With an RTL-SDR the
spectrum takes part too. A bookmark only counts as open when its own 6 kHz
stand out there as well, since the RMS squelch also opens on strong carriers
elsewhere in the capture. And the selected bookmarks in the clean part of the
spectrum (from 15 % of the span under the tuned frequency to 45 % over it) are
judged on it without a retune: the quiet ones are passed over, and when all of
them are quiet the receiver stays where it is, so channels within a few hundred
kHz of each other are watched without retuning. A retune and its check take
about 0.75 s at the default sample rate, longer at 0.25 MSPS. With SDR++ over
rigctl, set its squelch: the scan takes audio that isn't silent for an open
channel. Tuning by hand, clicking a bookmark, choosing a decoder, Auto-decode,
Auto-tune, the band scan and trunking stop the scan. *Select all* selects
every bookmark the search and group filter show, *Unselect all* unselects them
all; export and import keep the selection. Hang and the band scan's settings
are kept on the host too, in `web_settings.json` next to the bookmark file, so
every browser shows the same ones.

**Analog FM** — while no digital sync is held, the audio the analog monitor
would play is checked for a CTCSS tone, a DCS code and the DTMF ANI some radios
send at key-up (their ID as digits). The Decode readout then shows them in its
*Tone* row (e.g. `DCS 023N, ANI 1234`), and each analog transmission becomes a
call in the Activity log with its tone and ANI (the ANI also lists the radio
under Radios). A tone or code is a sure sign of analog: Auto-decode then calls
the channel analog at once, and the band scan bookmarks the carrier as "Analog
FM CTCSS 100.0" with the Analog decoder. Carriers with no tone and no digital
sync in 3 s of signal, which could as well be paging or a digital mode DSD-FME
doesn't decode, are skipped. A toned carrier with one at least 12 dB stronger
within 40 kHz is left for a later pass, since what is heard on it may be that
neighbour leaking in. An untoned
transmission gets an Activity item only when the monitor plays it for 2 s and
it sounds like speech, so noise over a low squelch doesn't fill the log.
The DTMF high group goes up to 1633 Hz: keep the RTL bandwidth at 6 kHz or more.

**Decode** — pick the decoder class (AUTO, TDMA trunk, P25 P1/P2, DMR, NXDN,
dPMR, YSF, D-STAR, M17, ProVoice, EDACS), modulation, signal inversion, TDMA
slot on/off, digital/analog gain and the audio filters. Live call info is shown
per slot (TG/SRC, group/private, burst type, errors, ENC alg/key, talker alias,
GPS), plus site/system context (P25 WACN/SYS/NAC/RFSS/SITE, DMR colour code and
branding, NXDN RAN/cipher) and the event history.

**RF** — RTL gain / PPM / sample volume / bandwidth / sample rate, the RMS
squelch, and the RIGCTL SetMod bandwidth. The squelch gates NXDN, dPMR and M17
sync, the analog monitor, the band scan and the bookmark scan. **Auto floor**, on by default with an RTL-SDR,
keeps it a margin (+4 dB unless you pick another) over the noise floor measured
across the whole spectrum, so it follows gain and AGC changes and holds on a
channel that never goes quiet. A squelch given on the command line
(`-i rtl:…:sq`) starts with Auto floor off. Off, the level stays where you put
it: step it with the box's arrows (applied as you go), type one and press *Set*
or Enter, or press *Channel floor* to take the tuned frequency's own quiet level
plus the margin. The margin goes from 0 dB (right at the floor) to +20 dB.

**Trunk** — enable trunking or the conventional scanner, toggle tuning of
group/private/data/encrypted calls, allow-list mode, hang time, TG hold, return
to control channel, manual channel cycle. Paste a channel-map CSV, a group CSV,
or a scanner frequency list and apply it live.

**Keys** — enter keys for the encryption schemes DSD-FME supports (DMR Basic
Privacy, NXDN/dPMR scrambler, RC4/DES, AES-128/256, Hytera, the TYT/Retevis/
Baofeng/Anytone/Kenwood/Connect-Systems/Vertex/Auctus vendor modes, straight
XOR, MOTOTRBO RAS, Hytera over-the-air signalling encryption) or import a key-list
CSV. In a hex key list, Hytera Basic and Enhanced encrypted data packets find
their key by the key id in their data header, ahead of a key entered by hand.
Keys are applied to the running decoder; only decode traffic you are
authorised to. The DMR box of the system picture shows *Access: Restricted
(RAS)* once the repeater is seen using Restricted Access, and the Hytera OTA
setting while it is on.
The Hytera OTA check (`0:0`, no key) doesn't detect OTA encryption, which
would look the same as a bad signal: it makes the decoder treat the system as
OTA-encrypted. CSBKs, PI headers and data headers are dropped, calls show under
pseudo TG 8686868 with a stand-in radio ID that stays the same for each radio,
and late entry is searched for the IV of a DMRA RC4 or AES-256 call. Turn it on
only for systems you know are OTA-encrypted, and not on control channels you
trunk from.

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

**Call recordings** — with *Record calls* on (in the Activity bar and the REC
tab; on by default), every call is recorded as the browser would play it, after
the decoder's gain, high-pass filter and mutes, whether a browser listens or
not. Open a call in the Activity log for a small player; *Keep* copies its
recording into the WAV folder. Recordings sit in a temporary folder
(`$TMPDIR/dsd-fme-web-rec-*`, else `/tmp`) that is removed when DSD-FME exits;
past 256 MB the oldest go first. A call muted by encryption without a key, the
block list or a talkgroup hold records nothing. With `-o null`, P25 Phase 1,
NXDN, dPMR, ProVoice and some YSF modes send the browser no voice, so their
calls record nothing either.

**REC** — the recording toggles: *Record calls*, and *Per-call WAV*, DSD-FME's
own recorder (`-P`), which writes one file per call into the WAV folder.
*Open recordings* lists the temporary recordings, the kept ones and the
per-call WAV files with their date, duration, frequency, protocol and timeslot,
talkgroup and radio, talker alias, GPS, encryption, system ids, file name and
size. Each can be played, downloaded or deleted (from the WAV folder too), and
the list filtered and sorted by date, duration or frequency. A kept recording
has its call's details in a `.json` file next to it. New per-call WAV files get
one as well, matched to their call in the Activity log; those that don't match
show what their file name holds.

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
