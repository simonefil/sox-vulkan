# SoX - Sound eXchange

SoX is a command-line audio processing tool for converting, processing, recording, and playing audio. This repository extends classic SoX with:

- Functional OpenMP-backed `--multi-threaded` processing across independent effect channels and CPU SDM channels; `--single-threaded` forces single-thread execution.
- Optional Vulkan acceleration for resampling, FIR filtering, and PCM-to-DSD conversion, with `fast`, `precise`, and `reference` numerical profiles and device-resident effect chains.
- Extended PCM-to-DSD conversion from DSD64 through DSD1024, with selectable high-order CPU modulators (`sdm`) and a parallel Vulkan MASH-2/FSM modulator (`sdm-vulkan`) that trades noise performance for an order of magnitude in speed.
- FFmpeg-backed AC-3, E-AC-3, AAC, ALAC, TrueHD, MLP, DTS, DTS-HD, and xHE-AAC/USAC support.
- Reproducible static release builds for Windows, macOS, Linux, and FreeBSD; bundled codec libraries are linked statically.

---

## Table of contents

1. [Dependencies](#dependencies)
2. [Build instructions](#build-instructions)
3. [Default options](#default-options)
4. [Build options](#build-options)
5. [The sample pipeline](#the-sample-pipeline)
6. [Runtime options](#runtime-options)
7. [Vulkan profiles](#vulkan-profiles)
8. [Verification](#verification)

---

## Dependencies

### Windows

**Required:**

- Visual Studio 2019 or later (with C++ workload)
- PowerShell 7 (`pwsh`) for the FFmpeg build; the script refuses to start on Windows PowerShell 5.1 unless `-NoFfmpeg` is given
- CMake 3.15 or later
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

**Required by the `-Vulkan` build option:**

- Vulkan SDK with `glslc`, glslang C headers, and `glslang.dll`

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

**Required by the `--vulkan` build option:**
```bash
sudo apt-get install -y libvulkan-dev glslc glslang-dev
```

SoX requires Vulkan-Headers 1.3.270 or later for `VK_EXT_frame_boundary`. Debian 12/Bookworm ships older headers in `libvulkan-dev`; keep its loader library, but install newer Vulkan headers under `/usr/local/include` or use a current Vulkan SDK.

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

**Required by the `--vulkan` build option:**
```bash
sudo dnf install -y vulkan-loader-devel vulkan-headers glslc glslang-devel
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

**Required by the `--vulkan` build option:**
```bash
sudo pacman -S vulkan-icd-loader vulkan-headers shaderc glslang
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

**Required by the `--vulkan` build option:**
```bash
sudo pkg install vulkan-loader vulkan-headers shaderc glslang
```

Running a Vulkan-enabled binary requires a Vulkan Runtime to be installed: the Vulkan loader plus a compatible GPU driver or ICD. On macOS, install MoltenVK as the Vulkan implementation. The exact runtime package is hardware- and platform-specific. VkFFT is installed automatically by the static build scripts.

---

## Build instructions

### Quick start

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

### Build with custom options

**Windows:**
```powershell
# Build without MP2 and ID3 tag support
.\build_static_libs.ps1 -NoMp2 -NoId3tag

# Build without FFmpeg-backed formats
.\build_static_libs.ps1 -NoFfmpeg

# Build with Vulkan support
.\build_static_libs.ps1 -Vulkan

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

# Keep dependency sources and build directories for incremental rebuilds
./build_static_libs.sh --keep-build

# Clean build directories
./build_static_libs.sh --clean
```

---

## Default options

### Codecs (enabled by default)

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

### Additional codecs (Linux/macOS/BSD only)

| Codec | Description |
|-------|-------------|
| AMR | Adaptive Multi-Rate audio |
| libmagic | File type detection |

### Audio drivers by platform

| Platform | Default Drivers |
|----------|-----------------|
| Windows | WaveAudio |
| Linux | ALSA + libao |
| macOS | CoreAudio + libao |
| FreeBSD | OSS + libao |

---

## Build options

### Windows (PowerShell)

| Argument | Description |
|----------|-------------|
| `-Jobs N` | Number of parallel build jobs (default: auto) |
| `-Clean` | Remove all build and output directories |
| `-KeepBuild` | Keep dependency sources, static libraries, and build directories |
| `-Help` | Show help message |

**Codec exclusion:**

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
| `-NoId3tag` | Exclude ID3 tag support |
| `-NoPng` | Exclude PNG spectrogram support |

**Vulkan backend:**

| Argument | Description |
|----------|-------------|
| `-Vulkan` | Enable the Vulkan rate, FIR, and DSD backends (disabled by default; requires the Vulkan SDK) |

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

**Codec exclusion:**

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

**Vulkan backend:**

| Argument | Description |
|----------|-------------|
| `--vulkan` | Enable the Vulkan rate, FIR, and DSD backends (needs a Vulkan loader, `glslc` and the glslang headers) |
| `--no-vulkan` | Disable them (default) |

**Audio driver exclusion:**

| Argument | Description |
|----------|-------------|
| `--no-alsa` | Exclude ALSA driver |
| `--no-ao` | Exclude libao driver |
| `--no-coreaudio` | Exclude CoreAudio driver (macOS) |
| `--no-oss` | Exclude OSS driver |

**Audio driver inclusion:**

| Argument | Description |
|----------|-------------|
| `--with-alsa` | Include ALSA driver |
| `--with-coreaudio` | Include CoreAudio driver |
| `--with-pulseaudio` | Include the dynamically linked PulseAudio driver |
| `--with-oss` | Include OSS driver |

---

## The sample pipeline

Samples travel through SoX as `double`, normalised to the half-open range
`[-1, +1)`. `SOX_SAMPLE_MAX` is `1.0` and is a limit, not a scale factor.

This is what the pipeline used to be, and what changed:

| | Before | Now |
|---|---|---|
| `sox_sample_t` | `int32_t`, full scale `2^31 - 1` | `double`, full scale `1.0` |
| Resolution | 32 bits | 53 bits of mantissa |
| Between two effects | rounded back to the integer grid | passed through unchanged |
| Where clipping happens | at every link | only where a sample leaves for a file |
| `-e float -b 64` input | 22 bits lost | exact |

A chain of neutral effects is now the exact identity. It was not before: each
link rounded to the 32-bit grid, so ten `gain 0` in a row cost ten roundings.

**This is an ABI break.** `SOVERSION` goes from 3 to 4, and every program that
links libsox has to be recompiled: `sox_sample_t` is in the signature of
`sox_read`, `sox_write`, the packed-DSD entry points and every effect handler.
An `int32_t *` is not a `double *` under any cast, so there is no compatibility
shim and none is planned.

Two things to know before upgrading:

- **Fewer clips are reported.** Material at or slightly over 0 dBFS used to
  count a clip at each effect and now counts one only on the way out.
- **CAF, W64 and MAT files written at `-b 32` by SoX 15.x and earlier are
  wrong.** They hold integer sample magnitudes inside a 32-bit float subtype;
  no other program reads them correctly, and SoX read them back correctly only
  because it made the matching mistake in the other direction. Rewrite them
  with the older SoX to a lossless integer format before upgrading.

---

## Runtime options

`sox --help` lists every global option. These are the ones that decide where the
work runs and how much of it moves per call.

| Option | Default | What it does |
|--------|---------|--------------|
| `--multi-threaded`, `--single-threaded` | single | OpenMP across the effect's channels. Pays off only when a call carries enough frames, see [Buffer size](#buffer-size). |
| `--buffer N` | 8192 | Frames per processing call, counted in samples, so each channel gets `N` divided by the channel count. The option with the largest effect on device throughput, see [Buffer size](#buffer-size). (The usage line says BYTES. It's samples.) |
| `--input-buffer N` | as `--buffer` | The same, input side only. |
| `--vulkan-fast`, `--vulkan-precise`, `--vulkan-reference` | none | Picks a device profile. Mutually exclusive. Omit them and everything runs on the CPU. |

Two more worth knowing. `--diagnostics DIR` writes a machine-readable `run.txt`
with the backend each effect actually took and the phase timings. `--dft-min N`
sets the smallest DFT the FIR and rate stages will use, as a power of two,
default 10.

---

## Vulkan profiles

Build with `./build_static_libs.sh --vulkan` on Linux, macOS and BSD, or
`.\build_static_libs.ps1 -Vulkan` on Windows. Then pick one profile at runtime.
There's no plain `--vulkan`.

| Profile | Arithmetic | Device requirement | Use for |
|---------|------------|--------------------|---------|
| omitted | CPU | None | CPU execution |
| `--vulkan-fast` | FP32 FFT and accumulation | Vulkan compute | Throughput, lowest device memory |
| `--vulkan-precise` | FP64 where the device has it, double-single FP32 otherwise | Vulkan compute | Accuracy on any device |
| `--vulkan-reference` | FP64 double-double | Hardware `shaderFloat64` | Qualification |

The profile decides where `rate`, `fir` and DSD decoding run. The modulator goes
by name instead:

| Effect | Runs on |
|--------|---------|
| `rate`, `fir`, DSD decode | The device under a profile, the CPU without one |
| `sdm` | The CPU, always |
| `sdm-vulkan` | The device, and it needs a profile |

In v15 `sdm` under a Vulkan flag quietly became the device modulator. It doesn't
any more, and that's the one incompatibility to watch for in old command lines.

### Rate

- The CPU planner owns coefficients, topology, quality, phase, bandwidth, aliasing, length, latency and drain.
- Vulkan runs the DFT, polyphase and half-band stages it supports: `medium`, `high`, `very-high`, exact ratios, the 44.1/48 kHz family, DSD rates.
- Everything else falls back to the CPU. `-V3` shows the route taken.
- `-R` and `-d` keep working on the CPU. Under a profile they're ignored with a warning: they'd exclude the effect from the device, and they say nothing about where the work should run.

### FIR

- Channel maps use 1-based numbers, comma-separated lists, and inclusive ranges.

```text
fir 1,2=front.txt 3=center.txt 4=sub.txt
fir 1-6=room-correction.txt
```

- Unmapped channels pass through with matched delay.
- Repeated channel assignments cascade in command-line order.
- Different coefficient lengths retain their relative alignment.

### What the profiles cost and buy

Twelve FIR configurations, `--buffer 262144`, on an M5 Pro and on a Ryzen 7
5700X3D with an RTX 3080: 8192 to 131072 taps, 48 to 384 kHz, 1 to 16 channels,
one shared filter or one filter per channel. SNR is against the same
convolution computed in exact rational arithmetic, so the figures are the
distance from the correct answer, not from each other.

| Profile | FIR SNR, M5 Pro | FIR SNR, RTX 3080 | Median throughput, M5 Pro / RTX 3080 |
|---------|-----------------|-------------------|--------------------------------------|
| CPU single | 282–310 dB | 278–310 dB | 286× / 105× |
| CPU multi | 282–310 dB | 278–310 dB | 400× / 132× |
| `--vulkan-fast` | 112–135 dB | 112–135 dB | 166× / 41× |
| `--vulkan-precise` | 263–288 dB | 279–311 dB | 98× / 40× |
| `--vulkan-reference` | Not available | 280–630 dB | n/a / 16× |

Same code, 16 to 25 dB apart on `precise`: the 3080 has `shaderFloat64` and gets
real FP64, Apple silicon falls back to double-single FP32 through MoltenVK.
`reference` needs `shaderFloat64` outright, so it does not run on the Mac at
all.

The CPU wins the median case on both machines. The device pulls ahead on
long filters and high channel counts, so measure the chain you actually run.

One configuration sits outside those ranges, and it is worth knowing why:

```text
fir a.txt fir b.txt          # two effects, chained
fir 1-2=a.txt 1-2=b.txt      # one effect, the two responses convolved first
```

These are not the same computation. Each `fir` emits as many samples as it
reads and aligns them on the peak of its response, which means it discards the
part of the convolution that comes before the peak: 8191 samples of leading
ringing for a 16384-tap filter. Chained, that ringing is thrown away before the
second filter can use it, so the second filter starts from an input that is
missing its first 8191 samples.

With two 16384-tap filters the damage is confined to the first 2728 output
samples and is exactly zero afterwards, but inside that stretch it is large,
about 4% of the level there. Measured over the first second the chained route
lands 34.3 dB from the correct answer, where the one-effect form is at 282 dB.

Under a `--vulkan-*` profile the question does not arise. `fir.c` convolves two
adjacent `fir` effects into a single response before anything runs, so there is
one alignment and the result is the exact one: 112 dB on `fast`, 263 to 279 on
`precise`, 605 on `reference`.

If you cascade filters, write them as one effect.

### DSD modulation

Three ways to reach DSD. The flag picks the rate stage, the effect name picks
the modulator.

| Path | Command | Rate stage | Modulator |
|------|---------|------------|-----------|
| CPU | `rate R sdm` | CPU | CPU, selectable `sdm-*` or `clans-*` |
| Hybrid | `--vulkan-P … rate R sdm` | Device | CPU |
| Device MASH | `--vulkan-P … rate R sdm-vulkan` | Device | Device, fixed MASH-2/FSM |

`sdm` takes `-f` (`sdm-4` to `sdm-8`, `clans-4` to `clans-8`) plus the trellis
overrides `-t`, `-n` and `-l`. It modulates at its input rate, so `rate` has to
reach the DSD rate first.

`sdm-vulkan` takes no options, applies −3 dB, and covers DSD64 to DSD1024 on 1
to 6 channels. Its shaping is fixed at second order. Higher-order FSM variants
blow up their state space, and truncating it costs more than the extra order
buys.

PCM to DSD, stereo, 10-second input, `--buffer 262144`. M5 Pro:

| Rate | CPU single | CPU multi | Hybrid fast | Hybrid precise | MASH fast | MASH precise |
|------|-----------|-----------|-------------|----------------|-----------|--------------|
| DSD64 | 18.15× | 30.32× | 29.97× | 22.15× | 108.61× | 47.67× |
| DSD128 | 9.10× | 15.50× | 16.74× | 12.20× | 60.68× | 26.71× |
| DSD256 | 4.52× | 7.73× | 8.67× | 6.51× | 33.33× | 14.84× |
| DSD512 | 2.63× | 4.38× | 5.35× | 3.82× | 17.95× | 7.90× |
| DSD1024 | 1.23× | 2.05× | 2.52× | 1.96× | 9.17× | 4.01× |

Ryzen 7 5700X3D with RTX 3080, same input and buffer. This machine has
`shaderFloat64`, so the reference columns exist here:

| Rate | CPU single | CPU multi | Hybrid fast | Hybrid precise | Hybrid reference | MASH fast | MASH precise | MASH reference |
|------|-----------|-----------|-------------|----------------|------------------|-----------|--------------|----------------|
| DSD64 | 8.63× | 10.99× | 9.74× | 9.86× | 5.80× | 43.48× | 41.49× | 12.17× |
| DSD128 | 4.38× | 5.49× | 5.44× | 5.32× | 3.13× | 37.04× | 32.05× | 8.00× |
| DSD256 | 2.18× | 2.78× | 2.76× | 2.79× | 1.66× | 25.19× | 22.62× | 4.67× |
| DSD512 | 1.12× | 1.35× | 1.49× | 1.48× | 0.85× | 15.95× | 14.06× | 2.57× |
| DSD1024 | 0.58× | 0.73× | 0.73× | 0.71× | 0.42× | 8.83× | 7.79× | 1.34× |

Those Windows figures use 262144 because every table here does, and that value
is the Mac's optimum, not this machine's. Read [Buffer size](#buffer-size)
before taking them as this hardware's ceiling.

The hybrid path buys little. The modulator stays on the host and it's the larger
half of the work, so moving `rate` onto the device gets you 8.67× against 7.73×
at DSD256 on the M5 Pro, and nothing at all on the Windows box.

The device modulator is where the order of magnitude is, and it costs 30 dB:

| Rate | CPU `sdm` in-band SNR | Device MASH in-band SNR | Use |
|------|----------------------|-------------------------|-----|
| DSD64 | 122.4 dB | 70.4 dB | Not recommended |
| DSD128 | 125.7 dB | 85.5 dB | Not recommended |
| DSD256 | 129.1 dB | 100.5 dB | Minimum useful rate |
| DSD512 | 147.5 dB | 116.0 dB | Supported use |
| DSD1024 | 159.6 dB | 129.7 dB | Supported use |

Measured 20 Hz to 20 kHz on the default `sdm` filter. The two machines agree
within 0.6 dB.

The three MASH columns differ in speed only. Same shaping under `fast`,
`precise` and `reference`, because the profile changes the rate stage in front
of the modulator, not the modulator.

On the hybrid path `fast` does bite, high up: 139.2 dB against 147.5 at DSD512,
139.7 against 159.6 at DSD1024. Below DSD512 the three profiles agree within a
few tenths of a dB. Use `precise` above DSD256.

### DSD decoding

Under a profile, DSD is decoded on the device from the file's own packed bits.
No bit is expanded into a sample on the host, and the half-band cascade becomes
one fused filter that decimates in a single pass. Without a profile the CPU path
is unchanged.

| Rule | Detail |
|------|--------|
| Conversion comes first | `vol`, `gain`, `trim` apply to the PCM, after `rate`. An effect in front of the conversion is an error, with or without a Vulkan flag. DSD-to-DSD chains are untouched. |
| Band ceiling | 20 kHz at DSD64, 30 kHz at DSD128, 60 kHz at DSD256, 90 kHz above. Past it a DSD stream holds shaped noise. |
| One DSD64 filter | The ceiling binds at every output rate: flat to 19.5 kHz, stopped at 20 kHz, from 44.1 kHz to 384 kHz. DSD64 decoded to 96 or 192 kHz is empty above 20 kHz. |
| Stop-band depth | The requested quality, capped by the profile's own SNR. Only the `fast` cap binds, and only from `-v` up. |
| Buffer | `--buffer 524288` or larger. One dispatch per call, so the default costs almost everything the device gains: DSD512 to 44.1 kHz on `--vulkan-fast` runs at 7.2× at the default and 69.7× at `524288`, same M5 Pro. |

DSD to PCM, stereo, 10-second input, to 44.1 kHz, `--buffer 524288`. M5 Pro:

| Rate | CPU single | CPU multi | Vulkan fast | Vulkan precise | Vulkan reference |
|------|-----------|-----------|-------------|----------------|------------------|
| DSD64 | 58.52× | 93.51× | 30.62× | 3.61× | Not available |
| DSD128 | 30.44× | 46.62× | 62.46× | 8.54× | Not available |
| DSD256 | 15.34× | 24.14× | 98.74× | 23.99× | Not available |
| DSD512 | 7.73× | 12.15× | 66.38× | 12.99× | Not available |
| DSD1024 | 3.90× | 6.12× | 39.90× | 6.55× | Not available |

Ryzen 7 5700X3D with RTX 3080:

| Rate | CPU single | CPU multi | Vulkan fast | Vulkan precise | Vulkan reference |
|------|-----------|-----------|-------------|----------------|------------------|
| DSD64 | 27.78× | 36.63× | 34.72× | 9.79× | 1.47× |
| DSD128 | 13.77× | 18.69× | 38.46× | 18.18× | 3.18× |
| DSD256 | 7.17× | 9.52× | 38.76× | 28.25× | 7.09× |
| DSD512 | 3.61× | 4.76× | 32.36× | 20.58× | 4.61× |
| DSD1024 | 1.82× | 2.41× | 23.75× | 12.84× | 2.63× |

DSD64 is the one rate the CPU wins, and the reason is arithmetic: the cascade
halves its rate at every stage and spends about ten multiplies per output
sample. Above DSD64 the ceiling shortens the response and the device leads by
three to eight times.

Accuracy of the whole decode, scored against the exact rational convolution of
the plan sox built, on DSD64 and DSD256 to 44.1 and 88.2 kHz:

| Path | SNR, M5 Pro | SNR, RTX 3080 |
|------|-------------|---------------|
| CPU | 182–197 dB | 182–197 dB |
| Vulkan fast | 113–123 dB | 113–123 dB |
| Vulkan precise | 260–269 dB (double-single fallback) | 287–296 dB (`shaderFloat64`) |
| Vulkan reference | Not available | Bit-exact |

On a build whose samples are normalised, `reference` is exact: the captured
pairs equal the exact rational sum in every bit, which is what the RTX 3080
column reports. On a build that scales samples to `SOX_SAMPLE_MAX` the exact
product no longer fits a pair, so the stage applies the factor on the device
before collapsing, and the sample written is the correctly rounded one, checked
frame by frame against the exact product.

`rate -v` on DSD64 is the one measured plan whose response is too wide for a
double-double. It reads 630.1 dB.

### Buffer size

The default is conservative and unchanged. On the Vulkan paths one dispatch goes
out per call, so a small buffer means many small dispatches and far more
host-device synchronisation than the work needs.

Starting points, measured on the two machines above:

| Workload | Buffer |
|----------|--------|
| DSD decoding (DSD to PCM) | `524288` or larger |
| PCM to DSD, stereo, Apple silicon | `262144` |
| PCM to DSD, stereo, Ryzen 7 5700X3D with RTX 3080 | `65536` |
| More than two channels | Multiply the stereo value by half the channel count: `786432` for 5.1 |

The two platforms don't share an optimum. On the M5 Pro throughput keeps
improving up to `262144`. On the Windows machine the Vulkan path degrades past
`65536`, by 13% at `262144` and 21% at `1048576`. Measure yours, with `-V3` to
see the submit count.

Scaling the buffer with the channel count keeps a multichannel run at the same
frames per channel as a stereo one, and it matters most with
`--multi-threaded`. On 5.1 DSD256 with `sdm -f sdm-8`, at the default buffer
`--multi-threaded` is *slower* than single-threaded, 7.79 s against 4.66 s: each
call is too short for the fork and join to repay themselves. At `--buffer
786432` the same run takes 1.21 s, 3.9 times faster than single-threaded.

### Examples

```bash
# PCM to DSD512, hybrid: rate on the device, modulator on the CPU
./output/sox --vulkan-precise --multi-threaded --buffer 262144 input.wav -r 22579200 output.dsf rate -v 22579200 sdm

# PCM to DSD512, all on the device: an order of magnitude faster, 31 dB noisier
./output/sox --vulkan-precise --multi-threaded --buffer 262144 input.wav -r 22579200 output.dsf rate -v 22579200 sdm-vulkan

# PCM to DSD512 on the CPU alone, highest quality
./output/sox --multi-threaded --buffer 262144 input.wav -r 22579200 output.dsf rate -v 22579200 sdm -f sdm-8

# DSD to PCM
./output/sox --vulkan-fast --buffer 524288 input.dsf output.wav rate 44100

# Long FIR
./output/sox --vulkan-fast input.wav output.wav fir impulse.txt
```

---

## Verification

After the build completes, verify the installation. The build scripts reject executables that dynamically import bundled codec libraries. Platform, OpenMP, and Vulkan infrastructure dependencies are allowed to remain dynamic.

### 1. Check version

```bash
# Windows
.\output\sox.exe --version

# Linux/macOS/BSD
./output/sox --version
```

Expected output:
```
sox:      SoX v16.2.0
```

### 2. Check supported formats

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

### 3. Check audio devices

```bash
# Windows
.\output\sox.exe --help-device all

# Linux/macOS/BSD
./output/sox --help-device all
```

### 4. Test conversion

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

## Library versions

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

## LLM disclaimer

This project uses AI-assisted development tools.

**Tools**

- Codex `gpt5.6-sol`
- Claude `opus5`

### Contribution profile

```
Phase                               Human│ AI
─────────────────────────────────────────┼───────────────
Requirements & Scope       95% ██████████│             5%
Architecture & Design      95% ██████████│             5%
Implementation             40%       ████│░░░░░░      60%
Testing                    20%         ██│░░░░░░░░    80%
Documentation               5%           │░░░░░░░░░░  95%
```
