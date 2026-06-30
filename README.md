# SoX - Sound eXchange

SoX is a command-line audio processing tool that can convert, apply effects, and play audio files in various formats.

This repository includes build scripts for compiling SoX with statically
linked codec libraries. Windows Vulkan builds additionally distribute the
official dynamic `glslang.dll`.

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
- Visual Studio 2019 or later (with C++ workload)
- PowerShell 7 (`pwsh`) for the FFmpeg build; the script refuses to start on
  Windows PowerShell 5.1 unless `-NoFfmpeg` is given
- CMake 3.15 or later
- Vulkan SDK with `glslc`, glslang C headers, and `glslang.dll` (required by default for the Vulkan FIR and DSD backends; use `-NoVulkan` to disable them)
- MSYS2 with GNU Make, diffutils, and pkgconf (required for the static FFmpeg build)
- NASM (available in the MSYS2 environment)
- Git (optional, for cloning)

The build script must run inside a Visual Studio developer environment, because
FFmpeg is configured with `--toolchain=msvc` and built under MSYS2: `cl.exe` has
to be on `PATH` for MSYS2 to inherit it, and `dumpbin.exe` is what verifies the
finished executable. The script stops with an error if either is missing.
Neither a plain PowerShell prompt nor `pwsh` on its own provides them.

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
sudo apt-get install -y \
  build-essential cmake git curl pkg-config \
  autoconf automake libtool xz-utils bzip2 unzip
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
sudo dnf install -y \
  gcc gcc-c++ make cmake git curl pkgconf-pkg-config \
  autoconf automake libtool xz bzip2 unzip
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
sudo pacman -S base-devel cmake git curl xz bzip2 unzip
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
brew install cmake pkgconf
```

**Required by the full static build:**
```bash
brew install autoconf automake libtool libomp
```

`libomp` is not optional: the script builds with `SOX_REQUIRE_OPENMP=ON` and
links Homebrew's `libomp.a`, so configuration fails without it.

**Required by `--vulkan`:**
```bash
brew install vulkan-loader vulkan-headers glslang shaderc
```

CMake needs all four: the loader and headers for `find_package(Vulkan)`,
`glslc` from `shaderc` to compile this tree's shaders at build time, and
glslang's C headers and library for VkFFT's run-time shader compilation. The
Vulkan backends are available on macOS and Windows only; CMake stops with an
error if `WITH_VULKAN` is on anywhere else.

**Optional:**
```bash
brew install nasm
```

`nasm` enables LAME's x86 assembly. Apple Silicon does not use it — LAME's
configure reports `checking for nasm... no` and builds without it — so install
it only on an Intel Mac.

### FreeBSD

**Required:**
```bash
sudo pkg install bash cmake git curl gmake autoconf automake libtool pkgconf nasm
```

---

## Build Instructions

### Quick Start

**Windows (Visual Studio Developer PowerShell):**
```powershell
cd C:\path\to\sox-repo
pwsh -File .\build_static_libs.ps1
```

Open "Developer PowerShell for VS" from the Start menu, or run `VsDevCmd.bat`
from a plain prompt, so that `cl.exe` and `dumpbin.exe` are on `PATH`. Call
`pwsh` explicitly as above: the default FFmpeg build needs PowerShell 7, and the
developer shell may well be Windows PowerShell 5.1. With `-NoFfmpeg` the script
runs on 5.1 too.

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
   - Compile the required FFmpeg libraries from source as static archives
   - Configure and build SoX with all codecs
   - Reject a final executable that dynamically imports codec libraries
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

# Build without FFmpeg-backed formats
.\build_static_libs.ps1 -NoFfmpeg

# Build without the Vulkan FIR and DSD backends
.\build_static_libs.ps1 -NoVulkan

# Build with 4 parallel jobs
.\build_static_libs.ps1 -Jobs 4

# Keep dependency sources and build directories for incremental rebuilds
.\build_static_libs.ps1 -KeepBuild

# Clean build directories
.\build_static_libs.ps1 -Clean
```

**Linux/macOS/BSD:**
```bash
# Build without AMR and MP2 support
./build_static_libs.sh --no-amr --no-mp2

# Build without FFmpeg-backed formats
./build_static_libs.sh --no-ffmpeg

# Build with 4 parallel jobs
./build_static_libs.sh --jobs 4

# Add the shared-only PulseAudio client driver (Linux)
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
| Dolby TrueHD | Elementary stream (decode and experimental encode) | FFmpeg libavcodec, libavutil |
| MLP | Meridian Lossless Packing elementary stream (decode and experimental encode) | FFmpeg libavcodec, libavutil |
| DTS | Core elementary stream decode and experimental encode; DTS extensions and DTS-HD decode | FFmpeg libavcodec, libavutil |
| MP3 | MPEG Audio Layer III | libmad (decoder), LAME (encoder) |
| MP2 | MPEG Audio Layer II | TwoLAME (encoder) |
| WavPack | Lossless audio compression | libwavpack |
| DSD | DSF and DSDIFF/DFF read and write; WSD read | Built in |
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

---

## Available Arguments

### Windows (PowerShell)

| Argument | Description |
|----------|-------------|
| `-Jobs N` | Number of parallel build jobs (default: auto) |
| `-Clean` | Remove all build and output directories |
| `-KeepBuild` | Keep dependency sources, static libraries, and build directories |
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
| `-NoFfmpeg` | Exclude FFmpeg-backed format support |
| `-NoVulkan` | Exclude the Windows/NVIDIA Vulkan FIR and DSD backends |
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
| `--no-ffmpeg` | Exclude FFmpeg-backed format support |
| `--no-amr` | Exclude AMR support |
| `--no-id3tag` | Exclude ID3 tag support |
| `--no-png` | Exclude PNG spectrogram support |
| `--no-magic` | Exclude libmagic support |

**Vulkan Backend:**

| Argument | Description |
|----------|-------------|
| `--vulkan` | Enable the Vulkan FIR and DSD backends (needs a Vulkan loader, `glslc` and the glslang headers) |
| `--no-vulkan` | Disable them (default) |

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
| `--with-pulseaudio` | Include the dynamically linked PulseAudio driver |
| `--with-oss` | Include OSS driver |

---

## Verification

After the build completes, verify the installation. The build scripts reject
executables that dynamically import bundled codec libraries. Platform,
OpenMP, and Vulkan infrastructure dependencies are allowed to remain dynamic.

### 1. Check Version

```bash
# Windows
.\output\sox.exe --version

# Linux/macOS/BSD
./output/sox --version
```

Expected output:
```
sox:      SoX v15.0.0
```

### 2. Check Supported Formats

```bash
# Windows
.\output\sox.exe --help-format all

# Linux/macOS/BSD
./output/sox --help-format all
```

This lists all formats enabled in the current build. Look for:

- `flac` - FLAC support
- `mp3` - MP3 support
- `ogg` - OGG/Vorbis support
- `opus` - Opus support
- `ac3` - AC-3 elementary stream support
- `eac3`, `ec3` - E-AC-3 elementary stream support
- `aac`, `adts` - AAC with ADTS framing
- `latm`, `loas` - AAC with LOAS/LATM framing
- `usac`, `xheaac`, `xhe-aac` - xHE-AAC/USAC with LOAS/LATM framing
- `m4a` - Apple Lossless in an M4A container
- `truehd`, `thd` - Dolby TrueHD elementary stream
- `mlp` - Meridian Lossless Packing elementary stream
- `dts` - DTS core and extended elementary streams
- `dtshd` - DTS-HD elementary streams (decode only)
- `wav` - WAV support
- `wavpack` - WavPack support
- `dsf`, `dff` - DSD stream read and write
- `wsd` - WSD stream read

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

# Encode AC-3 and E-AC-3
./output/sox input-5.1.wav -C 448 output-5.1.ac3
./output/sox input-5.1.wav -C 768 output-5.1.eac3

# Encode AAC-LC with LOAS/LATM framing
./output/sox input.wav -C 128 output.latm

# Encode Apple Lossless
./output/sox input-24bit.wav -C 2 output.m4a

# Encode TrueHD, MLP, and DTS
./output/sox input-5.1-side.wav -b 24 --channel-layout '5.1(side)' output.thd
./output/sox input-24bit.wav -b 24 output.mlp
./output/sox input-5.1-side.wav -C 1536 --channel-layout '5.1(side)' output.dts

# Encode PCM to DSD512
./output/sox --multi-threaded --buffer 524288 input.wav -r 22579200 output.dsf rate -v 22579200 sdm -f sdm-8

# Generate a five-second test tone
./output/sox -n test.wav synth 5 sine 440
```

On Windows, enable the NVIDIA Vulkan encode path explicitly:

```powershell
.\output\sox.exe --multi-threaded --vulkan --buffer 524288 input.wav `
  -r 22579200 output.dsf `
  rate -v 22579200 sdm
```

Use the Vulkan FIR backend for long coefficient files:

```powershell
.\output\sox.exe --vulkan input.wav output.wav fir impulse.txt
```

#### Codec Summary

| Format | Encoding | Decoding and limits |
|--------|----------|---------------------|
| Opus | 1-8 channels; mapping family 0 for mono/stereo and family 1 for multichannel audio | 1-8 standard channels; SoX converts between canonical WAVE order and Vorbis order |
| AAC/ADTS | AAC-LC through `aac` or `adts` | AAC-LC, HE-AAC, and HE-AACv2 |
| AAC/LOAS-LATM | AAC-LC through `latm` or `loas` | AAC-LC, HE-AAC, HE-AACv2, and xHE-AAC/USAC through `usac`, `xheaac`, or `xhe-aac` |
| ALAC/M4A | 1-8 channels, 16- or 24-bit lossless audio | 1-8 channels; output must be seekable |
| AC-3 | Mono through 5.1 at 32, 44.1, or 48 kHz | Mono through 5.1 |
| E-AC-3 | Up to 5.1 at 32, 44.1, or 48 kHz | Up to 7.1 |
| TrueHD | 16- or 24-bit PCM up to 5.1 at 44.1-192 kHz | Channel-based presentations up to 7.1 |
| MLP | 16- or 24-bit PCM up to 5.1 at 44.1-192 kHz | Preserves valid 20-bit source precision; the encoder pads an incomplete final frame |
| DTS | DTS core up to 5.1(side), 32-3840 kbit/s | DTS core, DTS-ES, DTS 96/24, DTS-HD HRA/MA, DTS Express/LBR, and channel-based DTS:X presentations |
| DTS-HD | Not supported | Use `dtshd` for extended DTS-HD streams and `dts` for core or mixed inputs |
| DSD | DSF and DSDIFF/DFF at DSD64, DSD128, DSD256, DSD512 and DSD1024 | WSD is read-only; `sdm-4` through `sdm-8` and `clans-4` through `clans-8` are available |

Run `sox --help-format FORMAT` for the sample rates, channel layouts, compression values, and read/write capabilities available for a specific format.

#### DSD modulation

The `sdm` effect performs PCM-to-DSD sigma-delta modulation; it does not
resample. Put a separate `rate` effect before it, as in
`rate -v 22579200 sdm`, and use the normal `rate` options to choose quality,
bandwidth, phase, rejection, and aliasing behavior. DSF and DSDIFF writers
only pack the resulting one-bit stream. On CPU, channels are processed with
the OpenMP runtime limit and `sdm` retains the upstream `-f`, `-t`, `-n`, and
`-l` options. Each channel retains the full-rate causal SDM recurrence.

On builds compiled with Vulkan support, a global Vulkan profile joins an
adjacent `rate -> sdm` pair into a resident device segment. `rate` keeps its
CPU planner and all of its normal options, while the selected Vulkan profile
controls the numerical executor. The SDM consumes the resulting stream at
DSD64 through DSD1024 without another interpolation filter or a host PCM
round-trip. It accepts one through six channels, uses the conservative
MASH-2 finite-state modulator at a fixed −3 dB input gain, and reports
`-f`, `-t`, `-n`, and `-l` as ignored because those CPU modulator controls do
not alter the GPU implementation. DSF and DSDIFF output use a channel-major
packed-word path; padding is applied only once at the logical end, so the
duration is rounded up by at most 31 DSD samples. DSD decoding remains on the
normal CPU path.

Vulkan shaders are compiled to SPIR-V and embedded in the executable at build time, so no shader or data files are installed beside `sox.exe`. The system `vulkan-1.dll` loader is supplied with the Vulkan-capable display driver. VkFFT runtime shader compilation uses the official dynamic `glslang.dll` copied from the Vulkan SDK and distributed beside `sox.exe`; it is not compiled or linked statically. The Microsoft Visual C++ Redistributable is required by glslang and by the OpenMP runtime. The release gate was validated on Windows with an NVIDIA RTX 3080 using 30-second mono, stereo, and 5.1 inputs across DSD64 through DSD1024; the integrated DSF output was bit-identical to the standalone reference for signed 32-bit PCM input.

#### Vulkan FIR

On builds compiled with Vulkan support, the `--vulkan-fast`,
`--vulkan-accurate`, `--vulkan-strict`, and `--vulkan-reference` profiles
select the partitioned VkFFT backend for the classic `fir` effect. It accepts
one through six channels and preserves the coefficient file, output length,
drain, and signed 32-bit SoX boundary used by the CPU implementation. A
channel map uses 1-based channel numbers, comma-separated lists, and inclusive
ranges:

```text
fir 1,2=front.txt 3=center.txt 4=sub.txt
fir 1-6=room-correction.txt
```

Channels omitted from the map pass through and are delayed with the mapped
channels so the stream remains synchronized. SoX reports each omitted channel
as a warning. Repeated or overlapping assignments cascade in command-line
order; for example, `fir 1,2=a.txt 2=b.txt` applies `a` to channel 1 and
`a * b` to channel 2. Adjacent mapped `fir` effects remain eligible for Vulkan
fusion even when their maps differ.

Coefficient arrays with different lengths are padded to a shared pre-peak and
post-peak reference before execution. This preserves the relative delay
encoded by each filter while giving the CPU flows and batched Vulkan executor
the same tap count and alignment. At verbosity level 3, SoX reports each
resolved channel, source file, original tap count and normalized alignment.

The backend uses FP64, a 32768-point R2C/C2R transform, and 16384-tap uniform
partitions. Qualification covers the supplied 1,048,576-tap filter at 384 kHz
on 5.1 input and a generated 2,000,000-tap filter. `glslang.dll` is loaded only
when VkFFT needs runtime compilation, so non-Vulkan SoX commands do not require
initializing the Vulkan FIR path.

#### Vulkan rate

On Windows builds compiled with Vulkan support, `--vulkan` executes qualified `rate` plans containing arbitrary sequences of frequency-domain FIR, exact rational polyphase, and half-band FIR stages. The CPU planner remains authoritative for coefficients, topology, quality, phase, bandwidth, aliasing, sample count, latency, and drain. The GPU policy covers `medium`, `high`, and `very-high` exact-ratio plans, 44.1/48 kHz family crossings, common PCM multiples, and conversions to and from DSD64, DSD128, DSD256, DSD512, and DSD1024 rates. `quick`, `low`, interpolated polyphase clocks, block-incompatible interpolation factors, and unsupported plans continue through the unchanged CPU executor.

The backend uses the shared FP64 partitioned VkFFT FIR engine, performs the planner-selected integer interpolation and decimation around it, and uses a dedicated FP64 shader for rational polyphase and sparse half-band convolution. It batches interleaved mono, stereo, and 5.1 channels. A downstream upsampling DFT stage whose `post_peak` is not divisible by its interpolation factor remains on CPU because its interstage subphase is not yet represented by the Vulkan executor. Verbose output reports each Vulkan stage and the total stage count when a composite executor is selected.

Windows qualification on an RTX 3080 passed 127/127 cases, with 121 plans routed through Vulkan, a maximum signed-32-bit error of 2 LSB, maximum RMS error of 0.601106 LSB, minimum SNR of 174.122 dB, and no Vulkan validation errors. The unchanged CPU contract passed 690/690 cases. Standalone DSD-rate benchmarks remain slower than CPU: the measured Vulkan/CPU-single speed ratio ranged from 0.01x to 0.40x and from 0.01x to 0.26x against CPU multi-thread execution. Multiple pipeline startups, host-side staging, synchronous submissions, and transfers dominate, so this route is qualified for equivalence but is not a standalone performance recommendation; it is groundwork for the future GPU-resident chain.

#### Channel Layouts

Place `--channel-layout LAYOUT` immediately before an FFmpeg-backed output file when the input channels have already been prepared in that order. The option labels the existing channels; it does not remix them. The selected layout must match the channel count and be supported by the encoder.

Without `--channel-layout`, SoX uses its canonical layout when the codec offers multiple layouts for the same channel count. ALAC uses its required Apple layout. Noncanonical layouts are preserved and reported without automatic remixing.

ALAC uses these noncanonical orders:

- 4-channel MPEG 4.0 B: `FL FR FC BC`;
- 7-channel Apple AAC 6.1: `FL FR FC LFE BL BR BC`;
- 8-channel MPEG 7.1 B: `FL FR FC LFE BL BR FLC FRC`.

```bash
# Label eight channels already prepared in MPEG 7.1 B order
./output/sox prepared-8ch.wav -b 24 --channel-layout '7.1(wide)' output.m4a

# Label seven channels already prepared in AAC 6.1(back) order
./output/sox prepared-7ch.wav -C 448 --channel-layout '6.1(back)' output.aac
```

#### FFmpeg Options

Use `--ffmpeg-opts key=value:key=value` immediately before an FFmpeg-backed input or output file. Bitrate, sample rate, channels, layout, sample format, and time base remain controlled by SoX options.

```bash
# E-AC-3 encoder options
./output/sox input.wav -C 640 --ffmpeg-opts 'dialnorm=-27:dmix_mode=loro:stereo_rematrixing=0' output.eac3

# E-AC-3 decoder options
./output/sox --ffmpeg-opts 'drc_scale=0.5' input.eac3 output.wav
```

Unsupported options are rejected. Available passthrough options depend on the selected FFmpeg codec.

Dolby Atmos and DTS:X object metadata is reported but not rendered; decoding returns the channel-based presentation. DTS-UHD Profile 2 is not supported.

### 5. Verify Static Linking

```bash
# Linux/FreeBSD: inspect direct dynamic dependencies
readelf -d ./output/sox | grep NEEDED
ldd ./output/sox

# macOS: Check for dynamic dependencies
otool -L ./output/sox

# Windows (Visual Studio Developer PowerShell)
dumpbin /DEPENDENTS .\output\sox.exe
```

On Linux, operating-system libraries such as glibc, ALSA, and explicitly enabled PulseAudio libraries may be dynamic. FFmpeg, FLAC, Ogg/Vorbis, Opus, libsndfile, MP3/MP2, WavPack, AMR, PNG, libmagic, and libao must not appear as direct `DT_NEEDED` entries.

On FreeBSD, base-system and compiler runtime libraries such as `libc`, `libm`, `libthr`, and `libomp` may be dynamic. Codec libraries and libao must not appear, and the executable must not contain an `RPATH` or `RUNPATH` pointing to `/usr/local/lib`.

On macOS, every dependency must come from `/usr/lib` or `/System/Library`, for example:

- `libSystem.B.dylib`
- `CoreAudio.framework`
- `CoreFoundation.framework`

FFmpeg and the bundled codec libraries must not appear as `.dylib` dependencies.

A `--vulkan` build adds `libvulkan` and `libglslang`, which are dynamic here for
the same reason they are on Windows: the loader belongs to the display driver
and glslang is what VkFFT compiles shaders with, so neither can be linked
statically. Without `--vulkan` neither appears at all.

On Windows, operating-system DLLs such as `KERNEL32.dll`, `WINMM.dll`, and
`bcrypt.dll` may appear, together with `VCOMP140.dll` from the Microsoft Visual
C++ Redistributable for OpenMP support and `vulkan-1.dll` when Vulkan is
enabled. The Vulkan package also contains the dynamic `glslang.dll`, whose own
dependencies can include the Microsoft C/C++ runtime. Codec DLLs and MSYS
runtime DLLs must not appear: codec dependencies are linked statically, while
Vulkan, glslang, OpenMP, and their platform runtimes remain dynamic.

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
| FFmpeg | 8.1.2 |
| VkFFT | 1.3.4 |

---

## License

SoX is distributed under the GNU General Public License (GPL) and GNU Lesser General Public License (LGPL). See `LICENSE.GPL` and `LICENSE.LGPL` for details.
