#!/bin/bash
#
# SoX Static Libraries Build Script
# Downloads and compiles all optional libraries as static libraries
#
# Usage: ./build_static_libs.sh [OPTIONS]
#
# Supported platforms: Linux, macOS, FreeBSD, NetBSD, OpenBSD
#

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build_deps"
STATIC_LIBS_DIR="${SCRIPT_DIR}/static_libs"
DOWNLOAD_DIR="${BUILD_DIR}/downloads"
SRC_DIR="${BUILD_DIR}/src"
SOX_BUILD_DIR="${SCRIPT_DIR}/build"
OUTPUT_DIR="${SCRIPT_DIR}/output"

# Default number of parallel jobs
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# ------------------------------------------------------------------------------
# PLATFORM DETECTION
# ------------------------------------------------------------------------------
PLATFORM="$(uname -s)"
case "${PLATFORM}" in
    Linux)
        PLATFORM_NAME="Linux"
        ;;
    Darwin)
        PLATFORM_NAME="macOS"
        ;;
    FreeBSD)
        PLATFORM_NAME="FreeBSD"
        ;;
    NetBSD)
        PLATFORM_NAME="NetBSD"
        ;;
    OpenBSD)
        PLATFORM_NAME="OpenBSD"
        ;;
    DragonFly)
        PLATFORM_NAME="DragonFlyBSD"
        ;;
    *)
        PLATFORM_NAME="${PLATFORM}"
        ;;
esac

# ------------------------------------------------------------------------------
# CODEC OPTIONS (default: all ON)
# ------------------------------------------------------------------------------
ENABLE_OGG=ON
ENABLE_FLAC=ON
ENABLE_OPUS=ON
ENABLE_MP3=ON
ENABLE_MP2=ON
ENABLE_WAVPACK=ON
ENABLE_SNDFILE=ON
ENABLE_AMR=ON
ENABLE_ID3TAG=ON
ENABLE_PNG=ON
ENABLE_MAGIC=ON

# ------------------------------------------------------------------------------
# AUDIO DRIVER OPTIONS (platform-specific defaults)
# Linux: ALSA + ao
# macOS: CoreAudio + ao
# BSD: OSS + ao
# ------------------------------------------------------------------------------
ENABLE_ALSA=OFF
ENABLE_AO=ON
ENABLE_PULSEAUDIO=OFF
ENABLE_OSS=OFF
ENABLE_COREAUDIO=OFF

case "${PLATFORM}" in
    Linux)
        ENABLE_ALSA=ON
        ;;
    Darwin)
        ENABLE_COREAUDIO=ON
        ;;
    FreeBSD|NetBSD|OpenBSD|DragonFly)
        ENABLE_OSS=ON
        ;;
esac

# Library versions
ZLIB_VERSION="1.3.1"
LIBPNG_VERSION="1.6.43"
LIBOGG_VERSION="1.3.5"
LIBVORBIS_VERSION="1.3.7"
FLAC_VERSION="1.4.3"
OPUS_VERSION="1.5.2"
OPUSFILE_VERSION="0.12"
LIBMAD_VERSION="0.15.1b"
LAME_VERSION="3.100"
TWOLAME_VERSION="0.4.0"
LIBID3TAG_VERSION="0.15.1b"
WAVPACK_VERSION="5.7.0"
LIBSNDFILE_VERSION="1.2.2"
OPENCORE_AMR_VERSION="0.1.6"
FILE_VERSION="5.45"
LIBTOOL_VERSION="2.4.7"
LIBAO_VERSION="1.2.2"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ------------------------------------------------------------------------------
# Helper functions
# ------------------------------------------------------------------------------

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[OK]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_command() {
    if ! command -v "$1" &> /dev/null; then
        log_error "Required command '$1' not found. Please install it."
        exit 1
    fi
}

download_file() {
    local url="$1"
    local output="$2"

    if [ -f "$output" ]; then
        log_info "Already downloaded: $(basename "$output")"
        return 0
    fi

    log_info "Downloading: $(basename "$output")"

    if command -v curl &> /dev/null; then
        curl -L -o "$output" "$url"
    elif command -v wget &> /dev/null; then
        wget -O "$output" "$url"
    else
        log_error "Neither curl nor wget found"
        exit 1
    fi
}

extract_archive() {
    local archive="$1"
    local dest="$2"

    log_info "Extracting: $(basename "$archive")"

    case "$archive" in
        *.tar.gz|*.tgz)
            tar --no-same-owner -xzf "$archive" -C "$dest"
            ;;
        *.tar.xz)
            tar --no-same-owner -xJf "$archive" -C "$dest"
            ;;
        *.tar.bz2)
            tar --no-same-owner -xjf "$archive" -C "$dest"
            ;;
        *.zip)
            unzip -q "$archive" -d "$dest"
            ;;
        *)
            log_error "Unknown archive format: $archive"
            exit 1
            ;;
    esac
}

# Common configure flags for static libraries
get_common_flags() {
    echo "--prefix=${STATIC_LIBS_DIR} --disable-shared --enable-static"
}

# ------------------------------------------------------------------------------
# macOS: Hide/Restore shared libraries for static linking
# On macOS, the linker prefers .dylib over .a even with -static flags.
# We hide .dylib files temporarily so the linker is forced to use .a files.
# ------------------------------------------------------------------------------

# Check if we need sudo for a directory
needs_sudo() {
    local dir="$1"
    if [ -w "$dir" ]; then
        return 1  # false, no sudo needed
    else
        return 0  # true, needs sudo
    fi
}

# Execute command with sudo if needed
maybe_sudo() {
    local dir="$1"
    shift
    if needs_sudo "$dir"; then
        sudo "$@"
    else
        "$@"
    fi
}

hide_shared_libs() {
    local dir="$1"

    if [ ! -d "$dir" ]; then
        return 0
    fi

    local count=$(find "$dir" -name "*.dylib" 2>/dev/null | wc -l | tr -d ' ')
    if [ "$count" -eq 0 ]; then
        log_info "No shared libraries to hide in ${dir}"
        return 0
    fi

    log_info "Hiding ${count} shared libraries in ${dir}..."

    if needs_sudo "$dir"; then
        log_info "  (requires sudo)"
    fi

    # Hide regular dylib files
    find "$dir" -name "*.dylib" -type f 2>/dev/null | while read -r lib; do
        local hidden="${lib%.dylib}.hidden"
        maybe_sudo "$dir" mv "$lib" "$hidden"
    done

    # Also hide symlinks to dylibs
    find "$dir" -name "*.dylib" -type l 2>/dev/null | while read -r link; do
        local hidden="${link%.dylib}.hidden_link"
        maybe_sudo "$dir" mv "$link" "$hidden"
    done

    log_success "Shared libraries hidden in ${dir}"
}

restore_shared_libs() {
    local dir="$1"

    if [ ! -d "$dir" ]; then
        return 0
    fi

    local count=$(find "$dir" \( -name "*.hidden" -o -name "*.hidden_link" \) 2>/dev/null | wc -l | tr -d ' ')
    if [ "$count" -eq 0 ]; then
        return 0
    fi

    log_info "Restoring ${count} shared libraries in ${dir}..."

    if needs_sudo "$dir"; then
        log_info "  (requires sudo)"
    fi

    # Restore regular dylib files
    find "$dir" -name "*.hidden" -type f 2>/dev/null | while read -r hidden; do
        local lib="${hidden%.hidden}.dylib"
        maybe_sudo "$dir" mv "$hidden" "$lib"
    done

    # Restore symlinks
    find "$dir" -name "*.hidden_link" 2>/dev/null | while read -r hidden; do
        local link="${hidden%.hidden_link}.dylib"
        maybe_sudo "$dir" mv "$hidden" "$link"
    done

    log_success "Shared libraries restored in ${dir}"
}

# Common CMake flags for static libraries
get_cmake_flags() {
    echo "-DCMAKE_INSTALL_PREFIX=${STATIC_LIBS_DIR} -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON"
}

# ------------------------------------------------------------------------------
# Build functions for each library
# ------------------------------------------------------------------------------

build_zlib() {
    log_info "========== Building zlib ${ZLIB_VERSION} =========="

    local url="https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz"
    local archive="${DOWNLOAD_DIR}/zlib-${ZLIB_VERSION}.tar.gz"
    local src="${SRC_DIR}/zlib-${ZLIB_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"

    # zlib uses a custom configure script
    ./configure --prefix="${STATIC_LIBS_DIR}" --static
    make -j${JOBS}
    make install

    log_success "zlib installed"
}

build_libpng() {
    log_info "========== Building libpng ${LIBPNG_VERSION} =========="

    local url="https://github.com/pnggroup/libpng/archive/refs/tags/v${LIBPNG_VERSION}.tar.gz"
    local archive="${DOWNLOAD_DIR}/libpng-${LIBPNG_VERSION}.tar.gz"
    local src="${SRC_DIR}/libpng-${LIBPNG_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"
    mkdir -p build && cd build

    cmake .. $(get_cmake_flags) \
        -DPNG_SHARED=OFF \
        -DPNG_STATIC=ON \
        -DPNG_TESTS=OFF \
        -DPNG_FRAMEWORK=OFF \
        -DZLIB_ROOT="${STATIC_LIBS_DIR}"

    make -j${JOBS}
    make install

    log_success "libpng installed"
}

build_libogg() {
    log_info "========== Building libogg ${LIBOGG_VERSION} =========="

    local url="https://github.com/xiph/ogg/releases/download/v${LIBOGG_VERSION}/libogg-${LIBOGG_VERSION}.tar.gz"
    local archive="${DOWNLOAD_DIR}/libogg-${LIBOGG_VERSION}.tar.gz"
    local src="${SRC_DIR}/libogg-${LIBOGG_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"

    ./configure $(get_common_flags)
    make -j${JOBS}
    make install

    log_success "libogg installed"
}

build_libvorbis() {
    log_info "========== Building libvorbis ${LIBVORBIS_VERSION} =========="

    local url="https://github.com/xiph/vorbis/releases/download/v${LIBVORBIS_VERSION}/libvorbis-${LIBVORBIS_VERSION}.tar.gz"
    local archive="${DOWNLOAD_DIR}/libvorbis-${LIBVORBIS_VERSION}.tar.gz"
    local src="${SRC_DIR}/libvorbis-${LIBVORBIS_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"

    ./configure $(get_common_flags) \
        --with-ogg="${STATIC_LIBS_DIR}" \
        PKG_CONFIG_PATH="${STATIC_LIBS_DIR}/lib/pkgconfig"

    make -j${JOBS}
    make install

    log_success "libvorbis installed"
}

build_flac() {
    log_info "========== Building FLAC ${FLAC_VERSION} =========="

    local url="https://github.com/xiph/flac/releases/download/${FLAC_VERSION}/flac-${FLAC_VERSION}.tar.xz"
    local archive="${DOWNLOAD_DIR}/flac-${FLAC_VERSION}.tar.xz"
    local src="${SRC_DIR}/flac-${FLAC_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"
    mkdir -p build && cd build

    cmake .. $(get_cmake_flags) \
        -DBUILD_PROGRAMS=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_TESTING=OFF \
        -DBUILD_DOCS=OFF \
        -DINSTALL_MANPAGES=OFF \
        -DWITH_OGG=ON \
        -DOGG_ROOT="${STATIC_LIBS_DIR}"

    make -j${JOBS}
    make install

    log_success "FLAC installed"
}

build_opus() {
    log_info "========== Building opus ${OPUS_VERSION} =========="

    local url="https://github.com/xiph/opus/releases/download/v${OPUS_VERSION}/opus-${OPUS_VERSION}.tar.gz"
    local archive="${DOWNLOAD_DIR}/opus-${OPUS_VERSION}.tar.gz"
    local src="${SRC_DIR}/opus-${OPUS_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"

    ./configure $(get_common_flags) \
        --disable-doc \
        --disable-extra-programs

    make -j${JOBS}
    make install

    log_success "opus installed"
}

build_opusfile() {
    log_info "========== Building opusfile ${OPUSFILE_VERSION} =========="

    local url="https://github.com/xiph/opusfile/releases/download/v${OPUSFILE_VERSION}/opusfile-${OPUSFILE_VERSION}.tar.gz"
    local archive="${DOWNLOAD_DIR}/opusfile-${OPUSFILE_VERSION}.tar.gz"
    local src="${SRC_DIR}/opusfile-${OPUSFILE_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"

    ./configure $(get_common_flags) \
        --disable-http \
        --disable-doc \
        PKG_CONFIG_PATH="${STATIC_LIBS_DIR}/lib/pkgconfig"

    make -j${JOBS}
    make install

    log_success "opusfile installed"
}

# Update outdated config.guess/config.sub for ARM64 support
update_config_scripts() {
    local dir="$1"
    log_info "Updating config.guess/config.sub for ARM64 compatibility..."

    # Download fresh copies from GNU
    if [ -f "${dir}/config.guess" ]; then
        curl -sL "https://git.savannah.gnu.org/cgit/config.git/plain/config.guess" -o "${dir}/config.guess" 2>/dev/null || \
        wget -q "https://git.savannah.gnu.org/cgit/config.git/plain/config.guess" -O "${dir}/config.guess" 2>/dev/null || true
    fi
    if [ -f "${dir}/config.sub" ]; then
        curl -sL "https://git.savannah.gnu.org/cgit/config.git/plain/config.sub" -o "${dir}/config.sub" 2>/dev/null || \
        wget -q "https://git.savannah.gnu.org/cgit/config.git/plain/config.sub" -O "${dir}/config.sub" 2>/dev/null || true
    fi
}

build_libmad() {
    log_info "========== Building libmad ${LIBMAD_VERSION} =========="

    local url="https://downloads.sourceforge.net/project/mad/libmad/${LIBMAD_VERSION}/libmad-${LIBMAD_VERSION}.tar.gz"
    local archive="${DOWNLOAD_DIR}/libmad-${LIBMAD_VERSION}.tar.gz"
    local src="${SRC_DIR}/libmad-${LIBMAD_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"

    # Update config.guess/config.sub for ARM64 support
    update_config_scripts "$src"

    # Fix for modern compilers - remove -fforce-mem flag
    sed -i.bak 's/-fforce-mem//g' configure 2>/dev/null || \
        sed -i '' 's/-fforce-mem//g' configure

    ./configure $(get_common_flags)

    # Fix for macOS clang - remove unsupported -march=i486 flag
    if [ "${PLATFORM}" = "Darwin" ]; then
        sed -i '' 's/-march=i486//g' Makefile 2>/dev/null || true
    fi

    make -j${JOBS}
    make install

    log_success "libmad installed"
}

build_lame() {
    log_info "========== Building LAME ${LAME_VERSION} =========="

    local url="https://downloads.sourceforge.net/project/lame/lame/${LAME_VERSION}/lame-${LAME_VERSION}.tar.gz"
    local archive="${DOWNLOAD_DIR}/lame-${LAME_VERSION}.tar.gz"
    local src="${SRC_DIR}/lame-${LAME_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"

    ./configure $(get_common_flags) \
        --disable-frontend \
        --disable-decoder \
        --enable-nasm

    make -j${JOBS}
    make install

    log_success "LAME installed"
}

build_twolame() {
    log_info "========== Building TwoLAME ${TWOLAME_VERSION} =========="

    local url="https://github.com/njh/twolame/releases/download/${TWOLAME_VERSION}/twolame-${TWOLAME_VERSION}.tar.gz"
    local archive="${DOWNLOAD_DIR}/twolame-${TWOLAME_VERSION}.tar.gz"
    local src="${SRC_DIR}/twolame-${TWOLAME_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"

    ./configure $(get_common_flags) \
        --disable-sndfile

    make -j${JOBS}
    make install

    log_success "TwoLAME installed"
}

build_libid3tag() {
    log_info "========== Building libid3tag ${LIBID3TAG_VERSION} =========="

    local url="https://downloads.sourceforge.net/project/mad/libid3tag/${LIBID3TAG_VERSION}/libid3tag-${LIBID3TAG_VERSION}.tar.gz"
    local archive="${DOWNLOAD_DIR}/libid3tag-${LIBID3TAG_VERSION}.tar.gz"
    local src="${SRC_DIR}/libid3tag-${LIBID3TAG_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"

    # Update config.guess/config.sub for ARM64 support
    update_config_scripts "$src"

    ./configure $(get_common_flags) \
        CPPFLAGS="-I${STATIC_LIBS_DIR}/include" \
        LDFLAGS="-L${STATIC_LIBS_DIR}/lib"

    make -j${JOBS}
    make install

    log_success "libid3tag installed"
}

build_wavpack() {
    log_info "========== Building WavPack ${WAVPACK_VERSION} =========="

    local url="https://github.com/dbry/WavPack/releases/download/${WAVPACK_VERSION}/wavpack-${WAVPACK_VERSION}.tar.xz"
    local archive="${DOWNLOAD_DIR}/wavpack-${WAVPACK_VERSION}.tar.xz"
    local src="${SRC_DIR}/wavpack-${WAVPACK_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"

    ./configure $(get_common_flags) \
        --disable-apps \
        --disable-dsd

    make -j${JOBS}
    make install

    log_success "WavPack installed"
}

build_libsndfile() {
    log_info "========== Building libsndfile ${LIBSNDFILE_VERSION} =========="

    local url="https://github.com/libsndfile/libsndfile/releases/download/${LIBSNDFILE_VERSION}/libsndfile-${LIBSNDFILE_VERSION}.tar.xz"
    local archive="${DOWNLOAD_DIR}/libsndfile-${LIBSNDFILE_VERSION}.tar.xz"
    local src="${SRC_DIR}/libsndfile-${LIBSNDFILE_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"
    mkdir -p build && cd build

    cmake .. $(get_cmake_flags) \
        -DBUILD_PROGRAMS=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_TESTING=OFF \
        -DENABLE_EXTERNAL_LIBS=ON \
        -DENABLE_MPEG=OFF \
        -DCMAKE_PREFIX_PATH="${STATIC_LIBS_DIR}"

    make -j${JOBS}
    make install

    log_success "libsndfile installed"
}

build_opencore_amr() {
    log_info "========== Building opencore-amr ${OPENCORE_AMR_VERSION} =========="

    local url="https://downloads.sourceforge.net/project/opencore-amr/opencore-amr/opencore-amr-${OPENCORE_AMR_VERSION}.tar.gz"
    local archive="${DOWNLOAD_DIR}/opencore-amr-${OPENCORE_AMR_VERSION}.tar.gz"
    local src="${SRC_DIR}/opencore-amr-${OPENCORE_AMR_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"

    ./configure $(get_common_flags)
    make -j${JOBS}
    make install

    log_success "opencore-amr installed"
}

build_libmagic() {
    log_info "========== Building file/libmagic ${FILE_VERSION} =========="

    local url="https://astron.com/pub/file/file-${FILE_VERSION}.tar.gz"
    local archive="${DOWNLOAD_DIR}/file-${FILE_VERSION}.tar.gz"
    local src="${SRC_DIR}/file-${FILE_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"

    ./configure $(get_common_flags) \
        --disable-bzlib \
        --disable-xzlib \
        --disable-zstdlib

    make -j${JOBS}
    make install

    log_success "libmagic installed"
}

build_libtool() {
    log_info "========== Building libtool/libltdl ${LIBTOOL_VERSION} =========="

    local url="https://ftp.gnu.org/gnu/libtool/libtool-${LIBTOOL_VERSION}.tar.gz"
    local archive="${DOWNLOAD_DIR}/libtool-${LIBTOOL_VERSION}.tar.gz"
    local src="${SRC_DIR}/libtool-${LIBTOOL_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src/libltdl"

    ./configure $(get_common_flags)

    make -j${JOBS}

    # libltdl builds as libltdlc.a (convenience library), manually install
    cp .libs/libltdlc.a "${STATIC_LIBS_DIR}/lib/libltdl.a"
    cp ltdl.h "${STATIC_LIBS_DIR}/include/"
    # Copy additional headers needed by ltdl.h
    mkdir -p "${STATIC_LIBS_DIR}/include/libltdl"
    cp libltdl/lt_system.h "${STATIC_LIBS_DIR}/include/libltdl/"
    cp libltdl/lt_error.h "${STATIC_LIBS_DIR}/include/libltdl/"
    cp libltdl/lt_dlloader.h "${STATIC_LIBS_DIR}/include/libltdl/"

    log_success "libltdl installed"
}

build_libao() {
    log_info "========== Building libao ${LIBAO_VERSION} =========="

    local url="https://github.com/xiph/libao/archive/refs/tags/${LIBAO_VERSION}.tar.gz"
    local archive="${DOWNLOAD_DIR}/libao-${LIBAO_VERSION}.tar.gz"
    local src="${SRC_DIR}/libao-${LIBAO_VERSION}"

    download_file "$url" "$archive"

    if [ ! -d "$src" ]; then
        extract_archive "$archive" "$SRC_DIR"
    fi

    cd "$src"

    # libao from git needs autoreconf
    if [ ! -f "configure" ]; then
        autoreconf -fi
    fi

    # Build with only null, wav and raw plugins (no audio drivers - sox handles those)
    ./configure $(get_common_flags) \
        --disable-pulse \
        --disable-alsa \
        --disable-oss \
        --disable-arts \
        --disable-esd \
        --disable-nas \
        --disable-sndio \
        --enable-wav \
        --enable-au \
        --enable-raw \
        --enable-null

    make -j${JOBS}
    make install

    log_success "libao installed"
}

# ------------------------------------------------------------------------------
# Help and usage
# ------------------------------------------------------------------------------

show_help() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Build SoX with statically linked audio codec libraries."
    echo "Detected platform: ${PLATFORM_NAME}"
    echo ""
    echo "General Options:"
    echo "  --jobs, -j N          Number of parallel build jobs (default: auto)"
    echo "  --clean               Remove build and output directories"
    echo "  --help, -h            Show this help message"
    echo ""
    echo "Codec Options (all enabled by default, use to exclude):"
    echo "  --no-ogg              Exclude OGG/Vorbis support"
    echo "  --no-flac             Exclude FLAC support"
    echo "  --no-opus             Exclude Opus support"
    echo "  --no-mp3              Exclude MP3 support (libmad + LAME)"
    echo "  --no-mp2              Exclude MP2 support (TwoLAME)"
    echo "  --no-wavpack          Exclude WavPack support"
    echo "  --no-sndfile          Exclude libsndfile support"
    echo "  --no-amr              Exclude AMR support (opencore-amr)"
    echo "  --no-id3tag           Exclude ID3 tag support"
    echo "  --no-png              Exclude PNG spectrogram support"
    echo "  --no-magic            Exclude file type detection (libmagic)"
    echo ""
    echo "Audio Driver Options:"
    echo "  Platform defaults:"
    echo "    Linux:   ALSA + libao"
    echo "    macOS:   CoreAudio + libao"
    echo "    BSD:     OSS + libao"
    echo ""
    echo "  --no-alsa             Exclude ALSA driver"
    echo "  --no-ao               Exclude libao driver"
    echo "  --no-coreaudio        Exclude CoreAudio driver (macOS)"
    echo "  --no-oss              Exclude OSS driver"
    echo "  --with-alsa           Include ALSA driver"
    echo "  --with-coreaudio      Include CoreAudio driver"
    echo "  --with-pulseaudio     Include PulseAudio driver (dynamic linking)"
    echo "  --with-oss            Include OSS driver"
    echo ""
    echo "Examples:"
    echo "  $0                           # Build with platform defaults"
    echo "  $0 --with-pulseaudio         # Add PulseAudio support"
    echo "  $0 --no-amr --no-mp2         # Exclude AMR and MP2"
    echo "  $0 --no-ao                   # Exclude libao"
    echo ""
    exit 0
}

# ------------------------------------------------------------------------------
# Main script
# ------------------------------------------------------------------------------

main() {
    echo ""
    echo "=============================================="
    echo "  SoX Static Libraries Build Script"
    echo "=============================================="
    echo ""

    # Parse arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            --jobs|-j)
                JOBS="$2"
                shift 2
                ;;
            --clean)
                log_info "Cleaning build directories..."
                rm -rf "${BUILD_DIR}" "${STATIC_LIBS_DIR}" "${SOX_BUILD_DIR}" "${OUTPUT_DIR}"
                log_success "Clean complete"
                exit 0
                ;;
            # Codec exclusion options
            --no-ogg)
                ENABLE_OGG=OFF
                shift
                ;;
            --no-flac)
                ENABLE_FLAC=OFF
                shift
                ;;
            --no-opus)
                ENABLE_OPUS=OFF
                shift
                ;;
            --no-mp3)
                ENABLE_MP3=OFF
                shift
                ;;
            --no-mp2)
                ENABLE_MP2=OFF
                shift
                ;;
            --no-wavpack)
                ENABLE_WAVPACK=OFF
                shift
                ;;
            --no-sndfile)
                ENABLE_SNDFILE=OFF
                shift
                ;;
            --no-amr)
                ENABLE_AMR=OFF
                shift
                ;;
            --no-id3tag)
                ENABLE_ID3TAG=OFF
                shift
                ;;
            --no-png)
                ENABLE_PNG=OFF
                shift
                ;;
            --no-magic)
                ENABLE_MAGIC=OFF
                shift
                ;;
            # Audio driver options
            --no-alsa)
                ENABLE_ALSA=OFF
                shift
                ;;
            --with-alsa)
                ENABLE_ALSA=ON
                shift
                ;;
            --no-ao)
                ENABLE_AO=OFF
                shift
                ;;
            --no-coreaudio)
                ENABLE_COREAUDIO=OFF
                shift
                ;;
            --with-coreaudio)
                ENABLE_COREAUDIO=ON
                shift
                ;;
            --no-oss)
                ENABLE_OSS=OFF
                shift
                ;;
            --with-oss)
                ENABLE_OSS=ON
                shift
                ;;
            --with-pulseaudio)
                ENABLE_PULSEAUDIO=ON
                shift
                ;;
            --help|-h)
                show_help
                ;;
            *)
                log_error "Unknown option: $1"
                echo "Use --help for usage information"
                exit 1
                ;;
        esac
    done

    # Check required tools
    log_info "Checking required tools..."
    check_command "make"
    check_command "cmake"
    check_command "tar"

    if ! command -v curl &> /dev/null && ! command -v wget &> /dev/null; then
        log_error "Neither curl nor wget found. Please install one of them."
        exit 1
    fi

    # Create directories
    mkdir -p "${DOWNLOAD_DIR}"
    mkdir -p "${SRC_DIR}"
    mkdir -p "${STATIC_LIBS_DIR}"

    log_info "Platform: ${PLATFORM_NAME}"
    log_info "Build directory: ${BUILD_DIR}"
    log_info "Output directory: ${STATIC_LIBS_DIR}"
    log_info "Parallel jobs: ${JOBS}"
    echo ""

    # Display configuration
    echo "Configuration:"
    echo "  Codecs:"
    echo "    OGG/Vorbis: ${ENABLE_OGG}"
    echo "    FLAC:       ${ENABLE_FLAC}"
    echo "    Opus:       ${ENABLE_OPUS}"
    echo "    MP3:        ${ENABLE_MP3}"
    echo "    MP2:        ${ENABLE_MP2}"
    echo "    WavPack:    ${ENABLE_WAVPACK}"
    echo "    Sndfile:    ${ENABLE_SNDFILE}"
    echo "    AMR:        ${ENABLE_AMR}"
    echo "    ID3 Tag:    ${ENABLE_ID3TAG}"
    echo "    PNG:        ${ENABLE_PNG}"
    echo "    Magic:      ${ENABLE_MAGIC}"
    echo "  Audio Drivers:"
    echo "    ALSA:       ${ENABLE_ALSA}"
    echo "    CoreAudio:  ${ENABLE_COREAUDIO}"
    echo "    OSS:        ${ENABLE_OSS}"
    echo "    libao:      ${ENABLE_AO}"
    echo "    PulseAudio: ${ENABLE_PULSEAUDIO}"
    echo ""

    # Set PKG_CONFIG_PATH for finding installed libraries
    export PKG_CONFIG_PATH="${STATIC_LIBS_DIR}/lib/pkgconfig:${PKG_CONFIG_PATH}"
    export CFLAGS="-I${STATIC_LIBS_DIR}/include ${CFLAGS}"
    export LDFLAGS="-L${STATIC_LIBS_DIR}/lib ${LDFLAGS}"
    export CMAKE_PREFIX_PATH="${STATIC_LIBS_DIR}"

    # Build libraries in dependency order
    # Level 0: No dependencies (always needed)
    build_zlib
    build_libtool

    # Codec libraries
    if [ "${ENABLE_OGG}" = "ON" ] || [ "${ENABLE_FLAC}" = "ON" ] || [ "${ENABLE_OPUS}" = "ON" ]; then
        build_libogg
    fi

    if [ "${ENABLE_OGG}" = "ON" ]; then
        build_libvorbis
    fi

    if [ "${ENABLE_FLAC}" = "ON" ]; then
        build_flac
    fi

    if [ "${ENABLE_OPUS}" = "ON" ]; then
        build_opus
        build_opusfile
    fi

    if [ "${ENABLE_MP3}" = "ON" ]; then
        build_libmad
        build_lame
    fi

    if [ "${ENABLE_MP2}" = "ON" ]; then
        build_twolame
    fi

    if [ "${ENABLE_WAVPACK}" = "ON" ]; then
        build_wavpack
    fi

    if [ "${ENABLE_AMR}" = "ON" ]; then
        build_opencore_amr
    fi

    if [ "${ENABLE_ID3TAG}" = "ON" ]; then
        build_libid3tag
    fi

    if [ "${ENABLE_PNG}" = "ON" ]; then
        build_libpng
    fi

    if [ "${ENABLE_MAGIC}" = "ON" ]; then
        build_libmagic
    fi

    if [ "${ENABLE_SNDFILE}" = "ON" ]; then
        build_libsndfile
    fi

    # Audio driver libraries
    if [ "${ENABLE_AO}" = "ON" ]; then
        build_libao
    fi

    echo ""
    log_success "All libraries built successfully!"
    echo ""

    # Build SoX
    log_info "========== Building SoX =========="

    # On macOS, hide shared libraries to force static linking
    if [ "${PLATFORM}" = "Darwin" ]; then
        # Set up trap to restore libs on error or interruption
        trap 'log_warn "Build interrupted, restoring shared libraries..."; restore_shared_libs "${STATIC_LIBS_DIR}"; [ -d "/usr/local/lib" ] && restore_shared_libs "/usr/local/lib"; exit 1' INT TERM ERR

        hide_shared_libs "${STATIC_LIBS_DIR}"
        # Also hide system libs that might interfere
        if [ -d "/usr/local/lib" ]; then
            hide_shared_libs "/usr/local/lib"
        fi
    fi

    mkdir -p "${SOX_BUILD_DIR}"
    cd "${SOX_BUILD_DIR}"

    cmake "${SCRIPT_DIR}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_PREFIX_PATH="${STATIC_LIBS_DIR}" \
        -DWITH_ALSA=${ENABLE_ALSA} \
        -DWITH_COREAUDIO=${ENABLE_COREAUDIO} \
        -DWITH_OSS=${ENABLE_OSS} \
        -DWITH_AO=${ENABLE_AO} \
        -DWITH_PULSEAUDIO=${ENABLE_PULSEAUDIO}

    cmake --build . --config Release -j${JOBS}

    # On macOS, restore shared libraries
    if [ "${PLATFORM}" = "Darwin" ]; then
        # Clear the trap first
        trap - INT TERM ERR

        restore_shared_libs "${STATIC_LIBS_DIR}"
        if [ -d "/usr/local/lib" ]; then
            restore_shared_libs "/usr/local/lib"
        fi
    fi

    log_success "SoX built successfully!"

    # Copy binary to output directory
    log_info "Copying sox binary to output directory..."
    mkdir -p "${OUTPUT_DIR}"

    if [ -f "${SOX_BUILD_DIR}/src/sox" ]; then
        cp "${SOX_BUILD_DIR}/src/sox" "${OUTPUT_DIR}/"
        chmod +x "${OUTPUT_DIR}/sox"
    else
        log_error "sox binary not found!"
        exit 1
    fi

    log_success "Binary copied to: ${OUTPUT_DIR}/sox"

    # Cleanup temporary files
    log_info "Cleaning up temporary files..."
    rm -rf "${BUILD_DIR}"
    rm -rf "${STATIC_LIBS_DIR}"
    rm -rf "${SOX_BUILD_DIR}"

    log_success "Cleanup complete!"

    echo ""
    echo "=============================================="
    log_success "Build complete!"
    echo "=============================================="
    echo ""
    echo "Output binary: ${OUTPUT_DIR}/sox"
    echo ""

    # Show sox info
    "${OUTPUT_DIR}/sox" --version
    echo ""
}

# Run main function
main "$@"
