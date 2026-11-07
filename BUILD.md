# Building DSD-FME web

This fork builds the same way as DSD-FME: one CMake project, a C and C++ compiler and a handful of
libraries. The web interface needs nothing extra. Its server and the page, fonts included, are
compiled into the `dsd-fme` binary.

It is developed and tested on Linux (Manjaro, x86-64). Upstream DSD-FME also builds on macOS with
Homebrew and on Windows under Cygwin (see [examples/Install_Notes.md](examples/Install_Notes.md)),
but the web interface hasn't been tried there.

The `download-and-install*.sh` scripts and examples/Install_Notes.md come from upstream: they clone
[lwvmobile/dsd-fme](https://github.com/lwvmobile/dsd-fme), which has no web interface. Use the steps
below for this fork.

## Dependencies

| Library | Needed | Used for | Without it |
|---|---|---|---|
| [mbelib](https://github.com/lwvmobile/mbelib), lwvmobile's fork, branch `ambe_tones` | yes | AMBE and IMBE voice (DMR, P25, NXDN, dPMR, D-STAR, YSF…) | built from source, see below |
| libsndfile | yes | audio files: WAV recordings, file and TCP input | |
| ncursesw (ncurses with wide characters) | yes | the `-N` terminal interface | |
| PulseAudio client libraries (libpulse, libpulse-simple) | yes | audio input and output; PipeWire's PulseAudio server works too | |
| librtlsdr | optional | `-i rtl` and `-i rtltcp`, and every RF feature of the page: spectrum and waterfall, click to tune, band scan, auto-tune, the squelch's Auto floor, the RF tab | no RTL input: `-i rtl` falls back to PulseAudio input, and the page decodes but has no spectrum |
| codec2 | optional | M17 voice | M17 still syncs and shows its data, but no voice |
| help2man | optional | the man page, `dsd-fme.1` | no man page |
| Python 3 | optional | the `web_assets` target, only needed after editing `web/` | |

To build you also need CMake 3.15 or newer, GCC or Clang with C++, make, pkg-config and git.

### Packages

Arch, Manjaro:

```
sudo pacman -S --needed base-devel cmake git libsndfile libpulse ncurses rtl-sdr codec2
```

Debian, Ubuntu, Raspberry Pi OS, Mint:

```
sudo apt install build-essential cmake git pkg-config libsndfile1-dev libpulse-dev libncurses-dev \
  librtlsdr-dev rtl-sdr libcodec2-dev
```

Fedora:

```
sudo dnf install gcc gcc-c++ make cmake git pkgconf-pkg-config libsndfile-devel pulseaudio-libs-devel \
  ncurses-devel rtl-sdr rtl-sdr-devel codec2-devel
```

Add `help2man` and Python 3 (`python` on Arch, `python3` elsewhere) if you want them. Only the
Arch/Manjaro line has been used with this fork. The Debian and Fedora lines are upstream's lists without the
packages the current build doesn't use (fftw, lapack, libusb, socat). The `rtl-sdr` package brings
`rtl_test`, `rtl_sdr` and `rtl_tcp`. On Arch it also installs the udev rules that let a normal user
open the dongle and the blacklist that keeps the DVB-T TV driver off it; other distributions'
packages do much the same.

### mbelib

Read the [patent notice](https://github.com/lwvmobile/mbelib#readme) first. Then:

```
git clone https://github.com/lwvmobile/mbelib
cd mbelib
git checkout ambe_tones
cmake -B build
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo ldconfig
cd ..
```

This installs `libmbe` and `mbelib.h` under /usr/local.

## Build

```
git clone https://github.com/altbert/dsd-fme-web
cd dsd-fme-web
cmake -B build
cmake --build build -j"$(nproc)"
```

The binary is `build/dsd-fme`. While `cmake -B build` runs, look for `Found RTLSDR` and
`Found CODEC2` in its output. If either line is missing, that feature is left out (see the table):
install the library's development package and run `cmake -B build` again. At startup dsd-fme
prints its `Build Version`, and `CODEC2 Support Enabled` if it has codec2.

Try it:

```
./build/dsd-fme -i rtl --web 8080      # then open http://localhost:8080/
```

Add `-o null` to keep the sound off the speakers. Usage: [examples/Web_Interface.md](examples/Web_Interface.md)
for the page, [examples/Example_Usage.md](examples/Example_Usage.md) for the command line.

To install it in /usr/local/bin, with the man page if help2man was found:

```
sudo cmake --install build
```

`sudo cmake --build build --target uninstall` removes what that installed.

### Updating

```
git pull
cmake -B build
cmake --build build -j"$(nproc)"
```

The build finds the source files with a wildcard, so CMake only picks up new ones when it runs
again. Upstream merges add files now and then, so run `cmake -B build` after every pull.

## Build types and options

| Configure with | Flags | |
|---|---|---|
| `cmake -B build` | `-O2 -g` | the default: optimized, with debug info and the asserts kept |
| `-DCMAKE_BUILD_TYPE=Debug` | `-O0 -g` | for a debugger |
| `-DCMAKE_BUILD_TYPE=Release` | `-O3 -DNDEBUG` | CMake's release flags, asserts off |
| `-DCMAKE_C_FLAGS=... -DCMAKE_CXX_FLAGS=...` with an `-O` in them | yours | used as given, e.g. `"-O1 -g -fsanitize=address,undefined"` for a sanitizer build |

Upstream's feature switches, as `-D<name>=ON` or `OFF`:

| Option | Default | |
|---|---|---|
| `COLORS` | ON | colors in the `-N` terminal |
| `COLORSLOGS` | ON | colors in the console output |
| `PVC` | OFF | ProVoice conventional frame sync |
| `LZ` | OFF | NXDN tweaks requested by LimaZulu |
| `ASCII` | ON | NXDN aliases in ASCII only |
| `BIG5`, `SJIS`, `UTF16` | OFF | NXDN aliases in that character set as well; set `-DASCII=OFF` with it |
| `TYPEC` | ON | NXDN Type-C and conventional |
| `TYPED` | ON | NXDN Type-D (IDAS) |
| `DCR` | ON | Japanese DCR, an NXDN variant |
| `NOS` | OFF | the old NXDN sync, both polarities at once |
| `SID` | OFF | P25 Phase 1 LSD / Soft ID, experimental |

CMake keeps these in the build directory's cache: an option stays as set until you set it again,
or until you start over with a new build directory.

## The web page

The page in `web/` (index.html, app.js, fonts) gets into the binary through `src/dsd_web_assets.c`,
a generated file that is committed, so a build doesn't need Python. After editing `web/`:

```
cmake --build build --target web_assets    # exists if Python 3 was found when cmake ran
cmake --build build
```

While you work on the page, `--web-root ./web` serves the files from disk, so a reload shows each
edit without rebuilding. The 7-segment font comes from `tools/gen_segment_font.py`, which needs the
Python packages fontTools and brotli. The README screenshots come from `tools/screenshots`, which
needs Node 22+, Chromium and ImageMagick (see its README).

## Troubleshooting

- **`error while loading shared libraries: libmbe.so.1`**: run `sudo ldconfig` after installing
  mbelib. If the loader doesn't search /usr/local/lib on your system, add it:
  `echo /usr/local/lib | sudo tee /etc/ld.so.conf.d/local.conf && sudo ldconfig`. The binary in
  `build/` finds the library either way, because CMake records its directory in that binary.
- **`Could NOT find MBE`** (or LibSndFile, PulseAudio, Curses): the library or its development package
  is missing. If you installed it under another prefix, point CMake at it with
  `cmake -B build -DCMAKE_PREFIX_PATH=/that/prefix`.
- **`RTL Support not enabled/compiled`** at startup, and no spectrum on the page: librtlsdr wasn't
  found when cmake ran (there was no `Found RTLSDR`). Install it and configure again.
- **`usb_claim_interface error -6`** from dsd-fme or `rtl_test`: another program has the dongle
  (another dsd-fme, rtl_tcp, SDR++…). If nothing does, the DVB-T TV driver probably took it: the
  `rtl-sdr` packages blacklist that driver, so replug the dongle, or unload it with
  `sudo modprobe -r dvb_usb_rtl28xxu`. **`usb_open error -3`** is a permissions problem: the udev
  rules come with the `rtl-sdr` package, so install it and replug.
- **`--web` does nothing or isn't recognized**: an older DSD-FME is running, often an upstream one
  that `sudo make install` left in /usr/local/bin. `which -a dsd-fme` lists the ones on your PATH.
  Run `./build/dsd-fme`, or install this build over the old one.
- **`Build Version: AW -128-NOTFOUND`**: `git describe`, which makes the version, found no tag to
  count from, as in a shallow clone (`--depth`) or a copy without the tags. Nothing is wrong with the
  build. To get the usual `AW <tag>-<commits>-g<hash>`, fetch the full history and the tags, and
  configure again: `git fetch --unshallow --tags` (just `git fetch --tags` if the clone isn't
  shallow), then `cmake -B build`. **`AW HEAD-HASH-NOTFOUND`** means the source came from an archive,
  not a git checkout.
