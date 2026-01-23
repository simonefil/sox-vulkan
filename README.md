# SoX - Sound eXchange

SoX is a command-line audio processing tool that can convert, apply effects, and play audio files in various formats.

This repository includes build scripts for compiling SoX with statically linked codec libraries, producing a single portable executable.

---

## Table of Contents

1. [Dependencies](#dependencies)
2. [Build Instructions](#build-instructions)
3. [Default Options](#default-options)
4. [Available Arguments](#available-arguments)
5. [Verification](#verification)

---

## Dependencies

### Windows

**Required:**
- Visual Studio 2019 or 2022 (with C++ workload)
- CMake 3.14 or later
- Git (optional, for cloning)

CMake is usually included with Visual Studio. If not, install it from https://cmake.org/download/

**Installation:**
```powershell
# Visual Studio Installer > Modify > Workloads > "Desktop development with C++"
# CMake is included with Visual Studio C++ tools
```

### Linux (Debian/Ubuntu)

**Required:**
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git curl
```

**Optional (for audio drivers):**
```bash
# ALSA (recommended)
sudo apt-get install -y libasound2-dev

# PulseAudio
sudo apt-get install -y libpulse-dev

# For AMR support
sudo apt-get install -y libopencore-amrnb-dev libopencore-amrwb-dev
```

### Linux (Fedora/RHEL/CentOS)

**Required:**
```bash
sudo dnf install -y gcc gcc-c++ make cmake git curl
```

**Optional:**
```bash
# ALSA
sudo dnf install -y alsa-lib-devel

# PulseAudio
sudo dnf install -y pulseaudio-libs-devel
```

### Linux (Arch Linux)

**Required:**
```bash
sudo pacman -S base-devel cmake git curl
```

**Optional:**
```bash
sudo pacman -S alsa-lib libpulse
```

### macOS

**Required:**
```bash
# Install Xcode Command Line Tools
xcode-select --install

# Install Homebrew (if not installed)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install dependencies
brew install cmake
```

**Optional:**
```bash
# For autoreconf (needed by some libraries)
brew install autoconf automake libtool
```

### FreeBSD

**Required:**
```bash
sudo pkg install cmake git curl gmake
```

### NetBSD

**Required:**
```bash
sudo pkgin install cmake git curl
```

### OpenBSD

**Required:**
```bash
doas pkg_add cmake git curl
```

---

## Build Instructions

### Quick Start

**Windows (PowerShell):**
```powershell
cd C:\path\to\sox-repo
.\build_static_libs.ps1
```

**Linux/macOS/BSD (Bash):**
```bash
cd /path/to/sox-repo
chmod +x build_static_libs.sh
./build_static_libs.sh
```

### Step-by-Step Process

1. **Clone or download the repository:**
   ```bash
   git clone <repository-url>
   cd sox-repo
   ```

2. **Run the build script:**

   The script will automatically:
   - Download all required codec libraries
   - Compile each library as a static library
   - Configure and build SoX with all codecs
   - Copy the final binary to the `output/` directory
   - Clean up temporary build files

3. **Find the output:**
   - Windows: `output\sox.exe`
   - Linux/macOS/BSD: `output/sox`

### Build with Custom Options

**Windows:**
```powershell
# Build without MP2 and ID3 tag support
.\build_static_libs.ps1 -NoMp2 -NoId3tag

# Build with 4 parallel jobs
.\build_static_libs.ps1 -Jobs 4

# Clean build directories
.\build_static_libs.ps1 -Clean
```

**Linux/macOS/BSD:**
```bash
# Build without AMR and MP2 support
./build_static_libs.sh --no-amr --no-mp2

# Build with 4 parallel jobs
./build_static_libs.sh --jobs 4

# Add PulseAudio support (Linux)
./build_static_libs.sh --with-pulseaudio

# Clean build directories
./build_static_libs.sh --clean
```

---

## Default Options

### Codecs (Enabled by Default)

| Codec | Description | Libraries |
|-------|-------------|-----------|
| OGG/Vorbis | Ogg container with Vorbis audio | libogg, libvorbis |
| FLAC | Free Lossless Audio Codec | libFLAC |
| Opus | Modern low-latency codec (decode and encode) | libopus, opusfile, libopusenc |
| AC-3 | ATSC A/52 elementary stream (decode and encode) | FFmpeg libavcodec, libavutil |
| E-AC-3 | Dolby Digital Plus elementary stream (decode and encode) | FFmpeg libavcodec, libavutil |
| AAC/ADTS | AAC-LC encode; AAC-LC, HE-AAC and HE-AACv2 decode | FFmpeg libavcodec, libavutil |
| AAC/LOAS-LATM | AAC-LC encode; AAC-LC, HE-AAC and HE-AACv2 decode | FFmpeg libavcodec, libavutil |
| xHE-AAC/USAC | LOAS/LATM elementary stream (decode only) | FFmpeg libavcodec, libavutil |
| ALAC/M4A | Apple Lossless in an M4A container (decode and encode) | FFmpeg libavcodec, libavformat, libavutil |
| MP3 | MPEG Audio Layer III | libmad (decoder), LAME (encoder) |
| MP2 | MPEG Audio Layer II | TwoLAME (encoder) |
| WavPack | Lossless audio compression | libwavpack |
| libsndfile | Multi-format audio I/O | libsndfile |
| ID3 Tag | MP3 metadata support | libid3tag |
| PNG | Spectrogram output | libpng, zlib |

### Additional Codecs (Linux/macOS/BSD Only)

| Codec | Description |
|-------|-------------|
| AMR | Adaptive Multi-Rate audio |
| libmagic | File type detection |

### Audio Drivers by Platform

| Platform | Default Drivers |
|----------|-----------------|
| Windows | WaveAudio |
| Linux | ALSA + libao |
| macOS | CoreAudio + libao |
| FreeBSD | OSS + libao |
| NetBSD | OSS + libao |
| OpenBSD | OSS + libao |

---

## Available Arguments

### Windows (PowerShell)

| Argument | Description |
|----------|-------------|
| `-Jobs N` | Number of parallel build jobs (default: auto) |
| `-Clean` | Remove all build and output directories |
| `-Help` | Show help message |

**Codec Exclusion:**

| Argument | Description |
|----------|-------------|
| `-NoOgg` | Exclude OGG/Vorbis support |
| `-NoFlac` | Exclude FLAC support |
| `-NoOpus` | Exclude Opus support |
| `-NoMp3` | Exclude MP3 support (libmad + LAME) |
| `-NoMp2` | Exclude MP2 support (TwoLAME) |
| `-NoWavpack` | Exclude WavPack support |
| `-NoSndfile` | Exclude libsndfile support |
| `-NoId3tag` | Exclude ID3 tag support |
| `-NoPng` | Exclude PNG spectrogram support |

**Audio Drivers:**

| Argument | Description |
|----------|-------------|
| `-NoWaveaudio` | Exclude WaveAudio driver |

### Linux/macOS/BSD (Bash)

| Argument | Description |
|----------|-------------|
| `--jobs N`, `-j N` | Number of parallel build jobs (default: auto) |
| `--clean` | Remove all build and output directories |
| `--help`, `-h` | Show help message |

**Codec Exclusion:**

| Argument | Description |
|----------|-------------|
| `--no-ogg` | Exclude OGG/Vorbis support |
| `--no-flac` | Exclude FLAC support |
| `--no-opus` | Exclude Opus support |
| `--no-mp3` | Exclude MP3 support (libmad + LAME) |
| `--no-mp2` | Exclude MP2 support (TwoLAME) |
| `--no-wavpack` | Exclude WavPack support |
| `--no-sndfile` | Exclude libsndfile support |
| `--no-amr` | Exclude AMR support |
| `--no-id3tag` | Exclude ID3 tag support |
| `--no-png` | Exclude PNG spectrogram support |
| `--no-magic` | Exclude libmagic support |

**Audio Driver Exclusion:**

| Argument | Description |
|----------|-------------|
| `--no-alsa` | Exclude ALSA driver |
| `--no-ao` | Exclude libao driver |
| `--no-coreaudio` | Exclude CoreAudio driver (macOS) |
| `--no-oss` | Exclude OSS driver |

**Audio Driver Inclusion:**

| Argument | Description |
|----------|-------------|
| `--with-alsa` | Include ALSA driver |
| `--with-coreaudio` | Include CoreAudio driver |
| `--with-pulseaudio` | Include PulseAudio driver |
| `--with-oss` | Include OSS driver |

---

## Verification

After the build completes, verify the installation:

### 1. Check Version

```bash
# Windows
.\output\sox.exe --version

# Linux/macOS/BSD
./output/sox --version
```

Expected output:
```
sox:      SoX v14.4.3
```

### 2. Check Supported Formats

```bash
# Windows
.\output\sox.exe --help-format all

# Linux/macOS/BSD
./output/sox --help-format all
```

This will list all supported audio formats. Look for:
- `flac` - FLAC support
- `mp3` - MP3 support
- `ogg` - OGG/Vorbis support
- `opus` - Opus support
- `ac3` - AC-3 elementary stream support
- `eac3` - E-AC-3 elementary stream support
- `aac`, `adts` - AAC with ADTS framing
- `latm`, `loas` - AAC with LOAS/LATM framing
- `usac` - xHE-AAC/USAC with LOAS/LATM framing
- `m4a` - Apple Lossless in an M4A container
- `wav` - WAV support
- `wavpack` - WavPack support

### 3. Check Audio Devices

```bash
# Windows
.\output\sox.exe --help-device all

# Linux/macOS/BSD
./output/sox --help-device all
```

### 4. Test Conversion

```bash
# Convert a WAV file to MP3
./output/sox input.wav output.mp3

# Convert a WAV file to FLAC
./output/sox input.wav output.flac

# Convert a WAV file to Opus at 128 kbit/s
./output/sox input.wav -C 128 output.opus

# Encode standard 5.1 surround at 384 kbit/s
./output/sox input-5.1.wav -C 384 output-5.1.opus

# Encode standard 5.1 AC-3 at 448 kbit/s
./output/sox input-5.1.wav -C 448 output-5.1.ac3

# Encode standard 5.1 E-AC-3 at 768 kbit/s
./output/sox input-5.1.wav -C 768 output-5.1.eac3

# Encode AAC-LC with LOAS/LATM framing at 128 kbit/s
./output/sox input.wav -C 128 output.latm

# Encode 24-bit Apple Lossless in an M4A container
./output/sox input-24bit.wav -C 2 output.m4a

# Generate a test tone (5 seconds, 440Hz sine wave)
./output/sox -n test.wav synth 5 sine 440
```

Opus encoding and decoding support the standard layouts from 1 to 8 channels.
Mono and stereo use mapping family 0; layouts with 3 to 8 channels use mapping
family 1. SoX automatically converts between its canonical WAVE channel order
and the Vorbis channel order required by multichannel Opus. Since SoX tracks
the channel count but not an explicit channel layout, non-standard discrete
layouts, ambisonics, and streams with more than 8 channels are not supported.

AAC encoding supports AAC-LC with either ADTS (`aac`, `adts`) or LOAS/LATM
(`latm`, `loas`) framing in the canonical SoX layouts from mono through 7.1.
Both handlers decode AAC-LC, HE-AAC and HE-AACv2. Quad, 6.1 and 7.1 streams
use an MPEG-4 Program Config Element (PCE), avoiding the ambiguous ADTS 7.1
channel configuration and preserving SoX's canonical channel order:
`FL FR BL BR` for quad, `FL FR FC LFE BC SL SR` for 6.1, and
`FL FR FC LFE BL BR SL SR` for 7.1. Non-standard discrete layouts are not
supported. SoX writes a `StreamMuxConfig` every 20 frames in LOAS/LATM
output so decoding can recover after joining a stream mid-file.

xHE-AAC/USAC decoding accepts LOAS/LATM elementary streams through the
`usac`, `xheaac`, and `xhe-aac` format names. SoX parses the LOAS/LATM
transport and passes raw USAC access units plus their `AudioSpecificConfig`
to FFmpeg 8 or later. The supported LATM subset has one synchronous program
and layer, no additional subframes or `otherData`, `audioMuxVersion` 1, and
`frameLengthType` 0. Encoding and container formats are not supported.
xHE-AAC loudness and dynamic-range metadata, if present, are not applied and
produces a warning.

ALAC support reads and writes Apple Lossless exclusively in M4A files. It
uses FFmpeg's `libavformat`, `libavcodec`, and `libavutil` libraries directly;
it does not invoke the `ffmpeg` executable. Encoding accepts 16-bit and 24-bit
PCM, while decoding accepts the ALAC 16-, 20-, 24-, and 32-bit depths. For
ALAC, `-C` selects the lossless compression effort from 0 through 2 and
defaults to 2; it is not a bitrate.

ALAC supports its official layouts from one through eight channels. SoX does
not remix channels automatically. For the layouts that differ from SoX's
canonical meaning for the same channel count, encoding and decoding produce a
warning and expose the channels in the following order:

- 4-channel MPEG 4.0 B: `FL FR FC BC`;
- 7-channel Apple AAC 6.1: `FL FR FC LFE BL BR BC`;
- 8-channel MPEG 7.1 B: `FL FR FC LFE BL BR FLC FRC`.

This allows an input to be prepared explicitly with effects such as `remix`
before ALAC encoding. In particular, MPEG 7.1 B is a front-wide layout, not
the canonical SoX 7.1 side-and-back layout. M4A input may be read from a pipe;
M4A output must be a seekable file.

FFmpeg-backed output formats accept `--channel-layout LAYOUT` immediately
before the output file. The option labels the existing interleaved channels;
it never remixes them. The layout name must exist, its channel count must
match the signal produced by the SoX effects chain, and the selected encoder
must support it. Run `sox --help-format FORMAT` to see every accepted layout
and its exact channel order for that format. For example:

```bash
# The eight input channels are already prepared in MPEG 7.1 B order.
./output/sox prepared-8ch.wav -b 24 \
  --channel-layout '7.1(wide)' output.m4a

# Preserve an explicitly prepared AAC 6.1(back) channel order in the PCE.
./output/sox prepared-7ch.wav -C 448 \
  --channel-layout '6.1(back)' output.aac
```

Without `--channel-layout`, SoX keeps its canonical layout for codecs that
offer several layouts with the same channel count. A format with one official
layout per channel count, such as ALAC, keeps that required layout and warns
when it differs from the canonical SoX meaning. An explicit selection confirms
that the caller prepared the channels intentionally, so the corresponding
encoding warning is omitted. Decoding never remixes: a noncanonical layout
produces a warning, and an unspecified FFmpeg decoder layout warns that only
the decoder sample order can be preserved.

AC-3 and E-AC-3 support use only FFmpeg's `libavcodec` and `libavutil`
libraries; they do not invoke the `ffmpeg` executable or use `libavformat`.
Both handlers read and write elementary streams at 32, 44.1, or 48 kHz.
AC-3 supports canonical SoX layouts from mono through 5.1. E-AC-3 decoding
supports canonical layouts through 7.1, while the FFmpeg encoder currently
supports encoding through 5.1. SoX distinguishes AC-3 from E-AC-3 by parsing
the shared syncframe header and its `bsid` field.

When an E-AC-3 input contains Dolby Atmos JOC/OAMD spatial metadata, SoX
warns that the metadata will be ignored and decodes only the channel-based
audio presentation. Container formats and preservation or rendering of
advanced Dolby metadata are outside these handlers.

Additional `libavcodec` AVOptions can be passed to an FFmpeg-backed input or
output with `--ffmpeg-opts`. Use `key=value:key=value` syntax and place the
option immediately before the file it applies to:

```bash
# Encode E-AC-3 with additional encoder metadata/options
./output/sox input.wav -C 640 \
  --ffmpeg-opts 'dialnorm=-27:dmix_mode=loro:stereo_rematrixing=0' output.eac3

# Configure the E-AC-3 decoder
./output/sox --ffmpeg-opts 'drc_scale=0.5' input.eac3 output.wav
```

Unknown options, options unsupported by the selected encoder or decoder, and
use with a non-FFmpeg format are errors. Options controlling bitrate, sample
rate, channels/layout, sample format, downmixing, or time base remain owned by
SoX and cannot be overridden through `--ffmpeg-opts`; use `-C`, `-r`, `-c`,
`--channel-layout`, and SoX effects such as `remix` instead. Available
passthrough options depend on the installed FFmpeg version.

### 5. Verify Static Linking (Linux/macOS)

```bash
# Linux: Check for dynamic dependencies
ldd ./output/sox

# macOS: Check for dynamic dependencies
otool -L ./output/sox
```

On Linux, a statically compiled binary should show only system libraries like:
- `linux-vdso.so`
- `libc.so`
- `libm.so`
- `libpthread.so`
- `libdl.so`
- `libasound.so` (if ALSA enabled)

On macOS, you should see only system frameworks:
- `CoreAudio.framework`
- `libSystem.B.dylib`

### 6. Test Audio Playback

```bash
# Generate and play a test tone
./output/sox -n -d synth 3 sine 440

# Play an audio file
./output/sox input.wav -d
```

---

## Troubleshooting

### Common Issues

**CMake not found (Windows):**
- Ensure Visual Studio is installed with C++ workload
- Or install CMake separately and add to PATH

**Build fails with missing headers (Linux):**
- Install development packages: `sudo apt-get install libasound2-dev`

**Permission denied (Linux/macOS):**
- Make script executable: `chmod +x build_static_libs.sh`

**Homebrew not found (macOS):**
- Install Homebrew first (see macOS dependencies section)

**autoreconf not found (macOS):**
- Install autotools: `brew install autoconf automake libtool`

### Clean Build

If you encounter issues, try a clean build:

```bash
# Windows
.\build_static_libs.ps1 -Clean
.\build_static_libs.ps1

# Linux/macOS/BSD
./build_static_libs.sh --clean
./build_static_libs.sh
```

---

## Library Versions

The build scripts download and compile the following library versions:

| Library | Version |
|---------|---------|
| zlib | 1.3.1 |
| libpng | 1.6.43 |
| libogg | 1.3.5 |
| libvorbis | 1.3.7 |
| FLAC | 1.4.3 |
| opus | 1.5.2 |
| opusfile | 0.12 |
| libopusenc | 0.3 |
| libmad | 0.15.1b |
| LAME | 3.100 |
| TwoLAME | 0.4.0 |
| libid3tag | 0.15.1b |
| WavPack | 5.7.0 |
| libsndfile | 1.2.2 |
| opencore-amr | 0.1.6 |
| file/libmagic | 5.45 |
| libtool/libltdl | 2.4.7 |
| libao | 1.2.2 |

---

## License

SoX is distributed under the GNU General Public License (GPL) and GNU Lesser General Public License (LGPL). See `LICENSE.GPL` and `LICENSE.LGPL` for details.
