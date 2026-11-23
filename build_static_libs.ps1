#
# SoX Static Libraries Build Script for Windows
# Downloads and compiles all optional libraries as static libraries
#
# Usage: .\build_static_libs.ps1 [OPTIONS]
#
# Requirements: Visual Studio 2019 or later, PowerShell 7, CMake, Git, MSYS2, NASM,
# GNU patch
#

param(
    [int]$Jobs = $env:NUMBER_OF_PROCESSORS,
    [switch]$Clean,
    [switch]$KeepBuild,
    [switch]$Help,
    [switch]$Library,

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
    [switch]$NoApe,
    [switch]$NoMusepack,

    # Optional acceleration backend (disabled by default)
    [switch]$Vulkan,

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
# zlib's CMake build names its MSVC static library libzs.lib, a name CMake's
# FindZLIB does not search for, so the libraries that link zlib get its path.
$ZlibLibrary = Join-Path $StaticLibsDir "lib\libzs.lib"

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
$EnableApe = -not $NoApe
$EnableMusepack = -not $NoMusepack
$EnableVulkan = $Vulkan

# ------------------------------------------------------------------------------
# AUDIO DRIVER OPTIONS
# Windows defaults: waveaudio only (libao not supported on Windows)
# ------------------------------------------------------------------------------
$EnableWaveaudio = -not $NoWaveaudio

# Library versions
$Versions = @{
    zlib = "1.4"
    libpng = "1.6.59"
    libogg = "1.3.6"
    libvorbis = "1.3.7"
    flac = "1.5.0"
    opus = "1.6.1"
    lame = "4.0"
    twolame = "0.4.0"
    wavpack = "5.9.0"
    libsndfile = "1.2.2"
    libmad = "0.16.5"
    libid3tag = "0.15.1b"
    ffmpeg = "9.0.2"
    mac = "13.47"
    musepack = "r475"
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

# Windows bsdtar lacks liblzma and pipes .tar.xz through an external xz, and
# that pipe deadlocks; xz is therefore run on its own.
function Find-Xz {
    $onPath = Get-Command xz.exe -ErrorAction SilentlyContinue
    if ($onPath) {
        return $onPath.Source
    }

    $candidates = @(
        "C:\msys64\mingw64\bin\xz.exe",
        "C:\msys32\mingw32\bin\xz.exe",
        "C:\msys64\usr\bin\xz.exe",
        "C:\msys32\usr\bin\xz.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return $null
}

# Windows bsdtar reads entry names in the ANSI code page and exits with 1 on
# the UTF-8 names in the FLAC tarball, so GNU tar from MSYS2 comes first; the
# tar.exe on PATH is the Windows one.
function Find-Tar {
    $candidates = @(
        "C:\msys64\usr\bin\tar.exe",
        "C:\msys32\usr\bin\tar.exe",
        "C:\Program Files\Git\usr\bin\tar.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return "C:\Windows\System32\tar.exe"
}

# GNU patch ships with both MSYS2 and Git for Windows.
function Find-Patch {
    $candidates = @(
        "C:\msys64\usr\bin\patch.exe",
        "C:\msys32\usr\bin\patch.exe",
        "C:\Program Files\Git\usr\bin\patch.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    $onPath = Get-Command patch.exe -ErrorAction SilentlyContinue
    if ($onPath) {
        return $onPath.Source
    }

    return $null
}

# Global patch path
$script:PatchPath = $null

# Applies, in name order, the patches of patches\<dependency>\ and then those
# of patches\<dependency>\msvc\ to an extracted source tree, with patch -p1.
# A patch the tree already applies to in reverse is skipped, so a rebuild over
# sources patched by an earlier run goes on.
function Apply-Patches($src, $dependency) {
    $patchRoot = Join-Path $ScriptDir "patches"
    $directories = @(
        (Join-Path $patchRoot $dependency)
        (Join-Path $patchRoot (Join-Path $dependency "msvc"))
    )

    foreach ($directory in $directories) {
        if (-not (Test-Path -LiteralPath $directory)) {
            continue
        }

        $patchFiles = Get-ChildItem -LiteralPath $directory -Filter "*.patch" -File | Sort-Object Name
        foreach ($patchFile in $patchFiles) {
            $name = $patchFile.FullName.Substring($patchRoot.Length + 1)

            # The reverse dry run fails on a tree not patched yet: under Stop,
            # its redirected stderr would end the script on Windows PowerShell
            # and 7.0/7.1.
            $ErrorActionPreference = "Continue"
            & $script:PatchPath -p1 -R -s -f --dry-run -d $src -i $patchFile.FullName *> $null
            if ($LASTEXITCODE -eq 0) {
                Write-Info "Patch already applied: $name"
                continue
            }

            & $script:PatchPath -p1 -N -s -f -d $src -i $patchFile.FullName
            if ($LASTEXITCODE -ne 0) {
                throw "Patch failed: $name"
            }
            Write-Info "Applied patch: $name"
        }
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
        $xz = Find-Xz
        if (-not $xz) {
            throw "xz not found: install it with MSYS2 (pacman -S mingw-w64-x86_64-xz) or put xz.exe on PATH"
        }

        Push-Location $destWin
        try {
            # Two steps, through a file: Start-Process redirects bytes, where
            # PowerShell's > operator would re-encode the .tar as text.
            $tarPath = Join-Path $destWin ([System.IO.Path]::GetFileNameWithoutExtension($archiveWin))
            $proc = Start-Process -FilePath $xz -ArgumentList "-d", "-c", "`"$archiveWin`"" `
                -RedirectStandardOutput $tarPath -NoNewWindow -Wait -PassThru
            if ($proc.ExitCode -ne 0) {
                throw "xz failed to decompress $archiveWin"
            }

            # The bare name: GNU tar reads the colon in "C:\" as a remote host.
            & "$(Find-Tar)" -xf ([System.IO.Path]::GetFileName($tarPath))
            if ($LASTEXITCODE -ne 0) {
                throw "tar failed to extract $tarPath"
            }
            Remove-Item $tarPath -ErrorAction SilentlyContinue
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

# Stops the build unless the archive has the expected SHA-256 digest.
function Test-Sha256($archive, $expected) {
    $actual = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected) {
        throw "$(Split-Path -Leaf $archive) checksum mismatch: $actual"
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
    Test-Sha256 $archive $expectedSha256

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
        "-DZLIB_BUILD_SHARED=OFF"
        "-DZLIB_BUILD_TESTING=OFF"
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
        "-DZLIB_LIBRARY=$ZlibLibrary"
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

    $url = "https://downloads.xiph.org/releases/opus/opus-$($Versions.opus).tar.gz"
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

function Build-Libmad {
    Write-Info "========== Building libmad $($Versions.libmad) =========="

    $url = "https://codeberg.org/tenacityteam/libmad/releases/download/$($Versions.libmad)/libmad-$($Versions.libmad).tar.gz"
    $archive = Join-Path $DownloadDir "libmad-$($Versions.libmad).tar.gz"
    $src = Join-Path $SrcDir "mad-$($Versions.libmad)-source"
    # The digest the Tenacity team publishes with the release, kept identical
    # to LIBMAD_SHA256 in build_static_libs.sh.
    $expectedSha256 = "f401b2ccf49c26ad0c3a6ef0179b1cbbff2406338b0603bb2033e6dcaf706114"

    Download-File $url $archive
    Test-Sha256 $archive $expectedSha256

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }

    Build-CMakeProject $src @(
        "-DEXAMPLE=OFF"
        "-DOPTIMIZE=ACCURACY"
    )

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

    Apply-Patches $src "lame"

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

    Apply-Patches $src "twolame"

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

    Apply-Patches $src "libid3tag"

    Build-CMakeProject $src @(
        "-DZLIB_LIBRARY=$ZlibLibrary"
    )

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
        "-DWAVPACK_INSTALL_DOCS=OFF"
        "-DBUILD_TESTING=OFF"
    )

    Write-Success "WavPack installed"
}

# Monkey's Audio SDK: libMAC, the C++ library SoX reads and writes APE with.
#
# The zip has no top-level directory, so it is unpacked into one of its own.
# patches\monkeysaudio\ drops MD5.cpp and MACDll.cpp from the build, and its
# msvc\ folder the GCC warning options cl rejects. The frame decoder and
# encoder headers are not installed by the SDK, so all headers are copied
# beside the public ones. The C++ runtime is static (Get-CMakeFlags).
function Build-MonkeysAudio {
    Write-Info "========== Building Monkey's Audio SDK $($Versions.mac) =========="

    $archiveName = "MAC_$($Versions.mac.Replace('.', ''))_SDK.zip"
    $url = "https://monkeysaudio.com/files/$archiveName"
    $archive = Join-Path $DownloadDir $archiveName
    $src = Join-Path $SrcDir "MAC_SDK-$($Versions.mac)"
    $expectedSha256 = "6aec852ecf38ef91c0b323b83356f16cc6d485f1cc7a338151e60995a87e8166"

    Download-File $url $archive
    Test-Sha256 $archive $expectedSha256

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $src
    }

    Apply-Patches $src "monkeysaudio"

    Build-CMakeProject $src @(
        "-DBUILD_SHARED=OFF"
        "-DBUILD_UTIL=OFF"
    )

    $headerDir = Join-Path $StaticLibsDir "include\MAC"
    New-Item -ItemType Directory -Path $headerDir -Force | Out-Null
    Copy-Item (Join-Path $src "Source\Shared\*.h") $headerDir -Force
    Copy-Item (Join-Path $src "Source\MACLib\*.h") $headerDir -Force

    Write-Success "Monkey's Audio SDK installed"
}

# Musepack: libmpcdec reads SV7 and SV8, libmpcenc and libmpcpsy write SV8.
#
# patches\musepack\ adds sox-libraries\CMakeLists.txt, which builds the three
# libraries without upstream's command-line tools, and resets the encoder
# state that r475 carries from one file to the next in the same process; its
# msvc\ folder renames the SV8 log2 tables, which clash with the UCRT.
function Build-Musepack {
    Write-Info "========== Building Musepack $($Versions.musepack) =========="

    $url = "https://files.musepack.net/source/musepack_src_$($Versions.musepack).tar.gz"
    $archive = Join-Path $DownloadDir "musepack_src_$($Versions.musepack).tar.gz"
    $src = Join-Path $SrcDir "musepack_src_$($Versions.musepack)"
    $libraries = Join-Path $src "sox-libraries"
    $expectedSha256 = "a4b1742f997f83e1056142d556a8c20845ba764b70365ff9ccf2e3f81c427b2b"

    Download-File $url $archive
    Test-Sha256 $archive $expectedSha256

    if (-not (Test-Path $src)) {
        Extract-Archive $archive $SrcDir
    }
    Apply-Patches $src "musepack"

    Build-CMakeProject $libraries

    Write-Success "Musepack installed"
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

# FFmpeg's build needs a POSIX shell, so configure and make run under MSYS2
# bash while --toolchain=msvc keeps cl as the compiler; FFmpeg is the only
# reason this script needs MSYS2.
#
# MSYS2_PATH_TYPE=inherit keeps the Developer PowerShell PATH, and cl.exe with
# it. Paths are converted with cygpath and passed through environment
# variables, so spaces or quotes cannot break the script text.  The prefix is
# written as C:/..., because it ends up in libavcodec.pc, where a pkg-config
# outside MSYS2 and CMake must read it.
#
# The component selection must match build_static_libs.sh: libavcodec's
# codecs and parsers only, since the containers are SoX's own.
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

    Apply-Patches $src "ffmpeg"

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
prefix="$(cygpath -m "$SOX_STATIC_PREFIX")"

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
  --disable-avformat \
  --enable-avutil \
  --disable-swresample \
  --disable-avdevice \
  --disable-avfilter \
  --disable-swscale \
  --enable-decoder=aac,aac_latm,alac,ac3,eac3,dca,mlp,truehd \
  --enable-encoder=aac,alac,ac3,eac3,dca,mlp,truehd \
  --enable-parser=aac,aac_latm,ac3,dca,mlp \
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

    # A deny list: the system DLLs a build imports vary with toolchain and
    # Windows version, while the forbidden ones are the codec libraries this
    # script builds. msys- catches an FFmpeg linked against the MSYS2
    # runtime, which runs only on a machine with MSYS2.
    $forbidden = "avcodec|avutil|swresample|FLAC|ogg|vorbis|opus|sndfile|libmad|mad\.dll|mp3lame|twolame|id3tag|wavpack|MACDll|libMAC|mpcdec|mpcenc|mpcpsy|opencore-amr|png|magic|libao|msys-"
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
    Write-Host "  -Library            Build static libsox + header; keep libraries and build tree"
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
    Write-Host "  -NoFfmpeg           Exclude FFmpeg codec support (AAC, ALAC, AC-3,"
    Write-Host "                      E-AC-3, DTS, MLP, TrueHD)"
    Write-Host "  -NoApe              Exclude Monkey's Audio support (Monkey's Audio SDK)"
    Write-Host "  -NoMusepack         Exclude Musepack support (libmpcdec, libmpcenc)"
    Write-Host ""
    Write-Host "Vulkan Backend (disabled by default):"
    Write-Host "  -Vulkan             Enable the Vulkan rate, FIR, and DSD backends"
    Write-Host ""
    Write-Host "Audio Driver Options:"
    Write-Host "  Default drivers: waveaudio (Windows native)"
    Write-Host ""
    Write-Host "  -NoWaveaudio        Exclude waveaudio driver (Windows default)"
    Write-Host ""
    Write-Host "Examples:"
    Write-Host "  .\build_static_libs.ps1                    # Build with all codecs"
    Write-Host "  .\build_static_libs.ps1 -NoMp2 -NoId3tag   # Exclude MP2 and ID3 tag"
    Write-Host "  .\build_static_libs.ps1 -Vulkan             # Build with Vulkan support"
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

    if ($Library) {
        $SoxBuildDir = Join-Path $ScriptDir "build-library"
        $OutputDir = Join-Path $ScriptDir "output\library"
        $KeepBuild = $true
        if ($EnableVulkan) {
            throw "-Library cannot use -Vulkan: third-party shared runtimes are required."
        }
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

    $script:PatchPath = Find-Patch
    if (-not $script:PatchPath) {
        Write-Err "GNU patch not found. Install Git for Windows, or MSYS2 and run: pacman -S patch"
        exit 1
    }
    Write-Info "Found patch: $script:PatchPath"

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
            Write-Err "Vulkan SDK with glslc is required. Install it or omit -Vulkan."
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
    Write-Host "    APE:        $EnableApe"
    Write-Host "    Musepack:   $EnableMusepack"
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
    }

    if ($EnableFfmpeg) {
        Build-Ffmpeg
    }

    if ($EnableApe) {
        Build-MonkeysAudio
    }

    if ($EnableMusepack) {
        Build-Musepack
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
            "-DWITH_APE=$($EnableApe.ToString().ToUpperInvariant())"
            "-DWITH_MUSEPACK=$($EnableMusepack.ToString().ToUpperInvariant())"
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

        if ($Library) {
            & $script:CMakePath --build . --config Release --target libsox --parallel $Jobs
        } else {
            & $script:CMakePath --build . --config Release --parallel $Jobs
        }
        if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }
    }
    finally {
        Pop-Location
    }

    Write-Success "SoX built successfully!"

    if ($Library) {
        $soxLib = Join-Path $SoxBuildDir "src\Release\libsox.lib"
        if (-not (Test-Path -LiteralPath $soxLib)) {
            $soxLib = Join-Path $SoxBuildDir "src\libsox.lib"
        }
        New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
        Copy-Item -LiteralPath $soxLib -Destination (Join-Path $OutputDir "libsox.lib")
        Copy-Item -LiteralPath (Join-Path $ScriptDir "src\sox.h") -Destination $OutputDir
        Write-Info "Library mode keeps static_libs and build-library (including GSM/LPC10 dependencies)."
    } else {
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
    if ($Library) {
        Write-Host "Output library: $OutputDir\libsox.lib"
        Write-Host "Header: $OutputDir\sox.h"
    } else {
        Write-Host "Output binary: $OutputDir\sox.exe"
        if ($EnableVulkan) {
            Write-Host "Vulkan runtime: $OutputDir\glslang.dll"
        }
    }
    Write-Host ""
}

# Run main function
Main
