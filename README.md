# SoX - Sound eXchange

SoX is a command-line audio processing tool for converting, processing, recording, and playing audio. This repository extends classic SoX with:

- Functional OpenMP-backed `--multi-threaded` processing across independent effect channels and CPU SDM channels; `--single-threaded` forces single-thread execution.
- Optional Vulkan acceleration for resampling, FIR filtering, and PCM-to-DSD conversion, with `fast`, `precise`, and `reference` numerical profiles and device-resident effect chains.
- Extended PCM-to-DSD conversion from DSD64 through DSD1024, with selectable high-order CPU modulators and an experimental parallel Vulkan MASH-2/FSM backend.
- FFmpeg-backed AC-3, E-AC-3, AAC, ALAC, TrueHD, MLP, DTS, DTS-HD, and xHE-AAC/USAC support.
- Reproducible static release builds for Windows, macOS, Linux, and FreeBSD; bundled codec libraries are linked statically.

---

## Table of Contents

1. [Dependencies](#dependencies)
2. [Build Instructions](#build-instructions)
3. [Default Options](#default-options)
4. [Build Options](#build-options)
5. [Vulkan Profiles](#vulkan-profiles)
6. [Verification](#verification)

---

## Dependencies

### Windows

**Required:**

- Visual Studio 2019 or later (with C++ workload)
- PowerShell 7 (`pwsh`) for the FFmpeg build; the script refuses to start on Windows PowerShell 5.1 unless `-NoFfmpeg` is given
- CMake 3.15 or later
- Vulkan SDK with `glslc`, glslang C headers, and `glslang.dll` (required by default for the Vulkan FIR and DSD backends; use `-NoVulkan` to disable them)
- MSYS2 with GNU Make, diffutils, and pkgconf (required for the static FFmpeg build)
- NASM (available in the MSYS2 environment)
- Git (optional, for cloning)

**Environment:**

- Run from a Visual Studio developer environment.
- Invoke PowerShell 7 explicitly with `pwsh`.
- Required on `PATH`: `cl.exe`, `dumpbin.exe`.
- CMake download: <https://cmake.org/download/>

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

`libomp` is required, not optional: the script builds with `SOX_REQUIRE_OPENMP=ON` and links Homebrew's `libomp.a`.

**Required by the `--vulkan` build option:**
```bash
brew install vulkan-loader vulkan-headers glslang shaderc
```

The Vulkan loader and headers are needed at compile time, `glslc` compiles the embedded shaders, and glslang compiles VkFFT shaders at run time.

**Optional:**
```bash
brew install nasm
```

`nasm` enables LAME's x86 assembly, so it is only useful on an Intel Mac.

### FreeBSD

**Required:**
```bash
sudo pkg install bash cmake git curl gmake autoconf automake libtool pkgconf nasm
```

---

## Build Instructions

### Quick Start

**Windows**, from a Developer Command Prompt for VS:
```bat
cd C:\path\to\sox-repo
pwsh -File .\build_static_libs.ps1
```

Without one, load the environment first with `VsDevCmd.bat -arch=x64 -host_arch=x64` from the Visual Studio `Common7\Tools` directory. Call `pwsh` explicitly: the developer shell is usually Windows PowerShell 5.1. For a native Windows ARM64 build, use `VsDevCmd.bat -arch=arm64 -host_arch=arm64` instead.

**Linux/macOS/BSD (Bash):**
```bash
cd /path/to/sox-repo
chmod +x build_static_libs.sh
./build_static_libs.sh
```

Output:

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

# Build with Vulkan support
./build_static_libs.sh --vulkan

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

## Build Options

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
| `-NoVulkan` | Exclude the Vulkan FIR and DSD backends |
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

## Vulkan Profiles

- Linux/macOS/BSD build: `./build_static_libs.sh --vulkan`
- Windows build: enabled by default; disable with `-NoVulkan`
- Runtime: select one profile
- No runtime `--vulkan` option

| Runtime option | Numerical family | Device requirement | Intended use |
|----------------|------------------|--------------------|--------------|
| omitted | CPU implementation | None | CPU execution |
| `--vulkan-fast` | FP32 FFT and accumulation | Vulkan compute | Highest GPU throughput and lowest memory use |
| `--vulkan-precise` | FP64 when available; double-single FP32 otherwise | Vulkan compute | High numerical accuracy with an FP32-device fallback |
| `--vulkan-reference` | FP64 double-double | Hardware `shaderFloat64` | Numerical reference and qualification |

FIR and rate depend on the selected execution profile:

| Backend/profile | Performance | FIR SNR, pre-quantization |
|-----------------|-------------|---------------------------|
| CPU | Lowest startup cost | 309–313 dB |
| Vulkan fast | Highest GPU throughput; lowest GPU memory | 139–140 dB |
| Vulkan precise | Between fast and reference | 288–311 dB |
| Vulkan reference | Highest setup and arithmetic cost | 624.77 dB in the qualified double-double case |

### DSD modulation

- CPU: selectable `sdm-*` and `clans-*` modulators; options `-f`, `-t`, `-n`, and `-l` apply.
- Vulkan: DSD64 through DSD1024, one to six channels, fixed MASH-2/FSM, −3 dB gain.
- Vulkan ignores CPU-only `sdm` options `-f`, `-t`, `-n`, and `-l`.

The Vulkan SDM backend is experimental. It is much faster than the CPU modulators, but its second-order noise shaping provides lower quality. Use it only at DSD256 or higher.

| Backend | Modulator | Performance | Quality |
|---------|-----------|-------------|---------|
| CPU | Selectable high-order `sdm-*` and `clans-*` | Lower throughput; OpenMP across channels | Recommended for maximum quality |
| Vulkan, all profiles | Fixed MASH-2/FSM | Highly parallel GPU backend | Experimental; use at DSD256 or higher |

PCM-to-DSD throughput on M5 Pro, stereo, 10-second input:

| Rate | CPU multi | Vulkan fast | Vulkan precise | Vulkan reference |
|------|-----------|-------------|----------------|------------------|
| DSD64 | 17.32× | 96.41× | 42.53× | Not available |
| DSD128 | 8.32× | 49.95× | 22.99× | Not available |
| DSD256 | 4.03× | 29.32× | 13.54× | Not available |
| DSD512 | 2.36× | 16.75× | 7.43× | Not available |
| DSD1024 | 1.16× | 8.24× | 3.76× | Not available |

PCM-to-DSD throughput on Ryzen 7 5700X3D and RTX 3080, stereo, 10-second input:

| Rate | CPU multi | Vulkan fast | Vulkan precise | Vulkan reference |
|------|-----------|-------------|----------------|------------------|
| DSD64 | 8.38× | 24.45× | 27.17× | 12.41× |
| DSD128 | 4.28× | 20.04× | 21.46× | 8.06× |
| DSD256 | 2.21× | 14.99× | 15.46× | 4.74× |
| DSD512 | 1.14× | 10.80× | 10.07× | 2.68× |
| DSD1024 | 0.56× | 5.99× | 6.60× | 1.41× |

| Rate | Vulkan SDM SNR, 20 Hz–20 kHz | Use |
|------|-------------------------------|-----|
| DSD64 | ~70 dB | Not recommended |
| DSD128 | ~85 dB | Not recommended |
| DSD256 | ~100 dB | Minimum useful rate |
| DSD512 | ~115 dB | Supported use |
| DSD1024 | ~130 dB | Supported use |

- The Vulkan SDM modulator is identical for `fast`, `precise`, and `reference`; the profile affects the preceding rate/FIR processing only.
- Vulkan reference was unavailable on the measured M5 Pro because it requires hardware `shaderFloat64`.
- Noise shaping is stable only at second order. Higher-order FSM variants were rejected because their state space explodes and truncation destroys quality.

### FIR

- Channel maps use 1-based numbers, comma-separated lists, and inclusive ranges.

```text
fir 1,2=front.txt 3=center.txt 4=sub.txt
fir 1-6=room-correction.txt
```

- Unmapped channels pass through with matched delay.
- Repeated channel assignments cascade in command-line order.
- Different coefficient lengths retain their relative alignment.

### Rate

- CPU planner remains authoritative for coefficients, topology, quality, phase, bandwidth, aliasing, length, latency, and drain.
- Vulkan executes supported DFT, polyphase, and half-band stages.
- Supported plans include `medium`, `high`, `very-high`, exact ratios, 44.1/48 kHz family conversion, and DSD rates.
- Unsupported stages fall back to CPU.
- Vulkan ignores `-R` and `-d`, with a warning: they are the only options that
  would exclude the effect from the device, and they say nothing about where
  the work should run. On CPU both continue to apply.
- Use `-V3` to inspect the selected route.

### DSD decoding

With a Vulkan profile selected, DSD is decoded on the device from the file's own packed bits: no bit is expanded into a sample on the host, and the half-band cascade becomes one fused filter that decimates in a single pass. Without a profile the CPU path is unchanged.

| Rule | Detail |
|------|--------|
| Conversion comes first | `vol`, `gain`, `trim` apply to the PCM, after `rate`. An effect in front of the conversion is an error, with or without a Vulkan flag. DSD-to-DSD chains are untouched. |
| Band ceiling | 20 kHz at DSD64, 30 kHz at DSD128, 60 kHz at DSD256, 90 kHz above. Beyond it a DSD stream holds shaped noise, not signal. |
| One DSD64 filter | The ceiling binds at every output rate: flat to 19.5 kHz, stopped at 20 kHz, from 44.1 kHz to 384 kHz. DSD64 decoded to 96 or 192 kHz is empty above 20 kHz. |
| Stop-band depth | The requested quality, capped by the profile's own SNR. Only the `fast` cap binds, and only from `-v` up. |
| Buffer                 | Decode with `--buffer 524288` or larger. One dispatch per call: at the 8 KiB default DSD512 runs at 4.1× instead of 19.2×. |

DSD-to-PCM throughput, stereo, 10-second input, to 44.1 kHz, `--buffer 524288`. M5 Pro:

| Rate | CPU single | CPU multi | Vulkan fast | Vulkan precise | Vulkan reference |
|------|-----------|-----------|-------------|----------------|------------------|
| DSD64 | 58.37× | 89.42× | 30.67× | 3.67× | Not available |
| DSD128 | 30.00× | 46.18× | 56.20× | 8.63× | Not available |
| DSD256 | 15.07× | 23.70× | 94.14× | 24.16× | Not available |
| DSD512 | 7.65× | 11.71× | 68.22× | 13.11× | Not available |
| DSD1024 | 3.80× | 6.01× | 39.26× | 6.67× | Not available |

Ryzen 7 5700X3D with RTX 3080:

| Rate | CPU single | CPU multi | Vulkan fast | Vulkan precise | Vulkan reference |
|------|-----------|-----------|-------------|----------------|------------------|
| DSD64 | 38.38× | 59.36× | 20.19× | 8.75× | 1.42× |
| DSD128 | 19.97× | 31.08× | 22.29× | 14.79× | 2.98× |
| DSD256 | 10.17× | 15.94× | 22.21× | 21.79× | 6.28× |
| DSD512 | 5.14× | 8.06× | 19.68× | 16.64× | 4.20× |
| DSD1024 | 2.57× | 4.05× | 15.10× | 11.20× | 2.51× |

DSD64 is the one case the CPU wins, for an arithmetic reason: the cascade halves its rate at every stage and spends about ten multiplies per output sample. Above DSD64 the ceiling shortens the response and the GPU leads by three to eight times.

Accuracy of the whole decode, scored against the exact rational convolution of the plan sox built:

| Path | SNR |
|------|-----|
| CPU | 182 dB |
| Vulkan fast | 113–123 dB |
| Vulkan precise | 260–269 dB on the FP32 fallback, 290–308 dB with `shaderFloat64` |
| Vulkan reference | 640.7 dB |

On a build whose samples are normalised the reference profile is exact: the captured pairs equal the exact rational sum in every bit. On a build that scales samples to `SOX_SAMPLE_MAX` the exact product no longer fits a pair, so the stage applies the factor on the device, before collapsing, and the sample written is the correctly rounded one, checked frame by frame against the exact product.

`rate -v` on DSD64 is the one measured plan whose response is too wide to fit a double-double: it reads 640.7 dB.

### Runtime examples

```bash
# PCM to DSD512
./output/sox --vulkan-precise --multi-threaded --buffer 524288 input.wav -r 22579200 output.dsf rate -v 22579200 sdm

# Long FIR
./output/sox --vulkan-fast input.wav output.wav fir impulse.txt
```

---

## Verification

After the build completes, verify the installation. The build scripts reject executables that dynamically import bundled codec libraries. Platform, OpenMP, and Vulkan infrastructure dependencies are allowed to remain dynamic.

### 1. Check Version

```bash
# Windows
.\output\sox.exe --version

# Linux/macOS/BSD
./output/sox --version
```

Expected output:
```
sox:      SoX v15.1.0
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
