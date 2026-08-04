#
# SoX Static Libraries Build Script for Windows
# Downloads and compiles all optional libraries as static libraries
#
# Usage: .\build_static_libs.ps1 [OPTIONS]
#
# Requirements: Visual Studio 2019 or later, PowerShell 7, CMake, Git, MSYS2, NASM
#

param(
    [int]$Jobs = $env:NUMBER_OF_PROCESSORS,
    [switch]$Clean,
    [switch]$KeepBuild,
    [switch]$Help,

    # Codec exclusion options (all enabled by default)
    [switch]$NoOgg,
    [switch]$NoFlac,
    [switch]$NoOpus,
    [switch]$NoMp3,
    [switch]$NoMp2,
    [switch]$NoWavpack,
    [switch]$NoSndfile,
    [switch]$NoId3tag,
    [switch]$NoPng,
    [switch]$NoFfmpeg,
    [switch]$NoVulkan,

    # Audio driver options
    # Default: waveaudio (libao not supported on Windows)
    [switch]$NoWaveaudio
)

$ErrorActionPreference = "Stop"

# Configuration
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ScriptDir "build_deps"
$StaticLibsDir = Join-Path $ScriptDir "static_libs"
$DownloadDir = Join-Path $BuildDir "downloads"
$SrcDir = Join-Path $BuildDir "src"
$SoxBuildDir = Join-Path $ScriptDir "build"
$OutputDir = Join-Path $ScriptDir "output"

# ------------------------------------------------------------------------------
# CODEC OPTIONS (default: all ON)
# ------------------------------------------------------------------------------
$EnableOgg = -not $NoOgg
$EnableFlac = -not $NoFlac
$EnableOpus = -not $NoOpus
$EnableMp3 = -not $NoMp3
$EnableMp2 = -not $NoMp2
$EnableWavpack = -not $NoWavpack
$EnableSndfile = -not $NoSndfile
$EnableId3tag = -not $NoId3tag
$EnablePng = -not $NoPng
$EnableFfmpeg = -not $NoFfmpeg
$EnableVulkan = -not $NoVulkan

# ------------------------------------------------------------------------------
# AUDIO DRIVER OPTIONS
# Windows defaults: waveaudio only (libao not supported on Windows)
# ------------------------------------------------------------------------------
$EnableWaveaudio = -not $NoWaveaudio

# Library versions
$Versions = @{
    zlib = "1.3.1"
    libpng = "1.6.43"
    libogg = "1.3.5"
    libvorbis = "1.3.7"
    flac = "1.4.3"
    opus = "1.5.2"
    opusfile = "0.12"
    libopusenc = "0.3"
    lame = "3.100"
    twolame = "0.4.0"
    wavpack = "5.7.0"
    libsndfile = "1.2.2"
    libmad = "0.15.1b"
    libid3tag = "0.15.1b"
    ffmpeg = "8.1.2"
    vkfft = "1.3.4"
}

# ------------------------------------------------------------------------------
# Helper functions
# ------------------------------------------------------------------------------

function Write-Info($msg) {
    Write-Host "[INFO] $msg" -ForegroundColor Cyan
}

function Write-Success($msg) {
    Write-Host "[OK] $msg" -ForegroundColor Green
}

function Write-Warn($msg) {
    Write-Host "[WARN] $msg" -ForegroundColor Yellow
}

function Write-Err($msg) {
    Write-Host "[ERROR] $msg" -ForegroundColor Red
}

function Test-Command($cmd) {
    $null = Get-Command $cmd -ErrorAction SilentlyContinue
    return $?
}

function Find-CMake {
    # Check if cmake is in PATH
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmake) {
        return $cmake.Source
    }

    # Search in common locations
    $searchPaths = @(
        "C:\Program Files\CMake\bin\cmake.exe",
        "C:\Program Files (x86)\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2019\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2019\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2019\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )

    foreach ($path in $searchPaths) {
        if (Test-Path $path) {
            return $path
        }
    }

    return $null
}

# Global cmake path
$script:CMakePath = $null

function Download-File($url, $output) {
    $partialOutput = "$output.part"

    if (Test-Path $output) {
        # Check if file is valid (not HTML error page)
        $content = Get-Content $output -First 1 -ErrorAction SilentlyContinue
        if ($content -notmatch "^<!DOCTYPE|^<html|^<HTML") {
            Write-Info "Already downloaded: $(Split-Path -Leaf $output)"
            return
        }
        # Invalid file, remove and re-download
        Remove-Item $output -Force
    }

    Write-Info "Downloading: $(Split-Path -Leaf $output)"
    Remove-Item $partialOutput -Force -ErrorAction SilentlyContinue

    # Use TLS 1.2/1.3
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13

    try {
        # Prefer curl for better redirect handling
        $curlPath = "C:\Windows\System32\curl.exe"
        if (Test-Path $curlPath) {
            & $curlPath -fL -o $partialOutput $url --retry 3 --retry-delay 2
            if ($LASTEXITCODE -eq 0) {
                Move-Item $partialOutput $output -Force
                return
            }
            Remove-Item $partialOutput -Force -ErrorAction SilentlyContinue
        }

        # Fallback to Invoke-WebRequest
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $url -OutFile $partialOutput -UseBasicParsing -MaximumRedirection 10
        Move-Item $partialOutput $output -Force
    }
    catch {
        throw "Failed to download: $url - $_"
    }
    finally {
        Remove-Item $partialOutput -Force -ErrorAction SilentlyContinue
    }
}

function Extract-Archive($archive, $dest) {
    Write-Info "Extracting: $(Split-Path -Leaf $archive)"

    # Use Windows-native tar with proper path handling
    $archiveWin = $archive -replace '/', '\'
    $destWin = $dest -replace '/', '\'

    if ($archive -match "\.tar\.gz$|\.tgz$") {
        # Use PowerShell to handle .tar.gz
        Push-Location $destWin
        try {
            # First decompress .gz, then extract .tar
            $tarFile = [System.IO.Path]::GetFileNameWithoutExtension($archiveWin)
            if ($tarFile -match "\.tar$") {
                $tarFile = $tarFile
            } else {
                $tarFile = $tarFile + ".tar"
            }

            # Try Windows tar first
            $env:Path = "C:\Windows\System32;$env:Path"
            & "C:\Windows\System32\tar.exe" -xzf "$archiveWin" 2>$null
            if ($LASTEXITCODE -ne 0) {
                # Fallback: use .NET for gzip and tar
                $gzStream = [System.IO.File]::OpenRead($archiveWin)
                $gzipStream = New-Object System.IO.Compression.GZipStream($gzStream, [System.IO.Compression.CompressionMode]::Decompress)
                $tarPath = Join-Path $destWin $tarFile
                $outStream = [System.IO.File]::Create($tarPath)
                $gzipStream.CopyTo($outStream)
                $outStream.Close()
                $gzipStream.Close()
                $gzStream.Close()

                & "C:\Windows\System32\tar.exe" -xf "$tarPath"
                Remove-Item $tarPath -ErrorAction SilentlyContinue
            }
        }
        finally {
            Pop-Location
        }
    }
    elseif ($archive -match "\.tar\.xz$") {
        Push-Location $destWin
        try {
            & "C:\Windows\System32\tar.exe" -xJf "$archiveWin"
        }
        finally {
            Pop-Location
        }
    }
    elseif ($archive -match "\.zip$") {
        Expand-Archive -Path $archiveWin -DestinationPath $destWin -Force
    }
    else {
        throw "Unknown archive format: $archive"
    }
}

function Get-CMakeFlags {
    return @(
        "-DCMAKE_INSTALL_PREFIX=$StaticLibsDir"
        "-DBUILD_SHARED_LIBS=OFF"
        "-DCMAKE_BUILD_TYPE=Release"
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
        "-DCMAKE_PREFIX_PATH=$StaticLibsDir"
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
        "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW"
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
    )
}

function Build-CMakeProject($srcDir, $extraFlags = @()) {
    $buildDir = Join-Path $srcDir "build"
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
    Push-Location $buildDir

    try {
        $flags = Get-CMakeFlags
        $flags += $extraFlags

        & $script:CMakePath .. @flags
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

        & $script:CMakePath --build . --config Release --parallel $Jobs
        if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

        & $script:CMakePath --install . --config Release
        if ($LASTEXITCODE -ne 0) { throw "CMake install failed" }
    }
    finally {
        Pop-Location
    }
}

# ------------------------------------------------------------------------------
# Build functions for each library
# ------------------------------------------------------------------------------

function Install-VkFFT {
    Write-Info "========== Installing VkFFT $($Versions.vkfft) =========="

    $url = "https://github.com/DTolm/VkFFT/archive/refs/tags/v$($Versions.vkfft).tar.gz"
    $archive = Join-Path $DownloadDir "VkFFT-$($Versions.vkfft).tar.gz"
    $src = Join-Path $SrcDir "VkFFT-$($Versions.vkfft)"
    $expectedSha256 = "b61055393adb3adc79009fe12401cbfbbdfba584e665e9c35fcbf4b32fb31f30"

    Download-File $url $archive
    $actualSha256 = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualSha256 -ne $expectedSha256) {
        throw "VkFFT archive checksum mismatch: $actualSha256"
    }

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    $includeDir = Join-Path $StaticLibsDir "include"
    $licenseDir = Join-Path $StaticLibsDir "share\licenses\VkFFT"
    New-Item -ItemType Directory -Path $includeDir -Force | Out-Null
    New-Item -ItemType Directory -Path $licenseDir -Force | Out-Null
    Copy-Item (Join-Path $src "vkFFT") $includeDir -Recurse -Force
    Copy-Item (Join-Path $src "LICENSE") $licenseDir -Force

    Write-Success "VkFFT headers installed"
}

function Build-Zlib {
    Write-Info "========== Building zlib $($Versions.zlib) =========="

    $url = "https://github.com/madler/zlib/releases/download/v$($Versions.zlib)/zlib-$($Versions.zlib).tar.gz"
    $archive = Join-Path $DownloadDir "zlib-$($Versions.zlib).tar.gz"
    $src = Join-Path $SrcDir "zlib-$($Versions.zlib)"

    Download-File $url $archive

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    Build-CMakeProject $src @(
        "-DZLIB_BUILD_EXAMPLES=OFF"
    )

    Write-Success "zlib installed"
}

function Build-Libpng {
    Write-Info "========== Building libpng $($Versions.libpng) =========="

    $url = "https://github.com/pnggroup/libpng/archive/refs/tags/v$($Versions.libpng).tar.gz"
    $archive = Join-Path $DownloadDir "libpng-$($Versions.libpng).tar.gz"
    $src = Join-Path $SrcDir "libpng-$($Versions.libpng)"

    Download-File $url $archive

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    Build-CMakeProject $src @(
        "-DPNG_SHARED=OFF"
        "-DPNG_STATIC=ON"
        "-DPNG_TESTS=OFF"
        "-DZLIB_ROOT=$StaticLibsDir"
    )

    Write-Success "libpng installed"
}

function Build-Libogg {
    Write-Info "========== Building libogg $($Versions.libogg) =========="

    $url = "https://github.com/xiph/ogg/releases/download/v$($Versions.libogg)/libogg-$($Versions.libogg).tar.gz"
    $archive = Join-Path $DownloadDir "libogg-$($Versions.libogg).tar.gz"
    $src = Join-Path $SrcDir "libogg-$($Versions.libogg)"

    Download-File $url $archive

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    Build-CMakeProject $src @(
        "-DBUILD_TESTING=OFF"
        "-DINSTALL_DOCS=OFF"
    )

    Write-Success "libogg installed"
}

function Build-Libvorbis {
    Write-Info "========== Building libvorbis $($Versions.libvorbis) =========="

    $url = "https://github.com/xiph/vorbis/releases/download/v$($Versions.libvorbis)/libvorbis-$($Versions.libvorbis).tar.gz"
    $archive = Join-Path $DownloadDir "libvorbis-$($Versions.libvorbis).tar.gz"
    $src = Join-Path $SrcDir "libvorbis-$($Versions.libvorbis)"

    Download-File $url $archive

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    Build-CMakeProject $src @(
        "-DOGG_ROOT=$StaticLibsDir"
    )

    Write-Success "libvorbis installed"
}

function Build-Flac {
    Write-Info "========== Building FLAC $($Versions.flac) =========="

    $url = "https://github.com/xiph/flac/releases/download/$($Versions.flac)/flac-$($Versions.flac).tar.xz"
    $archive = Join-Path $DownloadDir "flac-$($Versions.flac).tar.xz"
    $src = Join-Path $SrcDir "flac-$($Versions.flac)"

    Download-File $url $archive

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    Build-CMakeProject $src @(
        "-DBUILD_PROGRAMS=OFF"
        "-DBUILD_EXAMPLES=OFF"
        "-DBUILD_TESTING=OFF"
        "-DBUILD_DOCS=OFF"
        "-DINSTALL_MANPAGES=OFF"
        "-DWITH_OGG=ON"
        "-DOGG_ROOT=$StaticLibsDir"
    )

    Write-Success "FLAC installed"
}

function Build-Opus {
    Write-Info "========== Building opus $($Versions.opus) =========="

    $url = "https://github.com/xiph/opus/releases/download/v$($Versions.opus)/opus-$($Versions.opus).tar.gz"
    $archive = Join-Path $DownloadDir "opus-$($Versions.opus).tar.gz"
    $src = Join-Path $SrcDir "opus-$($Versions.opus)"

    Download-File $url $archive

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    Build-CMakeProject $src @(
        "-DOPUS_BUILD_PROGRAMS=OFF"
        "-DOPUS_BUILD_TESTING=OFF"
        "-DOPUS_INSTALL_PKG_CONFIG_MODULE=ON"
        "-DOPUS_STATIC_RUNTIME=ON"
    )

    Write-Success "opus installed"
}

function Build-Opusfile {
    Write-Info "========== Building opusfile $($Versions.opusfile) =========="

    $url = "https://github.com/xiph/opusfile/releases/download/v$($Versions.opusfile)/opusfile-$($Versions.opusfile).tar.gz"
    $archive = Join-Path $DownloadDir "opusfile-$($Versions.opusfile).tar.gz"
    $src = Join-Path $SrcDir "opusfile-$($Versions.opusfile)"

    Download-File $url $archive

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    # opusfile doesn't have CMake, we need to create a CMakeLists.txt
    $cmakeFile = Join-Path $src "CMakeLists.txt"

    # Always recreate to ensure correct version
    $cmakeContent = @"
cmake_minimum_required(VERSION 3.14)
project(opusfile C)

# Find opus and ogg from static_libs
find_path(OPUS_INCLUDE_DIR opus/opus.h)
find_library(OPUS_LIBRARY opus)
find_path(OGG_INCLUDE_DIR ogg/ogg.h)
find_library(OGG_LIBRARY ogg)

add_library(opusfile STATIC
    src/info.c
    src/internal.c
    src/opusfile.c
    src/stream.c
)

# Include opus headers directly (opusfile expects opus/*.h in include path)
target_include_directories(opusfile PUBLIC
    `$<BUILD_INTERFACE:`${CMAKE_CURRENT_SOURCE_DIR}/include>
    `$<INSTALL_INTERFACE:include>
)

target_include_directories(opusfile PRIVATE
    `${OPUS_INCLUDE_DIR}
    `${OPUS_INCLUDE_DIR}/opus
    `${OGG_INCLUDE_DIR}
)

target_link_libraries(opusfile PUBLIC `${OPUS_LIBRARY} `${OGG_LIBRARY})

target_compile_definitions(opusfile PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(opusfile PRIVATE /wd4244 /wd4267)
endif()

install(TARGETS opusfile ARCHIVE DESTINATION lib)
install(FILES include/opusfile.h DESTINATION include/opus)
"@
    Set-Content -Path $cmakeFile -Value $cmakeContent -Force

    Build-CMakeProject $src

    Write-Success "opusfile installed"
}

function Build-Libopusenc {
    Write-Info "========== Building libopusenc $($Versions.libopusenc) =========="

    $url = "https://github.com/xiph/libopusenc/releases/download/v$($Versions.libopusenc)/libopusenc-$($Versions.libopusenc).tar.gz"
    $archive = Join-Path $DownloadDir "libopusenc-$($Versions.libopusenc).tar.gz"
    $src = Join-Path $SrcDir "libopusenc-$($Versions.libopusenc)"

    Download-File $url $archive

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    # libopusenc doesn't provide CMake, so create a minimal static-library build.
    $cmakeFile = Join-Path $src "CMakeLists.txt"
    $cmakeContent = @"
cmake_minimum_required(VERSION 3.14)
project(libopusenc C)

find_path(OPUS_INCLUDE_DIR opus/opus.h)
find_library(OPUS_LIBRARY opus)

add_library(opusenc STATIC
    src/ogg_packer.c
    src/opus_header.c
    src/opusenc.c
    src/picture.c
    src/resample.c
    src/unicode_support.c
)

target_include_directories(opusenc PUBLIC
    `$<BUILD_INTERFACE:`${CMAKE_CURRENT_SOURCE_DIR}/include>
    `$<INSTALL_INTERFACE:include/opus>
)

target_include_directories(opusenc PRIVATE
    `${CMAKE_CURRENT_SOURCE_DIR}/src
    `${OPUS_INCLUDE_DIR}
    `${OPUS_INCLUDE_DIR}/opus
)

target_link_libraries(opusenc PUBLIC `${OPUS_LIBRARY})

target_compile_definitions(opusenc PRIVATE
    OPE_BUILD
    OUTSIDE_SPEEX
    RANDOM_PREFIX=libopusenc
    RESAMPLE_FULL_SINC_TABLE=1
    PACKAGE_NAME="libopusenc"
    PACKAGE_VERSION="$($Versions.libopusenc)"
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(opusenc PRIVATE /wd4244 /wd4267)
endif()

install(TARGETS opusenc ARCHIVE DESTINATION lib)
install(FILES include/opusenc.h DESTINATION include/opus)
"@
    Set-Content -Path $cmakeFile -Value $cmakeContent -Force

    Build-CMakeProject $src

    Write-Success "libopusenc installed"
}

function Build-Libmad {
    Write-Info "========== Building libmad $($Versions.libmad) =========="

    $url = "https://sourceforge.net/projects/mad/files/libmad/$($Versions.libmad)/libmad-$($Versions.libmad).tar.gz/download"
    $archive = Join-Path $DownloadDir "libmad-$($Versions.libmad).tar.gz"
    $src = Join-Path $SrcDir "libmad-$($Versions.libmad)"

    Download-File $url $archive

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    # Create CMakeLists.txt for libmad
    $cmakeFile = Join-Path $src "CMakeLists.txt"

    $cmakeContent = @"
cmake_minimum_required(VERSION 3.14)
project(mad C)

set(MAD_SOURCES
    bit.c
    decoder.c
    fixed.c
    frame.c
    huffman.c
    layer12.c
    layer3.c
    stream.c
    synth.c
    timer.c
    version.c
)

add_library(mad STATIC `${MAD_SOURCES})

target_include_directories(mad PUBLIC
    `$<BUILD_INTERFACE:`${CMAKE_CURRENT_SOURCE_DIR}>
    `$<INSTALL_INTERFACE:include>
)

# Use FPM_64BIT for 64-bit builds (FPM_INTEL uses x86 asm not supported on x64)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(FPM_MODE FPM_64BIT)
else()
    set(FPM_MODE FPM_DEFAULT)
endif()

target_compile_definitions(mad PRIVATE
    _CRT_SECURE_NO_WARNINGS
    `${FPM_MODE}
    HAVE_CONFIG_H
)

if(MSVC)
    target_compile_options(mad PRIVATE /wd4244 /wd4305 /wd4018)
endif()

# Generate config.h for MSVC
file(WRITE `${CMAKE_CURRENT_BINARY_DIR}/config.h "
#ifndef CONFIG_H
#define CONFIG_H

#define HAVE_STDINT_H 1
#define HAVE_LIMITS_H 1
#define HAVE_MEMORY_H 1
#define HAVE_STRING_H 1
#define STDC_HEADERS 1

#define SIZEOF_INT 4
#define SIZEOF_LONG 4
#define SIZEOF_LONG_LONG 8

#endif /* CONFIG_H */
")

target_include_directories(mad PRIVATE `${CMAKE_CURRENT_BINARY_DIR})

install(TARGETS mad ARCHIVE DESTINATION lib)
install(FILES mad.h DESTINATION include)
"@
    Set-Content -Path $cmakeFile -Value $cmakeContent -Force

    Build-CMakeProject $src

    Write-Success "libmad installed"
}

function Build-Lame {
    Write-Info "========== Building LAME $($Versions.lame) =========="

    # Use direct SourceForge download URL
    $url = "https://sourceforge.net/projects/lame/files/lame/$($Versions.lame)/lame-$($Versions.lame).tar.gz/download"
    $archive = Join-Path $DownloadDir "lame-$($Versions.lame).tar.gz"
    $src = Join-Path $SrcDir "lame-$($Versions.lame)"

    Download-File $url $archive

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    # Create CMakeLists.txt for LAME
    $cmakeFile = Join-Path $src "CMakeLists.txt"

    # Always recreate to ensure correct version
    $cmakeContent = @"
cmake_minimum_required(VERSION 3.14)
project(lame C)

# Collect source files
set(LAME_SOURCES
    libmp3lame/bitstream.c
    libmp3lame/encoder.c
    libmp3lame/fft.c
    libmp3lame/gain_analysis.c
    libmp3lame/id3tag.c
    libmp3lame/lame.c
    libmp3lame/mpglib_interface.c
    libmp3lame/newmdct.c
    libmp3lame/presets.c
    libmp3lame/psymodel.c
    libmp3lame/quantize.c
    libmp3lame/quantize_pvt.c
    libmp3lame/reservoir.c
    libmp3lame/set_get.c
    libmp3lame/tables.c
    libmp3lame/takehiro.c
    libmp3lame/util.c
    libmp3lame/vbrquantize.c
    libmp3lame/VbrTag.c
    libmp3lame/version.c
)

# Add vector sources if not on ARM
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
    list(APPEND LAME_SOURCES libmp3lame/vector/xmm_quantize_sub.c)
endif()

add_library(mp3lame STATIC `${LAME_SOURCES})

target_include_directories(mp3lame PUBLIC
    `$<BUILD_INTERFACE:`${CMAKE_CURRENT_SOURCE_DIR}/include>
    `$<BUILD_INTERFACE:`${CMAKE_CURRENT_SOURCE_DIR}/libmp3lame>
    `$<BUILD_INTERFACE:`${CMAKE_CURRENT_BINARY_DIR}>
    `$<INSTALL_INTERFACE:include>
)

target_compile_definitions(mp3lame PRIVATE
    HAVE_CONFIG_H
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_DEPRECATE
)

if(MSVC)
    target_compile_options(mp3lame PRIVATE /wd4244 /wd4305 /wd4018 /wd4267)
endif()

# Generate config.h for MSVC
file(WRITE `${CMAKE_CURRENT_BINARY_DIR}/config.h "
#ifndef CONFIG_H
#define CONFIG_H

#define HAVE_STDINT_H 1
#define HAVE_LIMITS_H 1
#define HAVE_MEMORY_H 1
#define HAVE_STRING_H 1
#define STDC_HEADERS 1

/* Define IEEE754 float type */
typedef float ieee754_float32_t;

/* Disable inline for MSVC compatibility */
#ifdef _MSC_VER
#define inline __inline
#endif

#define TAKEHIRO_IEEE754_HACK 1
#define USE_FAST_LOG 1

#endif /* CONFIG_H */
")

install(TARGETS mp3lame ARCHIVE DESTINATION lib)
install(FILES include/lame.h DESTINATION include/lame)
"@
    Set-Content -Path $cmakeFile -Value $cmakeContent -Force

    Build-CMakeProject $src

    Write-Success "LAME installed"
}

function Build-Twolame {
    Write-Info "========== Building TwoLAME $($Versions.twolame) =========="

    $url = "https://github.com/njh/twolame/releases/download/$($Versions.twolame)/twolame-$($Versions.twolame).tar.gz"
    $archive = Join-Path $DownloadDir "twolame-$($Versions.twolame).tar.gz"
    $src = Join-Path $SrcDir "twolame-$($Versions.twolame)"

    Download-File $url $archive

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    # TwoLAME uses autotools, we need to create a CMakeLists.txt
    $cmakeFile = Join-Path $src "CMakeLists.txt"

    $cmakeContent = @"
cmake_minimum_required(VERSION 3.14)
project(twolame C)

set(TWOLAME_SOURCES
    libtwolame/ath.c
    libtwolame/availbits.c
    libtwolame/bitbuffer.c
    libtwolame/crc.c
    libtwolame/dab.c
    libtwolame/encode.c
    libtwolame/energy.c
    libtwolame/fft.c
    libtwolame/get_set.c
    libtwolame/mem.c
    libtwolame/psycho_0.c
    libtwolame/psycho_1.c
    libtwolame/psycho_2.c
    libtwolame/psycho_3.c
    libtwolame/psycho_4.c
    libtwolame/psycho_n1.c
    libtwolame/subband.c
    libtwolame/twolame.c
    libtwolame/util.c
)

add_library(twolame STATIC `${TWOLAME_SOURCES})

target_include_directories(twolame PUBLIC
    `$<BUILD_INTERFACE:`${CMAKE_CURRENT_SOURCE_DIR}/libtwolame>
    `$<INSTALL_INTERFACE:include>
)

target_compile_definitions(twolame PRIVATE
    LIBTWOLAME_STATIC
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_DEPRECATE
    HAVE_CONFIG_H
)

if(MSVC)
    target_compile_options(twolame PRIVATE /wd4244 /wd4305 /wd4018 /wd4267)
endif()

# Generate config.h for MSVC
file(WRITE `${CMAKE_CURRENT_BINARY_DIR}/config.h "
#ifndef CONFIG_H
#define CONFIG_H

#define HAVE_STDINT_H 1
#define HAVE_LIMITS_H 1
#define HAVE_MEMORY_H 1
#define HAVE_STRING_H 1
#define STDC_HEADERS 1

#define PACKAGE \"twolame\"
#define PACKAGE_NAME \"TwoLAME\"
#define PACKAGE_VERSION \"$($Versions.twolame)\"
#define VERSION \"$($Versions.twolame)\"

#ifdef _MSC_VER
#define inline __inline
#endif

#endif /* CONFIG_H */
")

target_include_directories(twolame PRIVATE `${CMAKE_CURRENT_BINARY_DIR})

install(TARGETS twolame ARCHIVE DESTINATION lib)
install(FILES libtwolame/twolame.h DESTINATION include)
"@
    Set-Content -Path $cmakeFile -Value $cmakeContent -Force

    Build-CMakeProject $src

    Write-Success "TwoLAME installed"
}

function Build-Libid3tag {
    Write-Info "========== Building libid3tag $($Versions.libid3tag) =========="

    $url = "https://sourceforge.net/projects/mad/files/libid3tag/$($Versions.libid3tag)/libid3tag-$($Versions.libid3tag).tar.gz/download"
    $archive = Join-Path $DownloadDir "libid3tag-$($Versions.libid3tag).tar.gz"
    $src = Join-Path $SrcDir "libid3tag-$($Versions.libid3tag)"

    Download-File $url $archive

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    # Create CMakeLists.txt for libid3tag
    $cmakeFile = Join-Path $src "CMakeLists.txt"

    $cmakeContent = @"
cmake_minimum_required(VERSION 3.14)
project(id3tag C)

set(ID3TAG_SOURCES
    compat.c
    crc.c
    debug.c
    field.c
    file.c
    frame.c
    frametype.c
    genre.c
    latin1.c
    parse.c
    render.c
    tag.c
    ucs4.c
    utf16.c
    utf8.c
    util.c
    version.c
)

add_library(id3tag STATIC `${ID3TAG_SOURCES})

# Find zlib
find_package(ZLIB REQUIRED)

target_include_directories(id3tag PUBLIC
    `$<BUILD_INTERFACE:`${CMAKE_CURRENT_SOURCE_DIR}>
    `$<INSTALL_INTERFACE:include>
)

target_link_libraries(id3tag PRIVATE ZLIB::ZLIB)

target_compile_definitions(id3tag PRIVATE
    _CRT_SECURE_NO_WARNINGS
    HAVE_CONFIG_H
)

if(MSVC)
    target_compile_options(id3tag PRIVATE /wd4244 /wd4267)
endif()

# Generate config.h for MSVC
file(WRITE `${CMAKE_CURRENT_BINARY_DIR}/config.h "
#ifndef CONFIG_H
#define CONFIG_H

#define HAVE_STDINT_H 1
#define HAVE_LIMITS_H 1
#define HAVE_MEMORY_H 1
#define HAVE_STRING_H 1
#define STDC_HEADERS 1
#define HAVE_ZLIB_H 1
#define HAVE_FTRUNCATE 1

#ifdef _MSC_VER
#define ftruncate _chsize
#endif

#endif /* CONFIG_H */
")

target_include_directories(id3tag PRIVATE `${CMAKE_CURRENT_BINARY_DIR})

install(TARGETS id3tag ARCHIVE DESTINATION lib)
install(FILES id3tag.h DESTINATION include)
"@
    Set-Content -Path $cmakeFile -Value $cmakeContent -Force

    Build-CMakeProject $src

    Write-Success "libid3tag installed"
}

function Build-Wavpack {
    Write-Info "========== Building WavPack $($Versions.wavpack) =========="

    $url = "https://github.com/dbry/WavPack/releases/download/$($Versions.wavpack)/wavpack-$($Versions.wavpack).tar.xz"
    $archive = Join-Path $DownloadDir "wavpack-$($Versions.wavpack).tar.xz"
    $src = Join-Path $SrcDir "wavpack-$($Versions.wavpack)"

    Download-File $url $archive

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    Build-CMakeProject $src @(
        "-DWAVPACK_BUILD_PROGRAMS=OFF"
        "-DWAVPACK_BUILD_DOCS=OFF"
        "-DBUILD_TESTING=OFF"
    )

    Write-Success "WavPack installed"
}

function Build-Libsndfile {
    Write-Info "========== Building libsndfile $($Versions.libsndfile) =========="

    $url = "https://github.com/libsndfile/libsndfile/releases/download/$($Versions.libsndfile)/libsndfile-$($Versions.libsndfile).tar.xz"
    $archive = Join-Path $DownloadDir "libsndfile-$($Versions.libsndfile).tar.xz"
    $src = Join-Path $SrcDir "libsndfile-$($Versions.libsndfile)"

    Download-File $url $archive

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    Build-CMakeProject $src @(
        "-DBUILD_PROGRAMS=OFF"
        "-DBUILD_EXAMPLES=OFF"
        "-DBUILD_TESTING=OFF"
        "-DENABLE_EXTERNAL_LIBS=ON"
        "-DENABLE_MPEG=OFF"
        "-DCMAKE_C_FLAGS=/DFLAC__NO_DLL"
    )

    Write-Success "libsndfile installed"
}

function Find-MsysBash {
    $candidates = @(
        "C:\msys64\usr\bin\bash.exe",
        "C:\msys32\usr\bin\bash.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return $null
}

# Workaround for an FFmpeg build that MSVC rejects.
#
# cbs_type_table in libavcodec/cbs.c is populated by #ifdefs, one entry per
# coded-bitstream type. None of the codecs SoX enables brings one in, so with
# --disable-everything the initialiser is empty; a zero-sized array is a GNU
# extension that MSVC refuses outright, and the build stops there.
#
# The fix is the smallest one that keeps the semantics: append a NULL sentinel
# so the array has an element, and terminate the loop on that sentinel instead
# of on FF_ARRAY_ELEMS, which would now count one entry too many. Both edits
# are idempotent, so re-running over an already-patched tree is harmless, and
# every step throws rather than continuing if the source does not look as
# expected -- an FFmpeg upgrade that reshapes this file must be noticed here,
# not miscompiled.
function Patch-FfmpegMsvcEmptyCbsTable($src) {
    $cbsPath = Join-Path $src "libavcodec\cbs.c"
    $lines = [Collections.Generic.List[string]](Get-Content -LiteralPath $cbsPath)
    $tableDeclaration = "static const CodedBitstreamType *const cbs_type_table[] = {"
    $oldLoop = "    for (i = 0; i < FF_ARRAY_ELEMS(cbs_type_table); i++) {"
    $newLoop = "    for (i = 0; cbs_type_table[i]; i++) {"
    $tableStart = $lines.IndexOf($tableDeclaration)

    if ($tableStart -lt 0) {
        throw "FFmpeg CBS compatibility patch failed: table declaration not found"
    }

    $tableEnd = -1
    for ($i = $tableStart + 1; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -eq "};") {
            $tableEnd = $i
            break
        }
    }

    if ($tableEnd -lt 0) {
        throw "FFmpeg CBS compatibility patch failed: table end not found"
    }

    if ($lines[$tableEnd - 1] -ne "    NULL,") {
        $lines.Insert($tableEnd, "    NULL,")
    }

    $oldLoopIndex = $lines.IndexOf($oldLoop)
    if ($oldLoopIndex -ge 0) {
        $lines[$oldLoopIndex] = $newLoop
    }
    elseif ($lines.IndexOf($newLoop) -lt 0) {
        throw "FFmpeg CBS compatibility patch failed: table loop not found"
    }

    [IO.File]::WriteAllLines($cbsPath, $lines, [Text.UTF8Encoding]::new($false))
    Write-Info "Applied FFmpeg MSVC empty CBS table compatibility patch"
}

# FFmpeg has no MSVC project: its build system is autoconf-shaped and needs a
# POSIX shell, so this drives configure and make under MSYS2 bash while the
# actual compiler stays MSVC through --toolchain=msvc. That is why MSYS2 is a
# requirement of this script and of nothing else in it.
#
# MSYS2_PATH_TYPE=inherit is what makes it work: without it the MSYS2 shell
# replaces PATH with its own and cl.exe, from the Developer PowerShell this
# script must run in, disappears. Paths cross the boundary through cygpath,
# and the environment is passed by variable rather than interpolated into the
# script text so that a path containing a space or a quote cannot break it.
#
# The component selection matches build_static_libs.sh exactly; the two must
# stay in step, since they produce the same library for the same SoX sources.
function Build-Ffmpeg {
    Write-Info "========== Building FFmpeg $($Versions.ffmpeg) =========="

    $msysBash = Find-MsysBash
    if (-not $msysBash) {
        throw "MSYS2 bash not found. Install MSYS2 in C:\msys64 to build FFmpeg statically."
    }

    $url = "https://ffmpeg.org/releases/ffmpeg-$($Versions.ffmpeg).tar.xz"
    $archive = Join-Path $DownloadDir "ffmpeg-$($Versions.ffmpeg).tar.xz"
    $src = Join-Path $SrcDir "ffmpeg-$($Versions.ffmpeg)"
    $ffmpegBuild = Join-Path $src "build-static"

    Download-File $url $archive

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    Patch-FfmpegMsvcEmptyCbsTable $src

    New-Item -ItemType Directory -Path $ffmpegBuild -Force | Out-Null

    $env:SOX_FFMPEG_SRC = $src
    $env:SOX_FFMPEG_BUILD = $ffmpegBuild
    $env:SOX_STATIC_PREFIX = $StaticLibsDir
    $env:SOX_BUILD_JOBS = $Jobs
    $env:SOX_FFMPEG_TARGET_ARCH = $env:VSCMD_ARG_TGT_ARCH
    $previousMsys2PathType = $env:MSYS2_PATH_TYPE
    $env:MSYS2_PATH_TYPE = "inherit"

    $buildScript = @'
set -e
command -v cl >/dev/null ||
  { echo "MSVC cl.exe is not available; run from a Visual Studio Developer PowerShell." >&2; exit 1; }
command -v make >/dev/null ||
  { echo "GNU make is required in MSYS2." >&2; exit 1; }
command -v nasm >/dev/null ||
  { echo "NASM is required in MSYS2." >&2; exit 1; }
command -v cmp >/dev/null ||
  { echo "cmp (diffutils) is required in MSYS2." >&2; exit 1; }
command -v pkg-config >/dev/null ||
  { echo "pkg-config (pkgconf) is required in MSYS2." >&2; exit 1; }

src="$(cygpath -u "$SOX_FFMPEG_SRC")"
build="$(cygpath -u "$SOX_FFMPEG_BUILD")"
prefix="$(cygpath -u "$SOX_STATIC_PREFIX")"

set --
if [ "$SOX_FFMPEG_TARGET_ARCH" = "arm64" ]; then
  set -- --arch=aarch64 --disable-asm
fi

cd "$build"
"$src/configure" \
  --toolchain=msvc \
  --prefix="$prefix" \
  --disable-shared \
  --enable-static \
  --disable-programs \
  --disable-doc \
  --disable-debug \
  --disable-network \
  --disable-autodetect \
  --disable-everything \
  --enable-avcodec \
  --enable-avformat \
  --enable-avutil \
  --disable-swresample \
  --disable-avdevice \
  --disable-avfilter \
  --disable-swscale \
  --enable-decoder=aac,aac_latm,alac,ac3,eac3,dca,mlp,truehd \
  --enable-encoder=aac,alac,ac3,eac3,dca,mlp,truehd \
  --enable-parser=aac,aac_latm,ac3,dca,mlp \
  --enable-demuxer=mov \
  --enable-muxer=ipod \
  --enable-protocol=file \
  "$@"
make -j"$SOX_BUILD_JOBS"
make install
'@

    try {
        & $msysBash -lc $buildScript
        if ($LASTEXITCODE -ne 0) {
            throw "FFmpeg static build failed"
        }
    }
    finally {
        Remove-Item Env:SOX_FFMPEG_SRC -ErrorAction SilentlyContinue
        Remove-Item Env:SOX_FFMPEG_BUILD -ErrorAction SilentlyContinue
        Remove-Item Env:SOX_STATIC_PREFIX -ErrorAction SilentlyContinue
        Remove-Item Env:SOX_BUILD_JOBS -ErrorAction SilentlyContinue
        Remove-Item Env:SOX_FFMPEG_TARGET_ARCH -ErrorAction SilentlyContinue
        if ($null -eq $previousMsys2PathType) {
            Remove-Item Env:MSYS2_PATH_TYPE -ErrorAction SilentlyContinue
        }
        else {
            $env:MSYS2_PATH_TYPE = $previousMsys2PathType
        }
    }

    Write-Success "FFmpeg installed"
}

function Test-StaticDependencies($binary) {
    $dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if (-not $dumpbin) {
        throw "dumpbin.exe not found; run from a Visual Studio Developer PowerShell."
    }

    Write-Info "Verifying that audio codec dependencies are static..."
    $dependencies = & $dumpbin.Source /dependents $binary 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to inspect executable dependencies with dumpbin"
    }

    # A deny list rather than an allow list: the system DLLs a release build
    # legitimately imports vary with the toolchain and the Windows version,
    # while the codec libraries that must not appear are exactly the ones this
    # script builds. msys- catches an FFmpeg accidentally linked against the
    # MSYS2 runtime, which would run here and nowhere else.
    $forbidden = "avcodec|avformat|avutil|swresample|FLAC|ogg|vorbis|opus|sndfile|mp3lame|twolame|wavpack|opencore-amr|png|magic|libao|msys-"
    $unexpected = $dependencies | Select-String -Pattern $forbidden
    if ($unexpected) {
        throw "Unexpected dynamic dependency detected:`n$($unexpected -join [Environment]::NewLine)"
    }

    Write-Success "Audio codec dependencies are statically linked"
}

function Test-SoxExecutable($binary) {
    Write-Info "Running sox.exe --version smoke test..."
    $process = Start-Process -FilePath $binary -ArgumentList "--version" -NoNewWindow -PassThru

    try {
        if (-not $process.WaitForExit(15000)) {
            $process.Kill($true)
            throw "sox.exe --version timed out"
        }
        if ($process.ExitCode -ne 0) {
            throw "sox.exe --version failed with exit code $($process.ExitCode)"
        }
    }
    finally {
        $process.Dispose()
    }

    Write-Success "sox.exe smoke test passed"

    if ($EnableVulkan) {
        $help = & $binary --help 2>&1
        if ($LASTEXITCODE -ne 0 -or -not ($help | Select-String -SimpleMatch "--vulkan")) {
            throw "sox.exe help does not expose the enabled Vulkan backend"
        }
        Write-Success "sox.exe Vulkan help check passed"
    }
}

# ------------------------------------------------------------------------------
# Help function
# ------------------------------------------------------------------------------

function Show-Help {
    Write-Host "Usage: .\build_static_libs.ps1 [OPTIONS]"
    Write-Host ""
    Write-Host "Build SoX with statically linked audio codec libraries."
    Write-Host ""
    Write-Host "General Options:"
    Write-Host "  -Jobs N             Number of parallel build jobs (default: auto)"
    Write-Host "  -Clean              Remove build and output directories"
    Write-Host "  -KeepBuild          Keep downloaded sources, static libraries, and build directories"
    Write-Host "  -Help               Show this help message"
    Write-Host ""
    Write-Host "Codec Options (all enabled by default, use to exclude):"
    Write-Host "  -NoOgg              Exclude OGG/Vorbis support"
    Write-Host "  -NoFlac             Exclude FLAC support"
    Write-Host "  -NoOpus             Exclude Opus support"
    Write-Host "  -NoMp3              Exclude MP3 support (libmad + LAME)"
    Write-Host "  -NoMp2              Exclude MP2 support (TwoLAME)"
    Write-Host "  -NoWavpack          Exclude WavPack support"
    Write-Host "  -NoSndfile          Exclude libsndfile support"
    Write-Host "  -NoId3tag           Exclude ID3 tag support"
    Write-Host "  -NoPng              Exclude PNG spectrogram support"
    Write-Host "  -NoFfmpeg           Exclude FFmpeg codec and container support"
    Write-Host "  -NoVulkan           Exclude the Windows Vulkan FIR and DSD backends"
    Write-Host ""
    Write-Host "Audio Driver Options:"
    Write-Host "  Default drivers: waveaudio (Windows native)"
    Write-Host ""
    Write-Host "  -NoWaveaudio        Exclude waveaudio driver (Windows default)"
    Write-Host ""
    Write-Host "Examples:"
    Write-Host "  .\build_static_libs.ps1                    # Build with all codecs"
    Write-Host "  .\build_static_libs.ps1 -NoMp2 -NoId3tag   # Exclude MP2 and ID3 tag"
    Write-Host "  .\build_static_libs.ps1 -NoVulkan           # Build without Vulkan SDK"
    Write-Host ""
    exit 0
}

# ------------------------------------------------------------------------------
# Main script
# ------------------------------------------------------------------------------

function Main {
    Write-Host ""
    Write-Host "=============================================="
    Write-Host "  SoX Static Libraries Build Script (Windows)"
    Write-Host "=============================================="
    Write-Host ""

    if ($Help) {
        Show-Help
    }

    if ($Clean) {
        Write-Info "Cleaning build directories..."
        if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
        if (Test-Path $StaticLibsDir) { Remove-Item -Recurse -Force $StaticLibsDir }
        if (Test-Path $SoxBuildDir) { Remove-Item -Recurse -Force $SoxBuildDir }
        if (Test-Path $OutputDir) { Remove-Item -Recurse -Force $OutputDir }
        Write-Success "Clean complete"
        exit 0
    }

    # Check required tools
    Write-Info "Checking required tools..."

    if ($EnableFfmpeg -and $PSVersionTable.PSVersion.Major -lt 7) {
        Write-Err "PowerShell 7 or later is required for the FFmpeg build. Run this script with pwsh."
        exit 1
    }

    $script:CMakePath = Find-CMake
    if (-not $script:CMakePath) {
        Write-Err "CMake not found. Please install CMake or Visual Studio with C++ tools."
        exit 1
    }
    Write-Info "Found CMake: $script:CMakePath"

    if ($EnableVulkan) {
        if (-not $env:VULKAN_SDK) {
            Write-Err "VULKAN_SDK is required for the Vulkan/VkFFT build."
            exit 1
        }
        $glslc = Get-Command glslc.exe -ErrorAction SilentlyContinue
        if (-not $glslc) {
            $sdkGlslc = Join-Path $env:VULKAN_SDK "Bin\glslc.exe"
            if (Test-Path $sdkGlslc) {
                $glslc = Get-Item $sdkGlslc
            }
        }
        if (-not $glslc) {
            Write-Err "Vulkan SDK with glslc is required. Install it or use -NoVulkan."
            exit 1
        }
        Write-Info "Found glslc: $($glslc.FullName)"

        $glslangHeader = Join-Path $env:VULKAN_SDK "Include\glslang\Include\glslang_c_interface.h"
        $glslangRuntime = Join-Path $env:VULKAN_SDK "Bin\glslang.dll"
        if (-not (Test-Path $glslangHeader) -or -not (Test-Path $glslangRuntime)) {
            Write-Err "Vulkan SDK with glslang C headers and glslang.dll is required."
            exit 1
        }
    }

    if (-not (Test-Command "tar")) {
        Write-Warn "tar not found in PATH, will try to use built-in Windows tar"
    }

    # Create directories
    New-Item -ItemType Directory -Path $DownloadDir -Force | Out-Null
    New-Item -ItemType Directory -Path $SrcDir -Force | Out-Null
    New-Item -ItemType Directory -Path $StaticLibsDir -Force | Out-Null

    Write-Info "Build directory: $BuildDir"
    Write-Info "Output directory: $StaticLibsDir"
    Write-Info "Parallel jobs: $Jobs"
    Write-Host ""

    # Display configuration
    Write-Host "Configuration:"
    Write-Host "  Codecs:"
    Write-Host "    OGG/Vorbis: $EnableOgg"
    Write-Host "    FLAC:       $EnableFlac"
    Write-Host "    Opus:       $EnableOpus"
    Write-Host "    MP3:        $EnableMp3"
    Write-Host "    MP2:        $EnableMp2"
    Write-Host "    WavPack:    $EnableWavpack"
    Write-Host "    Sndfile:    $EnableSndfile"
    Write-Host "    ID3 Tag:    $EnableId3tag"
    Write-Host "    PNG:        $EnablePng"
    Write-Host "    FFmpeg:     $EnableFfmpeg"
    Write-Host "  Acceleration:"
    Write-Host "    Vulkan DSD: $EnableVulkan"
    Write-Host "  Audio Drivers:"
    Write-Host "    Waveaudio:  $EnableWaveaudio"
    Write-Host ""

    # Set environment for CMake to find installed libraries
    $env:CMAKE_PREFIX_PATH = $StaticLibsDir
    $env:PKG_CONFIG_PATH = "$StaticLibsDir\lib\pkgconfig"

    # Build libraries in dependency order
    # Level 0: No dependencies (always needed)
    Build-Zlib
    if ($EnableVulkan) {
        Install-VkFFT
    }

    # Codec libraries
    if ($EnableOgg -or $EnableFlac -or $EnableOpus) {
        Build-Libogg
    }

    if ($EnableOgg) {
        Build-Libvorbis
    }

    if ($EnableFlac) {
        Build-Flac
    }

    if ($EnableOpus) {
        Build-Opus
        Build-Opusfile
        Build-Libopusenc
    }

    if ($EnableFfmpeg) {
        Build-Ffmpeg
    }

    if ($EnableMp3) {
        Build-Libmad
        Build-Lame
    }

    if ($EnableMp2) {
        Build-Twolame
    }

    if ($EnableWavpack) {
        Build-Wavpack
    }

    if ($EnableId3tag) {
        Build-Libid3tag
    }

    if ($EnablePng) {
        Build-Libpng
    }

    if ($EnableSndfile) {
        Build-Libsndfile
    }

    Write-Host ""
    Write-Success "All libraries built successfully!"
    Write-Host ""

    # Build SoX
    Write-Info "========== Building SoX =========="
    New-Item -ItemType Directory -Path $SoxBuildDir -Force | Out-Null
    Push-Location $SoxBuildDir

    try {
        $cmakeArgs = @(
            $ScriptDir
            "-DCMAKE_BUILD_TYPE=Release"
            "-DBUILD_SHARED_LIBS=OFF"
            "-DBUILD_STATIC_EXECUTABLE=ON"
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
            "-DSOX_REQUIRE_STATIC_DEPENDENCIES=ON"
            "-DCMAKE_PREFIX_PATH=$StaticLibsDir"
            "-DSTATIC_LIBS_DIR=$StaticLibsDir"
            "-DWITH_OPENMP=ON"
            "-DSOX_REQUIRE_OPENMP=ON"
            "-DWITH_VULKAN=$($EnableVulkan.ToString().ToUpperInvariant())"
            "-DWITH_FFMPEG_CODECS=$($EnableFfmpeg.ToString().ToUpperInvariant())"
            "-DWITH_FFMPEG_FORMATS=$($EnableFfmpeg.ToString().ToUpperInvariant())"
        )

        # Add audio driver options
        if ($EnableWaveaudio) {
            $cmakeArgs += "-DWITH_WAVEAUDIO=ON"
        } else {
            $cmakeArgs += "-DWITH_WAVEAUDIO=OFF"
        }

        # libao is not supported on Windows - always disable
        $cmakeArgs += "-DWITH_AO=OFF"

        & $script:CMakePath @cmakeArgs

        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

        & $script:CMakePath --build . --config Release --parallel $Jobs
        if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }
    }
    finally {
        Pop-Location
    }

    Write-Success "SoX built successfully!"

    $soxExe = Join-Path $SoxBuildDir "src\Release\sox.exe"
    if (-not (Test-Path $soxExe)) {
        $soxExe = Join-Path $SoxBuildDir "src\sox.exe"
    }
    if (-not (Test-Path $soxExe)) {
        throw "sox.exe not found after build"
    }
    Test-StaticDependencies $soxExe
    Test-SoxExecutable $soxExe

    # Copy binary and non-codec runtime dependencies to output directory
    Write-Info "Copying SoX and runtime dependencies to output directory..."
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

    if (Test-Path $soxExe) {
        Copy-Item $soxExe $OutputDir
    } else {
        Write-Err "sox.exe not found!"
        exit 1
    }

    if ($EnableVulkan) {
        $glslangDll = Join-Path (Split-Path -Parent $soxExe) "glslang.dll"
        if (-not (Test-Path $glslangDll)) {
            throw "glslang.dll was not copied beside sox.exe by the Vulkan build"
        }
        Copy-Item $glslangDll $OutputDir
    }

    Write-Success "Binary copied to: $OutputDir\sox.exe"
    if ($EnableVulkan) {
        Write-Success "Vulkan runtime copied to: $OutputDir\glslang.dll"
    }

    if ($KeepBuild) {
        Write-Info "Keeping build directories for incremental rebuilds."
    } else {
        Write-Info "Cleaning up temporary build files..."
        if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
        if (Test-Path $StaticLibsDir) { Remove-Item -Recurse -Force $StaticLibsDir }
        if (Test-Path $SoxBuildDir) { Remove-Item -Recurse -Force $SoxBuildDir }
        Write-Success "Cleanup complete!"
    }

    Write-Host ""
    Write-Host "=============================================="
    Write-Success "Build complete!"
    Write-Host "=============================================="
    Write-Host ""
    Write-Host "Output binary: $OutputDir\sox.exe"
    if ($EnableVulkan) {
        Write-Host "Vulkan runtime: $OutputDir\glslang.dll"
    }
    Write-Host ""
}

# Run main function
Main
