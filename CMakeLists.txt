cmake_minimum_required(VERSION 3.15)

# The static build needs all three of these, and each is guarded because it
# only exists from a later CMake than the minimum above.
#
#   CMP0091  the MSVC runtime is chosen through CMAKE_MSVC_RUNTIME_LIBRARY
#            rather than by rewriting the compile flags; the static build sets
#            that variable below.
#   CMP0156  de-duplicate the link line according to what the linker can
#            actually do, instead of the older fixed rule.
#   CMP0179  when a static library is de-duplicated, keep its first
#            occurrence rather than its last.
#
# The last two matter because a static link resolves left to right: a codec
# library dropped to the end of the line, or kept only at its last mention,
# stops satisfying the references that came before it.  REORDER_FREELY goes
# further and lets CMake arrange the line itself, which is what handles the
# cyclic dependencies among the FFmpeg libraries.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()
if(POLICY CMP0179)
    cmake_policy(SET CMP0179 NEW)
endif()
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.31)
    set(CMAKE_LINK_LIBRARIES_STRATEGY REORDER_FREELY)
endif()

project(SoX
    VERSION 15.1.0
    DESCRIPTION "Sound eXchange - audio processing tool"
    LANGUAGES C
)

# The shared library's version, inherited from the libtool current:revision:age
# of the Autotools build that no longer exists here.  It feeds VERSION and
# SOVERSION in src/CMakeLists.txt and is independent of the project version
# above: changing it is an ABI decision, not a release one.
set(SOX_VERSION_MAJOR 3)
set(SOX_VERSION_MINOR 0)
set(SOX_VERSION_PATCH 0)

# =====================================================================
# Platform detection
# =====================================================================
set(IS_LINUX FALSE)
set(IS_FREEBSD FALSE)
set(IS_NETBSD FALSE)
set(IS_OPENBSD FALSE)
set(IS_BSD FALSE)
set(IS_MACOS FALSE)
set(IS_WINDOWS FALSE)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(IS_LINUX TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(IS_FREEBSD TRUE)
    set(IS_BSD TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    set(IS_NETBSD TRUE)
    set(IS_BSD TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
    set(IS_OPENBSD TRUE)
    set(IS_BSD TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(IS_MACOS TRUE)
elseif(WIN32)
    set(IS_WINDOWS TRUE)
endif()

# BSD systems: add /usr/local paths for ports/packages
if(IS_BSD)
    list(APPEND CMAKE_PREFIX_PATH "/usr/local")
    list(APPEND CMAKE_INCLUDE_PATH "/usr/local/include")
    list(APPEND CMAKE_LIBRARY_PATH "/usr/local/lib")
    # pkg-config path for BSD ports
    set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/usr/local/lib/pkgconfig:/usr/local/libdata/pkgconfig")
endif()

# Options
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(BUILD_STATIC_EXECUTABLE "Build executable with static dependencies" OFF)
option(SOX_REQUIRE_STATIC_DEPENDENCIES "Reject shared codec libraries and require static codec dependencies" OFF)
option(WITH_OPENMP "Enable OpenMP support" ON)
option(SOX_REQUIRE_OPENMP "Fail configuration when OpenMP support is unavailable" OFF)
option(WITH_VULKAN "Enable the Vulkan backend for FIR and DSD modulation" OFF)

# macOS is absent on purpose: it ships no static libSystem, so -static cannot
# work there and the option means "static dependencies, system libraries
# shared" instead.  That is why this sets SOX_REQUIRE_STATIC_DEPENDENCIES,
# which every platform honours, and adds -static only where it is achievable.
if(BUILD_STATIC_EXECUTABLE)
    set(BUILD_SHARED_LIBS OFF)
    set(SOX_REQUIRE_STATIC_DEPENDENCIES ON)

    if(IS_LINUX)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
    elseif(IS_WINDOWS AND MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# =====================================================================
# Static libraries support (from build_static_libs.sh)
# =====================================================================

# Check if static_libs directory exists and add it to search paths
set(STATIC_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/static_libs" CACHE PATH
    "Prefix containing statically built third-party libraries")
if(EXISTS "${STATIC_LIBS_DIR}")
    message(STATUS "Found static_libs directory: ${STATIC_LIBS_DIR}")
    list(INSERT CMAKE_PREFIX_PATH 0 "${STATIC_LIBS_DIR}")
    list(INSERT CMAKE_INCLUDE_PATH 0 "${STATIC_LIBS_DIR}/include")
    list(INSERT CMAKE_LIBRARY_PATH 0 "${STATIC_LIBS_DIR}/lib")
    set(ENV{PKG_CONFIG_PATH} "${STATIC_LIBS_DIR}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")

endif()

if(SOX_REQUIRE_STATIC_DEPENDENCIES AND BUILD_SHARED_LIBS)
    message(FATAL_ERROR "SOX_REQUIRE_STATIC_DEPENDENCIES requires BUILD_SHARED_LIBS=OFF")
endif()

if(SOX_REQUIRE_OPENMP AND NOT WITH_OPENMP)
    message(FATAL_ERROR "SOX_REQUIRE_OPENMP requires WITH_OPENMP=ON")
endif()

if(SOX_REQUIRE_STATIC_DEPENDENCIES)
    if(NOT EXISTS "${STATIC_LIBS_DIR}/lib")
        message(FATAL_ERROR "Static dependency directory not found: ${STATIC_LIBS_DIR}/lib")
    endif()
    # Prevent pkg-config from falling through to Homebrew, MacPorts, or system
    # shared-library metadata during a release build.
    set(ENV{PKG_CONFIG_LIBDIR} "${STATIC_LIBS_DIR}/lib/pkgconfig:${STATIC_LIBS_DIR}/share/pkgconfig")
endif()

# Optional format/device support
option(WITH_ALSA "ALSA audio driver support" ON)
option(WITH_AO "libao audio driver support" ON)
option(WITH_COREAUDIO "CoreAudio support (macOS)" ON)
option(WITH_OSS "OSS audio driver support" ON)
option(WITH_PULSEAUDIO "PulseAudio support" ON)
option(WITH_SNDIO "sndio audio driver support" ON)
option(WITH_WAVEAUDIO "WaveAudio support (Windows)" ON)
option(WITH_SUNAUDIO "Sun audio support" ON)

option(WITH_FLAC "FLAC format support" ON)
option(WITH_GSM "GSM format support" ON)
option(WITH_LPC10 "LPC10 format support" ON)
option(WITH_MAD "MAD MP3 decoder support" ON)
option(WITH_LAME "LAME MP3 encoder support" ON)
option(WITH_TWOLAME "TwoLAME MP2 encoder support" ON)
option(WITH_ID3TAG "id3tag support" ON)
option(WITH_OGGVORBIS "Ogg Vorbis support" ON)
option(WITH_OPUS "Opus support" ON)
option(WITH_FFMPEG_CODECS "FFmpeg libavcodec-based format support" ON)
option(WITH_FFMPEG_FORMATS "FFmpeg libavformat-based container support" ON)
option(WITH_WAVPACK "WavPack support" ON)
option(WITH_SNDFILE "libsndfile support" ON)
option(WITH_AMRNB "AMR-NB support" ON)
option(WITH_AMRWB "AMR-WB support" ON)

option(WITH_PNG "PNG support for spectrogram" ON)
option(WITH_MAGIC "libmagic support for file type detection" ON)
option(WITH_LADSPA "LADSPA effects support" ON)

# Set default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Compiler settings
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The Vulkan build compiles one C++ translation unit (vulkan_fft_dd.cpp) that
# vendors vkFFT.h. That header relies on C++11 empty-brace pointer
# initialization ("PfContainer* x = {};"), which is invalid before C++11 --
# without an explicit standard here, some toolchains default a bare .cpp file
# to a pre-C++11 dialect and fail with "scalar initializer cannot be empty".
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -W -Wmissing-prototypes -Wstrict-prototypes -pedantic)

    # Platform-specific linker flags
    if(IS_LINUX)
        add_link_options(-Wl,-z,defs)
    endif()

endif()

# Platform detection
include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckSymbolExists)
include(CheckTypeSize)
include(TestBigEndian)
include(GNUInstallDirs)
include(CMakePushCheckState)

# Check endianness
test_big_endian(WORDS_BIGENDIAN)

# Check headers
check_include_file(fcntl.h HAVE_FCNTL_H)
check_include_file(unistd.h HAVE_UNISTD_H)
check_include_file(byteswap.h HAVE_BYTESWAP_H)
check_include_file(sys/stat.h HAVE_SYS_STAT_H)
check_include_file(sys/time.h HAVE_SYS_TIME_H)
check_include_file(sys/timeb.h HAVE_SYS_TIMEB_H)
check_include_file(sys/types.h HAVE_SYS_TYPES_H)
check_include_file(sys/utsname.h HAVE_SYS_UTSNAME_H)
check_include_file(sys/ioctl.h HAVE_SYS_IOCTL_H)
check_include_file(termios.h HAVE_TERMIOS_H)
check_include_file(glob.h HAVE_GLOB_H)
check_include_file(fenv.h HAVE_FENV_H)
check_include_file(inttypes.h HAVE_INTTYPES_H)
check_include_file(stdint.h HAVE_STDINT_H)
check_include_file(string.h HAVE_STRING_H)
check_include_file(strings.h HAVE_STRINGS_H)
check_include_file(io.h HAVE_IO_H)

# Check functions
check_function_exists(strcasecmp HAVE_STRCASECMP)
check_function_exists(strdup HAVE_STRDUP)
check_function_exists(popen HAVE_POPEN)
check_function_exists(vsnprintf HAVE_VSNPRINTF)
check_function_exists(gettimeofday HAVE_GETTIMEOFDAY)
check_function_exists(mkstemp HAVE_MKSTEMP)
check_function_exists(fmemopen HAVE_FMEMOPEN)
check_function_exists(lrint HAVE_LRINT)
check_function_exists(fseeko HAVE_FSEEKO)

# Math library
include(CheckLibraryExists)
check_library_exists(m pow "" HAVE_LIBM)
if(HAVE_LIBM)
    set(MATH_LIBRARY m)
endif()

# OpenMP support
if(WITH_OPENMP)
    find_package(OpenMP COMPONENTS C)
    if(OpenMP_C_FOUND)
        set(HAVE_OPENMP TRUE)
    elseif(SOX_REQUIRE_OPENMP)
        message(FATAL_ERROR "OpenMP support is required but was not found for the C compiler")
    endif()
endif()

# Three separate Vulkan dependencies, and only the first is the loader:
# glslc compiles this tree's own shaders at build time, glslang is what VkFFT
# calls at run time to compile the GLSL it generates, and VkFFT itself is
# header-only.
#
# glslang is found differently per platform because it is consumed
# differently.  The Windows SDK ships it as a DLL whose import library is not
# usable from a build that does not otherwise link the SDK, so the DLL is
# located here to be copied and installed beside the executable, and
# glslang_dynamic.c resolves the entry points at run time; everywhere else it
# is an ordinary library and gets linked.  NO_DEFAULT_PATH keeps the search on
# the SDK, since any other glslang.dll on the machine is the wrong build.
if(WITH_VULKAN)
    if(NOT IS_WINDOWS AND NOT IS_MACOS AND NOT IS_LINUX AND NOT IS_FREEBSD)
        message(FATAL_ERROR "The Vulkan FIR and DSD backends support Windows, macOS, Linux, and FreeBSD")
    endif()
    find_package(Vulkan REQUIRED)
    find_program(GLSLC_EXECUTABLE NAMES glslc REQUIRED)
    if(IS_WINDOWS)
        find_file(GLSLANG_RUNTIME_DLL NAMES glslang.dll PATHS "$ENV{VULKAN_SDK}/Bin" NO_DEFAULT_PATH REQUIRED)
    else()
        find_path(GLSLANG_INCLUDE_DIR NAMES glslang/Include/glslang_c_interface.h REQUIRED)
        if(IS_LINUX)
            find_package(Threads REQUIRED)
            find_package(glslang CONFIG REQUIRED)
            set(GLSLANG_LIBRARY glslang::glslang glslang::SPIRV)
        else()
            find_library(GLSLANG_LIBRARY NAMES glslang REQUIRED)
        endif()
    endif()
    find_path(VKFFT_INCLUDE_DIR NAMES vkFFT.h
        HINTS "${STATIC_LIBS_DIR}"
        PATH_SUFFIXES include/vkFFT vkFFT
        REQUIRED)
    set(HAVE_VULKAN TRUE)
endif()

# Find packages using pkg-config when available
find_package(PkgConfig QUIET)

# ===== External library detection =====

# GSM library (check for external, otherwise use in-tree)
set(EXTERNAL_GSM FALSE)
if(WITH_GSM)
    find_library(GSM_LIBRARY gsm)
    find_path(GSM_INCLUDE_DIR gsm/gsm.h gsm.h)
    if(GSM_LIBRARY AND GSM_INCLUDE_DIR)
        set(EXTERNAL_GSM TRUE)
        set(GSM_LIBRARIES ${GSM_LIBRARY})
    endif()
    set(HAVE_GSM TRUE)
endif()

# LPC10 library (check for external, otherwise use in-tree)
set(EXTERNAL_LPC10 FALSE)
if(WITH_LPC10)
    find_library(LPC10_LIBRARY lpc10)
    find_path(LPC10_INCLUDE_DIR lpc10.h)
    if(LPC10_LIBRARY AND LPC10_INCLUDE_DIR)
        set(EXTERNAL_LPC10 TRUE)
        set(LPC10_LIBRARIES ${LPC10_LIBRARY})
    endif()
    set(HAVE_LPC10 TRUE)
endif()

# PNG library (for spectrogram) - prefer static on Windows
if(WITH_PNG)
    if(IS_WINDOWS)
        find_library(PNG_LIBRARY_STATIC libpng16_static png16_static PATHS ${CMAKE_PREFIX_PATH}/lib NO_DEFAULT_PATH)
        if(PNG_LIBRARY_STATIC)
            find_path(PNG_INCLUDE_DIR png.h PATHS ${CMAKE_PREFIX_PATH}/include NO_DEFAULT_PATH)
            if(PNG_INCLUDE_DIR)
                set(PNG_FOUND TRUE)
                set(PNG_LIBRARIES ${PNG_LIBRARY_STATIC})
                set(PNG_INCLUDE_DIRS ${PNG_INCLUDE_DIR})
                set(HAVE_PNG TRUE)
            endif()
        else()
            find_package(PNG)
            if(PNG_FOUND)
                set(HAVE_PNG TRUE)
            endif()
        endif()
    else()
        find_package(PNG)
        if(PNG_FOUND)
            set(HAVE_PNG TRUE)
        endif()
    endif()
endif()

# ZLIB (needed by several libraries) - prefer static library on Windows
if(IS_WINDOWS)
    # Prefer zlibstatic.lib for static linking
    find_library(ZLIB_LIBRARY_STATIC zlibstatic PATHS ${CMAKE_PREFIX_PATH}/lib NO_DEFAULT_PATH)
    if(ZLIB_LIBRARY_STATIC)
        set(ZLIB_LIBRARIES ${ZLIB_LIBRARY_STATIC})
        find_path(ZLIB_INCLUDE_DIR zlib.h)
        if(ZLIB_INCLUDE_DIR)
            set(ZLIB_FOUND TRUE)
            set(ZLIB_INCLUDE_DIRS ${ZLIB_INCLUDE_DIR})
        endif()
    else()
        find_package(ZLIB)
    endif()
else()
    find_package(ZLIB)
endif()

# libmagic
if(WITH_MAGIC)
    find_library(MAGIC_LIBRARY magic)
    find_path(MAGIC_INCLUDE_DIR magic.h)
    if(MAGIC_LIBRARY AND MAGIC_INCLUDE_DIR)
        set(HAVE_MAGIC TRUE)
        set(MAGIC_LIBRARIES ${MAGIC_LIBRARY})
    endif()
endif()

# FLAC
if(WITH_FLAC)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(FLAC IMPORTED_TARGET flac)
        if(FLAC_FOUND)
            set(FLAC_LIBRARIES PkgConfig::FLAC)
        endif()
    endif()
    if(NOT FLAC_FOUND)
        find_library(FLAC_LIBRARY FLAC)
        find_path(FLAC_INCLUDE_DIR FLAC/all.h)
        if(FLAC_LIBRARY AND FLAC_INCLUDE_DIR)
            set(FLAC_LIBRARIES ${FLAC_LIBRARY})
            set(FLAC_INCLUDE_DIRS ${FLAC_INCLUDE_DIR})
            set(FLAC_FOUND TRUE)
        endif()
    endif()
    if(FLAC_FOUND)
        set(HAVE_FLAC TRUE)
    endif()
endif()

# Ogg Vorbis
#
# The modules are listed dependents first, which is the order a static link
# resolves them in, and the same shape recurs for sndfile below.
#
# On a fully static link, PkgConfig:: carries only Libs, so the transitive
# dependencies in Libs.private never reach the link line; the _STATIC_ variant
# is the flattened list that does.  It is used only where -static is actually
# in effect -- Linux and BSD -- because elsewhere the imported target is
# correct and pulls in fewer system libraries.
if(WITH_OGGVORBIS)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(VORBIS IMPORTED_TARGET vorbisfile vorbisenc vorbis)
        pkg_check_modules(OGG IMPORTED_TARGET ogg)
        if(VORBIS_FOUND)
            if(SOX_REQUIRE_STATIC_DEPENDENCIES AND (IS_LINUX OR IS_BSD))
                set(VORBIS_LIBRARIES ${VORBIS_STATIC_LINK_LIBRARIES})
            else()
                set(VORBIS_LIBRARIES PkgConfig::VORBIS)
            endif()
        endif()
        if(OGG_FOUND)
            if(SOX_REQUIRE_STATIC_DEPENDENCIES AND (IS_LINUX OR IS_BSD))
                set(OGG_LIBRARIES ${OGG_STATIC_LINK_LIBRARIES})
            else()
                set(OGG_LIBRARIES PkgConfig::OGG)
            endif()
        endif()
    endif()
    if(NOT VORBIS_FOUND)
        find_library(VORBIS_LIBRARY vorbis)
        find_library(VORBISFILE_LIBRARY vorbisfile)
        find_library(VORBISENC_LIBRARY vorbisenc)
        find_library(OGG_LIBRARY ogg)
        find_path(VORBIS_INCLUDE_DIR vorbis/codec.h)
        if(VORBIS_LIBRARY AND VORBISFILE_LIBRARY AND VORBISENC_LIBRARY AND OGG_LIBRARY AND VORBIS_INCLUDE_DIR)
            set(VORBIS_LIBRARIES ${VORBISENC_LIBRARY} ${VORBISFILE_LIBRARY} ${VORBIS_LIBRARY})
            set(OGG_LIBRARIES ${OGG_LIBRARY})
            set(VORBIS_INCLUDE_DIRS ${VORBIS_INCLUDE_DIR})
            set(VORBIS_FOUND TRUE)
            set(OGG_FOUND TRUE)
        endif()
    endif()
    if(VORBIS_FOUND AND OGG_FOUND)
        set(HAVE_OGG_VORBIS TRUE)
        set(OGG_VORBIS_LIBRARIES ${VORBIS_LIBRARIES} ${OGG_LIBRARIES})
        set(OGG_VORBIS_INCLUDE_DIRS ${VORBIS_INCLUDE_DIRS})
    endif()
endif()

# Opus (decoder and encoder are optional independently)
if(WITH_OPUS)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(OPUSFILE IMPORTED_TARGET opusfile)
        pkg_check_modules(OPUSENC IMPORTED_TARGET libopusenc)
        if(OPUSFILE_FOUND)
            set(OPUSFILE_LIBRARIES PkgConfig::OPUSFILE)
        endif()
        if(OPUSENC_FOUND)
            set(OPUSENC_LIBRARIES PkgConfig::OPUSENC)
        endif()
    endif()

    find_library(OPUS_LIBRARY opus)
    find_path(OPUS_INCLUDE_DIR opus.h PATH_SUFFIXES opus)
    find_library(OPUS_OGG_LIBRARY ogg)
    find_path(OPUS_OGG_INCLUDE_DIR ogg/ogg.h)

    if(NOT OPUSFILE_FOUND)
        find_library(OPUSFILE_LIBRARY opusfile)
        find_path(OPUSFILE_INCLUDE_DIR opusfile.h PATH_SUFFIXES opus)
        if(OPUS_LIBRARY AND OPUS_INCLUDE_DIR AND
                OPUS_OGG_LIBRARY AND OPUS_OGG_INCLUDE_DIR AND
                OPUSFILE_LIBRARY AND OPUSFILE_INCLUDE_DIR)
            set(OPUSFILE_LIBRARIES ${OPUSFILE_LIBRARY} ${OPUS_LIBRARY})
            set(OPUSFILE_INCLUDE_DIRS
                ${OPUSFILE_INCLUDE_DIR}
                ${OPUS_INCLUDE_DIR}
                ${OPUS_OGG_INCLUDE_DIR}
            )
            set(OPUSFILE_FOUND TRUE)
        endif()
    endif()

    if(NOT OPUSENC_FOUND)
        find_library(OPUSENC_LIBRARY opusenc)
        find_path(OPUSENC_INCLUDE_DIR opusenc.h PATH_SUFFIXES opus)
        if(OPUS_LIBRARY AND OPUS_INCLUDE_DIR AND OPUSENC_LIBRARY AND OPUSENC_INCLUDE_DIR)
            set(OPUSENC_LIBRARIES ${OPUSENC_LIBRARY} ${OPUS_LIBRARY})
            set(OPUSENC_INCLUDE_DIRS
                ${OPUSENC_INCLUDE_DIR}
                ${OPUS_INCLUDE_DIR}
            )
            set(OPUSENC_FOUND TRUE)
        endif()
    endif()

    set(OPUS_LIBRARIES)
    set(OPUS_INCLUDE_DIRS)
    if(OPUSFILE_FOUND)
        set(HAVE_OPUSFILE TRUE)
        list(APPEND OPUS_LIBRARIES ${OPUSFILE_LIBRARIES})
        list(APPEND OPUS_INCLUDE_DIRS ${OPUSFILE_INCLUDE_DIRS})
        if(OPUS_OGG_LIBRARY)
            list(APPEND OPUS_LIBRARIES ${OPUS_OGG_LIBRARY})
        endif()
    endif()
    if(OPUSENC_FOUND)
        set(HAVE_OPUSENC TRUE)
        list(APPEND OPUS_LIBRARIES ${OPUSENC_LIBRARIES})
        list(APPEND OPUS_INCLUDE_DIRS ${OPUSENC_INCLUDE_DIRS})
    endif()
    if(OPUS_LIBRARY)
        list(APPEND OPUS_LIBRARIES ${OPUS_LIBRARY})
    endif()
    list(REMOVE_DUPLICATES OPUS_LIBRARIES)
    list(REMOVE_DUPLICATES OPUS_INCLUDE_DIRS)

    if(HAVE_OPUSFILE OR HAVE_OPUSENC)
        set(HAVE_OPUS TRUE)
    endif()
endif()

# FFmpeg codecs
#
# The version floor is real, not a guess: ffmpeg-codec.c calls
# avcodec_get_supported_config(), which arrived in libavcodec 61 and replaced
# the per-field capability arrays that came before it.  libavutil 59 is the
# matching major from the same FFmpeg release; the two are not independently
# selectable in practice.  Only the pkg-config path can test it: the
# find_library fallback below has no version to read, so an older libavcodec
# gets past configure and fails at the call site instead.
if(WITH_FFMPEG_CODECS)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(FFMPEG_CODECS IMPORTED_TARGET
            libavcodec>=61
            libavutil>=59
        )
        if(FFMPEG_CODECS_FOUND)
            set(FFMPEG_CODECS_LIBRARIES PkgConfig::FFMPEG_CODECS)
        endif()
    endif()
    if(NOT FFMPEG_CODECS_FOUND)
        find_library(AVCODEC_LIBRARY avcodec)
        find_library(AVUTIL_LIBRARY avutil)
        find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
        find_path(AVUTIL_INCLUDE_DIR libavutil/avutil.h)
        if(AVCODEC_LIBRARY AND AVUTIL_LIBRARY AND AVCODEC_INCLUDE_DIR AND AVUTIL_INCLUDE_DIR)
            set(FFMPEG_CODECS_LIBRARIES
                ${AVCODEC_LIBRARY}
                ${AVUTIL_LIBRARY}
            )
            set(FFMPEG_CODECS_INCLUDE_DIRS
                ${AVCODEC_INCLUDE_DIR}
                ${AVUTIL_INCLUDE_DIR}
            )
            set(FFMPEG_CODECS_FOUND TRUE)
        endif()
    endif()
    if(FFMPEG_CODECS_FOUND)
        set(HAVE_FFMPEG_CODECS TRUE)
    endif()
endif()

# FFmpeg containers.  Gated on the codecs: a container yields packets that
# only ffmpeg-codec.c can decode, so libavformat without libavcodec would
# build a demuxer with nothing behind it.
if(WITH_FFMPEG_FORMATS AND HAVE_FFMPEG_CODECS)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(FFMPEG_FORMATS IMPORTED_TARGET libavformat>=61)
        if(FFMPEG_FORMATS_FOUND)
            set(FFMPEG_FORMATS_LIBRARIES PkgConfig::FFMPEG_FORMATS)
        endif()
    endif()
    if(NOT FFMPEG_FORMATS_FOUND)
        find_library(AVFORMAT_LIBRARY avformat)
        find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h)
        if(AVFORMAT_LIBRARY AND AVFORMAT_INCLUDE_DIR)
            set(FFMPEG_FORMATS_LIBRARIES ${AVFORMAT_LIBRARY})
            set(FFMPEG_FORMATS_INCLUDE_DIRS ${AVFORMAT_INCLUDE_DIR})
            set(FFMPEG_FORMATS_FOUND TRUE)
        endif()
    endif()
    if(FFMPEG_FORMATS_FOUND)
        set(HAVE_FFMPEG_FORMATS TRUE)
    endif()
endif()

# MAD (MP3 decoder)
if(WITH_MAD)
    find_library(MAD_LIBRARY mad)
    find_path(MAD_INCLUDE_DIR mad.h)
    if(MAD_LIBRARY AND MAD_INCLUDE_DIR)
        set(HAVE_MAD_H TRUE)
        set(MAD_LIBRARIES ${MAD_LIBRARY})
        # Use FPM_64BIT for 64-bit builds to avoid x86 inline asm in mad.h
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            add_definitions(-DFPM_64BIT)
        else()
            add_definitions(-DFPM_DEFAULT)
        endif()
    endif()
endif()

# LAME (MP3 encoder)
if(WITH_LAME)
    find_library(LAME_LIBRARY mp3lame)
    find_path(LAME_INCLUDE_DIR lame/lame.h)
    if(LAME_LIBRARY AND LAME_INCLUDE_DIR)
        set(CMAKE_REQUIRED_INCLUDES ${LAME_INCLUDE_DIR})
        check_include_file(lame/lame.h HAVE_LAME_LAME_H)
        set(LAME_LIBRARIES ${LAME_LIBRARY})
        set(HAVE_LAME TRUE)
    endif()
endif()

# TwoLAME (MP2 encoder)
if(WITH_TWOLAME)
    find_library(TWOLAME_LIBRARY twolame)
    find_path(TWOLAME_INCLUDE_DIR twolame.h)
    if(TWOLAME_LIBRARY AND TWOLAME_INCLUDE_DIR)
        set(HAVE_TWOLAME TRUE)
        set(HAVE_TWOLAME_H TRUE)
        set(TWOLAME_LIBRARIES ${TWOLAME_LIBRARY})
    endif()
endif()

# MP3 support requires at least one of MAD, LAME, or TwoLAME
if(HAVE_MAD_H OR HAVE_LAME OR HAVE_TWOLAME)
    set(HAVE_MP3 TRUE)
    set(MP3_LIBRARIES)
    if(HAVE_MAD_H)
        list(APPEND MP3_LIBRARIES ${MAD_LIBRARIES})
    endif()
    if(HAVE_LAME)
        list(APPEND MP3_LIBRARIES ${LAME_LIBRARIES})
    endif()
    if(HAVE_TWOLAME)
        list(APPEND MP3_LIBRARIES ${TWOLAME_LIBRARIES})
    endif()
endif()

# id3tag
if(WITH_ID3TAG)
    find_library(ID3TAG_LIBRARY id3tag)
    find_path(ID3TAG_INCLUDE_DIR id3tag.h)
    if(ID3TAG_LIBRARY AND ID3TAG_INCLUDE_DIR)
        set(HAVE_ID3TAG TRUE)
        set(ID3TAG_LIBRARIES ${ID3TAG_LIBRARY})
        if(ZLIB_FOUND)
            if(TARGET ZLIB::ZLIB)
                list(APPEND ID3TAG_LIBRARIES ZLIB::ZLIB)
            else()
                list(APPEND ID3TAG_LIBRARIES ${ZLIB_LIBRARIES})
            endif()
        endif()
    endif()
endif()

# WavPack
if(WITH_WAVPACK)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(WAVPACK IMPORTED_TARGET wavpack)
        if(WAVPACK_FOUND)
            set(WAVPACK_LIBRARIES PkgConfig::WAVPACK)
        endif()
    endif()
    if(NOT WAVPACK_FOUND)
        find_library(WAVPACK_LIBRARY wavpack)
        find_path(WAVPACK_INCLUDE_DIR wavpack/wavpack.h)
        if(WAVPACK_LIBRARY AND WAVPACK_INCLUDE_DIR)
            set(WAVPACK_LIBRARIES ${WAVPACK_LIBRARY})
            set(WAVPACK_INCLUDE_DIRS ${WAVPACK_INCLUDE_DIR})
            set(WAVPACK_FOUND TRUE)
        endif()
    endif()
    if(WAVPACK_FOUND)
        set(HAVE_WAVPACK TRUE)
    endif()
endif()

# libsndfile
if(WITH_SNDFILE)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SNDFILE IMPORTED_TARGET sndfile)
        if(SNDFILE_FOUND)
            if(SOX_REQUIRE_STATIC_DEPENDENCIES AND (IS_LINUX OR IS_BSD))
                set(SNDFILE_LIBRARIES ${SNDFILE_STATIC_LINK_LIBRARIES})
            else()
                set(SNDFILE_LIBRARIES PkgConfig::SNDFILE)
            endif()
        endif()
    endif()
    if(NOT SNDFILE_FOUND)
        find_library(SNDFILE_LIBRARY sndfile)
        find_path(SNDFILE_INCLUDE_DIR sndfile.h)
        if(SNDFILE_LIBRARY AND SNDFILE_INCLUDE_DIR)
            set(SNDFILE_LIBRARIES ${SNDFILE_LIBRARY})
            set(SNDFILE_INCLUDE_DIRS ${SNDFILE_INCLUDE_DIR})
            set(SNDFILE_FOUND TRUE)
        endif()
    endif()
    if(SNDFILE_FOUND)
        set(HAVE_SNDFILE TRUE)
    endif()
endif()

# AMR-NB
if(WITH_AMRNB)
    find_library(AMRNB_LIBRARY opencore-amrnb amrnb)
    find_path(AMRNB_INCLUDE_DIR opencore-amrnb/interf_dec.h amrnb/sp_dec.h)
    if(AMRNB_LIBRARY AND AMRNB_INCLUDE_DIR)
        set(HAVE_AMRNB TRUE)
        set(AMRNB_LIBRARIES ${AMRNB_LIBRARY})
        # Check if OpenCore headers are present (vs 3GPP reference)
        if(EXISTS "${AMRNB_INCLUDE_DIR}/opencore-amrnb/interf_dec.h")
            set(HAVE_OPENCORE_AMRNB_INTERF_DEC_H TRUE)
        endif()
    endif()
endif()

# AMR-WB
if(WITH_AMRWB)
    find_library(AMRWB_LIBRARY opencore-amrwb amrwb)
    find_path(AMRWB_INCLUDE_DIR opencore-amrwb/dec_if.h amrwb/dec.h)
    if(AMRWB_LIBRARY AND AMRWB_INCLUDE_DIR)
        set(HAVE_AMRWB TRUE)
        set(AMRWB_LIBRARIES ${AMRWB_LIBRARY})
        # Check if OpenCore headers are present (vs 3GPP reference)
        if(EXISTS "${AMRWB_INCLUDE_DIR}/opencore-amrwb/dec_if.h")
            set(HAVE_OPENCORE_AMRWB_DEC_IF_H TRUE)
        endif()
    endif()
endif()

# ===== Audio device driver detection =====

# ALSA (Linux only)
if(WITH_ALSA AND IS_LINUX)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(ALSA IMPORTED_TARGET alsa)
        if(ALSA_FOUND)
            set(ALSA_LIBRARIES PkgConfig::ALSA)
        endif()
    endif()
    if(NOT ALSA_FOUND)
        find_library(ALSA_LIBRARY asound)
        find_path(ALSA_INCLUDE_DIR alsa/asoundlib.h)
        if(ALSA_LIBRARY AND ALSA_INCLUDE_DIR)
            set(ALSA_LIBRARIES ${ALSA_LIBRARY})
            set(ALSA_INCLUDE_DIRS ${ALSA_INCLUDE_DIR})
            set(ALSA_FOUND TRUE)
        endif()
    endif()
    if(ALSA_FOUND)
        set(HAVE_ALSA TRUE)
    endif()
endif()

# PulseAudio (Linux and FreeBSD)
if(WITH_PULSEAUDIO AND (IS_LINUX OR IS_FREEBSD))
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(PULSEAUDIO IMPORTED_TARGET libpulse-simple)
        if(PULSEAUDIO_FOUND)
            set(PULSEAUDIO_LIBRARIES PkgConfig::PULSEAUDIO)
        endif()
    endif()
    if(NOT PULSEAUDIO_FOUND)
        find_library(PULSE_LIBRARY pulse)
        find_library(PULSE_SIMPLE_LIBRARY pulse-simple)
        find_path(PULSE_INCLUDE_DIR pulse/simple.h)
        if(PULSE_LIBRARY AND PULSE_SIMPLE_LIBRARY AND PULSE_INCLUDE_DIR)
            set(PULSEAUDIO_LIBRARIES ${PULSE_SIMPLE_LIBRARY} ${PULSE_LIBRARY})
            set(PULSEAUDIO_INCLUDE_DIRS ${PULSE_INCLUDE_DIR})
            set(PULSEAUDIO_FOUND TRUE)
        endif()
    endif()
    if(NOT PULSEAUDIO_FOUND AND SOX_REQUIRE_STATIC_DEPENDENCIES)
        # PulseAudio's official build produces libpulse and libpulse-simple as
        # shared-only client libraries. Keep this fallback scoped to the
        # optional PulseAudio driver; codec dependencies remain static-only.
        set(_SOX_SAVED_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
        set(CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_SHARED_LIBRARY_SUFFIX})
        find_library(PULSE_LIBRARY pulse)
        find_library(PULSE_SIMPLE_LIBRARY pulse-simple)
        set(CMAKE_FIND_LIBRARY_SUFFIXES ${_SOX_SAVED_LIBRARY_SUFFIXES})
        unset(_SOX_SAVED_LIBRARY_SUFFIXES)
        if(PULSE_LIBRARY AND PULSE_SIMPLE_LIBRARY AND PULSE_INCLUDE_DIR)
            set(PULSEAUDIO_LIBRARIES ${PULSE_SIMPLE_LIBRARY} ${PULSE_LIBRARY})
            set(PULSEAUDIO_INCLUDE_DIRS ${PULSE_INCLUDE_DIR})
            set(PULSEAUDIO_FOUND TRUE)
            message(STATUS "Using shared-only PulseAudio client libraries")
        endif()
    endif()
    if(PULSEAUDIO_FOUND)
        set(HAVE_PULSEAUDIO TRUE)
    endif()
endif()

# OSS (Linux, FreeBSD, NetBSD, OpenBSD)
if(WITH_OSS)
    check_include_file(sys/soundcard.h HAVE_SYS_SOUNDCARD_H)
    check_include_file(machine/soundcard.h HAVE_MACHINE_SOUNDCARD_H)
    if(HAVE_SYS_SOUNDCARD_H OR HAVE_MACHINE_SOUNDCARD_H)
        set(HAVE_OSS TRUE)
        set(OSS_LIBRARIES "")

        # FreeBSD: OSS is built into the kernel, no extra library needed
        # NetBSD: requires libossaudio
        # OpenBSD: deprecated, use sndio instead
        if(IS_NETBSD)
            find_library(OSS_LIBRARY ossaudio)
            if(OSS_LIBRARY)
                set(OSS_LIBRARIES ${OSS_LIBRARY})
            endif()
        endif()

        # Linux with OSS emulation may need ossaudio
        if(IS_LINUX)
            find_library(OSS_LIBRARY ossaudio)
            if(OSS_LIBRARY)
                set(OSS_LIBRARIES ${OSS_LIBRARY})
            endif()
        endif()
    endif()
endif()

# Sun Audio
if(WITH_SUNAUDIO)
    check_include_file(sys/audioio.h HAVE_SYS_AUDIOIO_H)
    check_include_file(sun/audioio.h HAVE_SUN_AUDIOIO_H)
    if(HAVE_SYS_AUDIOIO_H OR HAVE_SUN_AUDIOIO_H)
        set(HAVE_SUN_AUDIO TRUE)
    endif()
endif()

# CoreAudio (macOS only)
if(WITH_COREAUDIO AND IS_MACOS)
    find_library(COREAUDIO_FRAMEWORK CoreAudio)
    if(COREAUDIO_FRAMEWORK)
        set(HAVE_COREAUDIO TRUE)
        set(COREAUDIO_LIBRARIES "-framework CoreAudio")
    endif()
endif()

# WaveAudio (Windows only)
if(WITH_WAVEAUDIO AND IS_WINDOWS)
    set(HAVE_WAVEAUDIO TRUE)
    set(WAVEAUDIO_LIBRARIES winmm)
endif()

# sndio (OpenBSD native, also available on FreeBSD/NetBSD/Linux)
if(WITH_SNDIO)
    find_library(SNDIO_LIBRARY sndio)
    find_path(SNDIO_INCLUDE_DIR sndio.h)
    if(SNDIO_LIBRARY AND SNDIO_INCLUDE_DIR)
        set(HAVE_SNDIO TRUE)
        set(SNDIO_LIBRARIES ${SNDIO_LIBRARY})
        set(SNDIO_INCLUDE_DIRS ${SNDIO_INCLUDE_DIR})
    endif()
endif()

# libao
if(WITH_AO)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(AO IMPORTED_TARGET ao)
        if(AO_FOUND)
            set(AO_LIBRARIES PkgConfig::AO)
        endif()
    endif()
    if(NOT AO_FOUND)
        find_library(AO_LIBRARY ao)
        find_path(AO_INCLUDE_DIR ao/ao.h)
        if(AO_LIBRARY AND AO_INCLUDE_DIR)
            set(AO_LIBRARIES ${AO_LIBRARY})
            set(AO_INCLUDE_DIRS ${AO_INCLUDE_DIR})
            set(AO_FOUND TRUE)
        endif()
    endif()
    if(AO_FOUND)
        set(HAVE_AO TRUE)
    endif()
endif()

# Windows-specific settings
if(IS_WINDOWS)
    set(HAVE_WIN32_GLOB_H TRUE)
    set(HAVE_WIN32_LTDL_H TRUE)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)
    endif()
endif()

# BSD-specific checks
if(IS_BSD)
    # Check for BSD-specific functions
    check_function_exists(arc4random HAVE_ARC4RANDOM)

    # DragonFlyBSD detection (similar to FreeBSD)
    if(CMAKE_SYSTEM_NAME STREQUAL "DragonFly")
        set(IS_DRAGONFLY TRUE)
        set(IS_BSD TRUE)
    endif()
endif()

# LADSPA support (requires ltdl or dlopen)
if(WITH_LADSPA)
    # Find libltdl (from libtool)
    find_path(LTDL_INCLUDE_DIR ltdl.h
        HINTS ${CMAKE_PREFIX_PATH}
        PATH_SUFFIXES include
    )
    find_library(LTDL_LIBRARY
        NAMES ltdl libltdl
        HINTS ${CMAKE_PREFIX_PATH}
        PATH_SUFFIXES lib
    )

    if(LTDL_INCLUDE_DIR AND LTDL_LIBRARY)
        set(HAVE_LADSPA_H TRUE)
        set(HAVE_LIBLTDL TRUE)
        set(LTDL_LIBRARIES ${LTDL_LIBRARY})
        set(LTDL_INCLUDE_DIRS ${LTDL_INCLUDE_DIR})
        set(LADSPA_PATH "${CMAKE_INSTALL_FULL_LIBDIR}/ladspa" CACHE PATH "LADSPA plugin path")
        message(STATUS "Found libltdl: ${LTDL_LIBRARY}")
    else()
        message(STATUS "libltdl not found, disabling LADSPA")
        set(HAVE_LADSPA_H FALSE)
    endif()
endif()

# Generate config header
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/soxconfig.h.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/src/soxconfig.h
    @ONLY
)

# Make config header available
include_directories(${CMAKE_CURRENT_BINARY_DIR}/src)

# Add subdirectories
if(NOT EXTERNAL_GSM AND WITH_GSM)
    add_subdirectory(libgsm)
endif()

if(NOT EXTERNAL_LPC10 AND WITH_LPC10)
    add_subdirectory(lpc10)
endif()

add_subdirectory(src)

# Determine platform name for summary
if(IS_LINUX)
    set(PLATFORM_NAME "Linux")
elseif(IS_FREEBSD)
    set(PLATFORM_NAME "FreeBSD")
elseif(IS_NETBSD)
    set(PLATFORM_NAME "NetBSD")
elseif(IS_OPENBSD)
    set(PLATFORM_NAME "OpenBSD")
elseif(IS_MACOS)
    set(PLATFORM_NAME "macOS")
elseif(IS_WINDOWS)
    set(PLATFORM_NAME "Windows")
else()
    set(PLATFORM_NAME "${CMAKE_SYSTEM_NAME}")
endif()

# Summary
message(STATUS "")
message(STATUS "=== SoX Configuration Summary ===")
message(STATUS "")
message(STATUS "Platform:                ${PLATFORM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "Build type:              ${CMAKE_BUILD_TYPE}")
message(STATUS "Install prefix:          ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
message(STATUS "Optional Device Drivers:")
message(STATUS "  ALSA:                  ${HAVE_ALSA}")
message(STATUS "  libao:                 ${HAVE_AO}")
message(STATUS "  CoreAudio:             ${HAVE_COREAUDIO}")
message(STATUS "  OSS:                   ${HAVE_OSS}")
message(STATUS "  PulseAudio:            ${HAVE_PULSEAUDIO}")
message(STATUS "  sndio:                 ${HAVE_SNDIO}")
message(STATUS "  Sun Audio:             ${HAVE_SUN_AUDIO}")
message(STATUS "  WaveAudio:             ${HAVE_WAVEAUDIO}")
message(STATUS "")
message(STATUS "Optional File Formats:")
message(STATUS "  AMR-NB:                ${HAVE_AMRNB}")
message(STATUS "  AMR-WB:                ${HAVE_AMRWB}")
message(STATUS "  FLAC:                  ${HAVE_FLAC}")
message(STATUS "  GSM:                   ${HAVE_GSM} (external: ${EXTERNAL_GSM})")
message(STATUS "  LPC10:                 ${HAVE_LPC10} (external: ${EXTERNAL_LPC10})")
message(STATUS "  MP3 (mad/lame/twolame):${HAVE_MP3}")
message(STATUS "  Ogg Vorbis:            ${HAVE_OGG_VORBIS}")
message(STATUS "  Opus decode:           ${HAVE_OPUSFILE}")
message(STATUS "  Opus encode:           ${HAVE_OPUSENC}")
message(STATUS "  FFmpeg codecs:         ${HAVE_FFMPEG_CODECS}")
message(STATUS "  FFmpeg containers:     ${HAVE_FFMPEG_FORMATS}")
message(STATUS "  libsndfile:            ${HAVE_SNDFILE}")
message(STATUS "  WavPack:               ${HAVE_WAVPACK}")
message(STATUS "")
message(STATUS "Other Options:")
message(STATUS "  PNG (spectrogram):     ${HAVE_PNG}")
message(STATUS "  libmagic:              ${HAVE_MAGIC}")
message(STATUS "  LADSPA:                ${HAVE_LADSPA_H}")
message(STATUS "  OpenMP:                ${HAVE_OPENMP}")
message(STATUS "  Vulkan FIR/DSD:        ${HAVE_VULKAN}")
message(STATUS "")

# pkg-config file
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/sox.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/sox.pc
    @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sox.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
