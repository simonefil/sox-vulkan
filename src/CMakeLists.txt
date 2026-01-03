# src/CMakeLists.txt - libsox library and sox executable

# =====================================================================
# libsox - Core library
# =====================================================================

# Format handlers and utils source
set(LIBSOX_CORE_SOURCES
    adpcms.c
    aiff.c
    cvsd.c
    g711.c
    g721.c
    g723_24.c
    g723_40.c
    g72x.c
    vox.c
    raw.c
    formats.c
    formats_i.c
    skelform.c
    xmalloc.c
    getopt.c
    util.c
    libsox.c
    libsox_i.c
    sox-fmt.c
)

set(LIBSOX_CORE_HEADERS
    adpcms.h
    aiff.h
    cvsd.h
    cvsdfilt.h
    g711.h
    g72x.h
    vox.h
    raw.h
    formats.h
    sox_i.h
    xmalloc.h
    soxomp.h
    sox.h
)

# Effects source
set(LIBSOX_EFFECTS_SOURCES
    bend.c
    biquad.c
    biquads.c
    chorus.c
    compand.c
    compandt.c
    contrast.c
    dcshift.c
    delay.c
    dft_filter.c
    dither.c
    divide.c
    dop.c
    downsample.c
    earwax.c
    echo.c
    echos.c
    effects.c
    effects_i.c
    effects_i_dsp.c
    fade.c
    fft4g.c
    fir.c
    firfit.c
    flanger.c
    gain.c
    hilbert.c
    input.c
    ladspa.c
    loudness.c
    mcompand.c
    noiseprof.c
    noisered.c
    output.c
    overdrive.c
    pad.c
    phaser.c
    rate.c
    remix.c
    repeat.c
    reverb.c
    reverse.c
    sdm.c
    silence.c
    sinc.c
    skeleff.c
    speed.c
    splice.c
    stat.c
    stats.c
    stretch.c
    swap.c
    synth.c
    tempo.c
    tremolo.c
    trim.c
    upsample.c
    vad.c
    vol.c
)

set(LIBSOX_EFFECTS_HEADERS
    band.h
    biquad.h
    compandt.h
    dft_filter.h
    dither.h
    effects.h
    fft4g.h
    fifo.h
    ladspa.h
    mcompand_xover.h
    noisered.h
    rate_filters.h
    rate_half_fir.h
    rate_poly_fir0.h
    rate_poly_fir.h
    sdm.h
    sdm_x86.h
)

# Built-in format handlers
set(LIBSOX_FORMAT_SOURCES
    raw-fmt.c
    s1-fmt.c
    s2-fmt.c
    s3-fmt.c
    s4-fmt.c
    u1-fmt.c
    u2-fmt.c
    u3-fmt.c
    u4-fmt.c
    al-fmt.c
    la-fmt.c
    ul-fmt.c
    lu-fmt.c
    8svx.c
    aiff-fmt.c
    aifc-fmt.c
    au.c
    avr.c
    cdr.c
    cvsd-fmt.c
    dvms-fmt.c
    dat.c
    hcom.c
    htk.c
    maud.c
    prc.c
    sf.c
    smp.c
    sounder.c
    soundtool.c
    sphere.c
    tx16w.c
    voc.c
    vox-fmt.c
    ima-fmt.c
    adpcm.c
    ima_rw.c
    wav.c
    wve.c
    xa.c
    nulfile.c
    f4-fmt.c
    f8-fmt.c
    gsrt.c
    dsf.c
    dsdiff.c
)

set(LIBSOX_FORMAT_HEADERS
    adpcm.h
    ima_rw.h
)

# Combine all sources
set(LIBSOX_SOURCES
    ${LIBSOX_CORE_SOURCES}
    ${LIBSOX_EFFECTS_SOURCES}
    ${LIBSOX_FORMAT_SOURCES}
)

set(LIBSOX_HEADERS
    ${LIBSOX_CORE_HEADERS}
    ${LIBSOX_EFFECTS_HEADERS}
    ${LIBSOX_FORMAT_HEADERS}
)

# Create libsox library
add_library(libsox ${LIBSOX_SOURCES} ${LIBSOX_HEADERS})

# Set output name (produces libsox.so on Linux, libsox.lib on Windows)
set_target_properties(libsox PROPERTIES
    OUTPUT_NAME libsox
    VERSION ${SOX_VERSION_MAJOR}.${SOX_VERSION_MINOR}.${SOX_VERSION_PATCH}
    SOVERSION ${SOX_VERSION_MAJOR}
)

# Include directories
target_include_directories(libsox PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

# Add static_libs to library search path if it exists
set(STATIC_LIBS_DIR "${CMAKE_SOURCE_DIR}/static_libs")
if(EXISTS "${STATIC_LIBS_DIR}/lib")
    target_link_directories(libsox PRIVATE "${STATIC_LIBS_DIR}/lib")
endif()

# Link math library
if(MATH_LIBRARY)
    target_link_libraries(libsox PRIVATE ${MATH_LIBRARY})
endif()

# OpenMP support
if(HAVE_OPENMP AND OpenMP_C_FOUND)
    target_link_libraries(libsox PRIVATE OpenMP::OpenMP_C)
endif()

# =====================================================================
# Optional format/device support - conditionally add sources and libs
# =====================================================================

# PNG support (spectrogram effect)
if(HAVE_PNG)
    target_sources(libsox PRIVATE spectrogram.c)
    if(TARGET PNG::PNG)
        target_link_libraries(libsox PRIVATE PNG::PNG)
    else()
        target_link_libraries(libsox PRIVATE ${PNG_LIBRARIES} ${ZLIB_LIBRARIES})
        target_include_directories(libsox PRIVATE ${PNG_INCLUDE_DIRS})
    endif()
endif()

# libmagic support
if(HAVE_MAGIC)
    target_link_libraries(libsox PRIVATE ${MAGIC_LIBRARIES})
    target_include_directories(libsox PRIVATE ${MAGIC_INCLUDE_DIR})
endif()

# GSM format
if(HAVE_GSM)
    target_sources(libsox PRIVATE gsm.c)
    if(EXTERNAL_GSM)
        target_link_libraries(libsox PRIVATE ${GSM_LIBRARIES})
        target_include_directories(libsox PRIVATE ${GSM_INCLUDE_DIR})
    else()
        target_link_libraries(libsox PRIVATE SoX::gsm)
    endif()
    target_compile_definitions(libsox PRIVATE STATIC_GSM)
endif()

# LPC10 format
if(HAVE_LPC10)
    target_sources(libsox PRIVATE lpc10.c)
    if(EXTERNAL_LPC10)
        target_link_libraries(libsox PRIVATE ${LPC10_LIBRARIES})
        target_include_directories(libsox PRIVATE ${LPC10_INCLUDE_DIR})
    else()
        target_link_libraries(libsox PRIVATE SoX::lpc10)
    endif()
    target_compile_definitions(libsox PRIVATE STATIC_LPC10)
endif()

# FLAC format
if(HAVE_FLAC)
    target_sources(libsox PRIVATE flac.c)
    target_link_libraries(libsox PRIVATE ${FLAC_LIBRARIES})
    target_include_directories(libsox PRIVATE ${FLAC_INCLUDE_DIRS})
    target_compile_definitions(libsox PRIVATE STATIC_FLAC FLAC__NO_DLL)
endif()

# Ogg Vorbis format
if(HAVE_OGG_VORBIS)
    target_sources(libsox PRIVATE vorbis.c)
    target_link_libraries(libsox PRIVATE ${OGG_VORBIS_LIBRARIES})
    target_include_directories(libsox PRIVATE ${OGG_VORBIS_INCLUDE_DIRS})
    target_compile_definitions(libsox PRIVATE STATIC_OGG_VORBIS)
endif()

# Opus format
if(HAVE_OPUS)
    target_sources(libsox PRIVATE opus.c)
    target_link_libraries(libsox PRIVATE ${OPUS_LIBRARIES})
    target_include_directories(libsox PRIVATE ${OPUS_INCLUDE_DIRS})
    target_compile_definitions(libsox PRIVATE STATIC_OPUS)
endif()

# MP3 format (mad/lame/twolame)
if(HAVE_MP3)
    target_sources(libsox PRIVATE mp3.c)
    target_link_libraries(libsox PRIVATE ${MP3_LIBRARIES})
    target_compile_definitions(libsox PRIVATE STATIC_MP3)
    if(HAVE_MAD_H)
        target_include_directories(libsox PRIVATE ${MAD_INCLUDE_DIR})
    endif()
    if(HAVE_LAME)
        target_include_directories(libsox PRIVATE ${LAME_INCLUDE_DIR})
    endif()
    if(HAVE_TWOLAME)
        target_include_directories(libsox PRIVATE ${TWOLAME_INCLUDE_DIR})
    endif()
endif()

# id3tag support
if(HAVE_ID3TAG)
    target_sources(libsox PRIVATE id3.c)
    target_link_libraries(libsox PRIVATE ${ID3TAG_LIBRARIES})
    target_include_directories(libsox PRIVATE ${ID3TAG_INCLUDE_DIR})
endif()

# WavPack format
if(HAVE_WAVPACK)
    target_sources(libsox PRIVATE wavpack.c)
    target_link_libraries(libsox PRIVATE ${WAVPACK_LIBRARIES})
    target_include_directories(libsox PRIVATE ${WAVPACK_INCLUDE_DIRS})
    target_compile_definitions(libsox PRIVATE STATIC_WAVPACK)
endif()

# libsndfile support
if(HAVE_SNDFILE)
    target_sources(libsox PRIVATE
        sndfile.c
        caf.c
        mat4.c
        mat5.c
        paf.c
        fap.c
        w64.c
        xi.c
        pvf.c
        sd2.c
    )
    target_link_libraries(libsox PRIVATE ${SNDFILE_LIBRARIES})
    target_include_directories(libsox PRIVATE ${SNDFILE_INCLUDE_DIRS})
    target_compile_definitions(libsox PRIVATE STATIC_SNDFILE)
endif()

# AMR-NB format
if(HAVE_AMRNB)
    target_sources(libsox PRIVATE amr-nb.c)
    target_link_libraries(libsox PRIVATE ${AMRNB_LIBRARIES})
    target_include_directories(libsox PRIVATE ${AMRNB_INCLUDE_DIR})
    target_compile_definitions(libsox PRIVATE STATIC_AMRNB)
endif()

# AMR-WB format
if(HAVE_AMRWB)
    target_sources(libsox PRIVATE amr-wb.c)
    target_link_libraries(libsox PRIVATE ${AMRWB_LIBRARIES})
    target_include_directories(libsox PRIVATE ${AMRWB_INCLUDE_DIR})
    target_compile_definitions(libsox PRIVATE STATIC_AMRWB)
endif()

# =====================================================================
# Audio device drivers
# =====================================================================

# ALSA driver (Linux)
if(HAVE_ALSA)
    target_sources(libsox PRIVATE alsa.c)
    target_link_libraries(libsox PRIVATE ${ALSA_LIBRARIES})
    target_include_directories(libsox PRIVATE ${ALSA_INCLUDE_DIRS})
    target_compile_definitions(libsox PRIVATE STATIC_ALSA)
endif()

# PulseAudio driver
if(HAVE_PULSEAUDIO)
    target_sources(libsox PRIVATE pulseaudio.c)
    target_link_libraries(libsox PRIVATE ${PULSEAUDIO_LIBRARIES})
    target_include_directories(libsox PRIVATE ${PULSEAUDIO_INCLUDE_DIRS})
    target_compile_definitions(libsox PRIVATE STATIC_PULSEAUDIO)
endif()

# OSS driver
if(HAVE_OSS)
    target_sources(libsox PRIVATE oss.c)
    if(OSS_LIBRARIES)
        target_link_libraries(libsox PRIVATE ${OSS_LIBRARIES})
    endif()
    target_compile_definitions(libsox PRIVATE STATIC_OSS)
endif()

# Sun Audio driver
if(HAVE_SUN_AUDIO)
    target_sources(libsox PRIVATE sunaudio.c)
    target_compile_definitions(libsox PRIVATE STATIC_SUN_AUDIO)
endif()

# CoreAudio driver (macOS)
if(HAVE_COREAUDIO)
    target_sources(libsox PRIVATE coreaudio.c)
    target_link_libraries(libsox PRIVATE ${COREAUDIO_LIBRARIES})
    target_compile_definitions(libsox PRIVATE STATIC_COREAUDIO)
endif()

# WaveAudio driver (Windows)
if(HAVE_WAVEAUDIO)
    target_sources(libsox PRIVATE waveaudio.c)
    target_link_libraries(libsox PRIVATE ${WAVEAUDIO_LIBRARIES})
    target_compile_definitions(libsox PRIVATE STATIC_WAVEAUDIO)
endif()

# sndio driver (OpenBSD, FreeBSD, NetBSD)
if(HAVE_SNDIO)
    target_sources(libsox PRIVATE sndio.c)
    target_link_libraries(libsox PRIVATE ${SNDIO_LIBRARIES})
    if(SNDIO_INCLUDE_DIRS)
        target_include_directories(libsox PRIVATE ${SNDIO_INCLUDE_DIRS})
    endif()
    target_compile_definitions(libsox PRIVATE STATIC_SNDIO)
endif()

# libao driver
if(HAVE_AO)
    target_sources(libsox PRIVATE ao.c)
    target_link_libraries(libsox PRIVATE ${AO_LIBRARIES})
    target_include_directories(libsox PRIVATE ${AO_INCLUDE_DIRS})
    target_compile_definitions(libsox PRIVATE STATIC_AO)
endif()

# =====================================================================
# Windows-specific sources
# =====================================================================

if(IS_WINDOWS)
    target_sources(libsox PRIVATE
        win32-glob.c
        win32-ltdl.c
    )
endif()

# libltdl for LADSPA support (non-Windows)
if(NOT IS_WINDOWS AND HAVE_LIBLTDL)
    target_link_libraries(libsox PRIVATE ${LTDL_LIBRARIES})
    target_include_directories(libsox PRIVATE ${LTDL_INCLUDE_DIRS})
    # libltdl needs libdl on Linux
    if(IS_LINUX)
        target_link_libraries(libsox PRIVATE dl)
    endif()
endif()

# =====================================================================
# sox executable
# =====================================================================

set(SOX_SOURCES sox.c)

if(IS_WINDOWS)
    list(APPEND SOX_SOURCES win32-glob.c)
endif()

add_executable(sox_exe ${SOX_SOURCES})

set_target_properties(sox_exe PROPERTIES
    OUTPUT_NAME sox
)

# Add static_libs to link path for sox_exe
if(EXISTS "${STATIC_LIBS_DIR}/lib")
    target_link_directories(sox_exe PRIVATE "${STATIC_LIBS_DIR}/lib")
endif()

target_link_libraries(sox_exe PRIVATE libsox)

# For static builds, we need to link transitive dependencies
# These are needed because libsndfile and other static libs have dependencies
if(NOT BUILD_SHARED_LIBS AND EXISTS "${STATIC_LIBS_DIR}/lib")
    # Link order matters for static libraries
    # Dependencies should come after the libraries that use them
    if(HAVE_SNDFILE)
        target_link_libraries(sox_exe PRIVATE ${SNDFILE_LIBRARIES})
    endif()
    if(HAVE_FLAC)
        target_link_libraries(sox_exe PRIVATE ${FLAC_LIBRARIES})
    endif()
    if(HAVE_OGG_VORBIS)
        target_link_libraries(sox_exe PRIVATE ${OGG_VORBIS_LIBRARIES})
    endif()
    if(HAVE_OPUS)
        target_link_libraries(sox_exe PRIVATE ${OPUS_LIBRARIES} opus)
    endif()
    if(HAVE_WAVPACK)
        target_link_libraries(sox_exe PRIVATE ${WAVPACK_LIBRARIES})
    endif()
    if(HAVE_MP3)
        target_link_libraries(sox_exe PRIVATE ${MP3_LIBRARIES})
    endif()
    if(HAVE_AMRNB)
        target_link_libraries(sox_exe PRIVATE ${AMRNB_LIBRARIES})
    endif()
    if(HAVE_AMRWB)
        target_link_libraries(sox_exe PRIVATE ${AMRWB_LIBRARIES})
    endif()
    # Audio drivers (from system)
    if(HAVE_ALSA)
        target_link_libraries(sox_exe PRIVATE ${ALSA_LIBRARIES})
    endif()
    # Common dependencies
    target_link_libraries(sox_exe PRIVATE ogg)
    if(MATH_LIBRARY)
        target_link_libraries(sox_exe PRIVATE ${MATH_LIBRARY})
    endif()
    # pthread needed on FreeBSD and other BSDs for wavpack
    if(CMAKE_SYSTEM_NAME MATCHES "FreeBSD|OpenBSD|NetBSD|DragonFly")
        target_link_libraries(sox_exe PRIVATE pthread)
    endif()
endif()

# =====================================================================
# Installation
# =====================================================================

# Install library
install(TARGETS libsox
    EXPORT SoXTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install executable
install(TARGETS sox_exe
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install header
install(FILES sox.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Create symlinks for play/rec/soxi (Unix-like systems: Linux, macOS, BSD)
if(NOT IS_WINDOWS)
    install(CODE "
        execute_process(
            COMMAND \${CMAKE_COMMAND} -E create_symlink sox \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/play
        )
        execute_process(
            COMMAND \${CMAKE_COMMAND} -E create_symlink sox \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/rec
        )
        execute_process(
            COMMAND \${CMAKE_COMMAND} -E create_symlink sox \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/soxi
        )
    ")
endif()

# Export targets
install(EXPORT SoXTargets
    FILE SoXTargets.cmake
    NAMESPACE SoX::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SoX
)
