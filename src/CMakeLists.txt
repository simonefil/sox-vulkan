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
    diagnostics.c
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
    diagnostics.h
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
    crossfeed.c
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
    matroska.c
    mp4.c
    container.c
    codec.c
    codec-pcm.c
    ebml.c
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
    codec.h
    container.h
    ebml.h
    ima_rw.h
    ffmpeg-codec.h
)

# Every shader under vulkan/ goes through two steps: glslc compiles it to a
# .spv, then sox_embed_binary turns that into a C array in a _spv.inc the host
# includes, so a single executable carries all the profiles.
#
# The three names per shader (_SOURCE, _SPV, _INC) are spelled out because
# each pair is a separate add_custom_command with its own DEPENDS; the
# reference-profile shaders also depend on double_double.glsl, which glslc
# resolves relative to the including file.
#
# sox_embed_binary is built for the host and must exist before any .inc,
# hence the DEPENDS on the target.
if(HAVE_VULKAN)
    set(SDM_VULKAN_MASH_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/sdm_mash2_fsm.comp")
    set(SDM_VULKAN_RESIDENT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/sdm_resident_f32.comp")
    set(SDM_VULKAN_RESIDENT_PRECISE_F32_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/sdm_resident_precise_f32.comp")
    set(SDM_VULKAN_RESIDENT_REFERENCE_DD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/sdm_resident_reference_dd.comp")
    set(SDM_VULKAN_RESIDENT_F64_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/sdm_resident_f64.comp")
    set(FIR_VULKAN_PARTITION_F32_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/fir_partition_f32.comp")
    set(FIR_VULKAN_PARTITION_PRECISE_F32_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/fir_partition_precise_f32.comp")
    set(FIR_VULKAN_PARTITION_PRECISE_F64_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/fir_partition_precise_f64.comp")
    set(VULKAN_DOUBLE_DOUBLE_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/double_double.glsl")
    set(FIR_VULKAN_PARTITION_REFERENCE_DD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/fir_partition_reference_dd.comp")
    set(FIR_VULKAN_SPECTRUM_MULTIPLY_REFERENCE_DD_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/fir_spectrum_multiply_reference_dd.comp")
    set(RATE_VULKAN_POLYPHASE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_polyphase_f64.comp")
    set(RATE_VULKAN_POLYPHASE_F32_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_polyphase_f32.comp")
    set(RATE_VULKAN_POLYPHASE_PRECISE_F32_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_polyphase_precise_f32.comp")
    set(RATE_VULKAN_POLYPHASE_REFERENCE_DD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_polyphase_reference_dd.comp")
    set(RATE_VULKAN_DSD_FIR_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_dsd_fir_f64.comp")
    set(RATE_VULKAN_DSD_FIR_F32_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_dsd_fir_f32.comp")
    set(RATE_VULKAN_DSD_FIR_PRECISE_F32_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_dsd_fir_precise_f32.comp")
    set(RATE_VULKAN_DSD_FIR_REFERENCE_DD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_dsd_fir_reference_dd.comp")
    set(RATE_VULKAN_CUBIC_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_cubic_f64.comp")
    set(RATE_VULKAN_CUBIC_F32_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_cubic_f32.comp")
    set(RATE_VULKAN_CUBIC_PRECISE_F32_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_cubic_precise_f32.comp")
    set(RATE_VULKAN_CUBIC_REFERENCE_DD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_cubic_reference_dd.comp")
    set(RATE_VULKAN_SELECT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_select_f64.comp")
    set(RATE_VULKAN_SELECT_F32_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_select_f32.comp")
    set(RATE_VULKAN_SELECT_PRECISE_F32_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_select_precise_f32.comp")
    set(RATE_VULKAN_SELECT_REFERENCE_DD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_select_reference_dd.comp")
    set(RATE_VULKAN_PREPARE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_prepare_f64.comp")
    set(RATE_VULKAN_PREPARE_F32_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_prepare_f32.comp")
    set(RATE_VULKAN_PREPARE_PRECISE_F32_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_prepare_precise_f32.comp")
    set(RATE_VULKAN_PREPARE_REFERENCE_DD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_prepare_reference_dd.comp")
    set(RATE_VULKAN_STREAM_APPEND_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_stream_append_f64.comp")
    set(RATE_VULKAN_STREAM_APPEND_F32_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_stream_append_f32.comp")
    set(RATE_VULKAN_STREAM_APPEND_PRECISE_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_stream_append_precise_f32.comp")
    set(RATE_VULKAN_STREAM_APPEND_REFERENCE_DD_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_stream_append_reference_dd.comp")
    set(SDM_VULKAN_MASH_SPV "${CMAKE_CURRENT_BINARY_DIR}/sdm_mash2_fsm.comp.spv")
    set(SDM_VULKAN_RESIDENT_SPV "${CMAKE_CURRENT_BINARY_DIR}/sdm_resident_f32.comp.spv")
    set(SDM_VULKAN_RESIDENT_PRECISE_F32_SPV "${CMAKE_CURRENT_BINARY_DIR}/sdm_resident_precise_f32.comp.spv")
    set(SDM_VULKAN_RESIDENT_REFERENCE_DD_SPV "${CMAKE_CURRENT_BINARY_DIR}/sdm_resident_reference_dd.comp.spv")
    set(SDM_VULKAN_RESIDENT_F64_SPV "${CMAKE_CURRENT_BINARY_DIR}/sdm_resident_f64.comp.spv")
    set(FIR_VULKAN_PARTITION_F32_SPV "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_f32.comp.spv")
    set(FIR_VULKAN_PARTITION_PRECISE_F32_SPV "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_precise_f32.comp.spv")
    set(FIR_VULKAN_PARTITION_PRECISE_F64_SPV "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_precise_f64.comp.spv")
    set(FIR_VULKAN_PARTITION_REFERENCE_DD_SPV "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_reference_dd.comp.spv")
    set(FIR_VULKAN_SPECTRUM_MULTIPLY_REFERENCE_DD_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/fir_spectrum_multiply_reference_dd.comp.spv")
    set(RATE_VULKAN_POLYPHASE_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_f64.comp.spv")
    set(RATE_VULKAN_POLYPHASE_F32_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_f32.comp.spv")
    set(RATE_VULKAN_POLYPHASE_PRECISE_F32_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_precise_f32.comp.spv")
    set(RATE_VULKAN_POLYPHASE_REFERENCE_DD_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_reference_dd.comp.spv")
    set(RATE_VULKAN_DSD_FIR_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_dsd_fir_f64.comp.spv")
    set(RATE_VULKAN_DSD_FIR_F32_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_dsd_fir_f32.comp.spv")
    set(RATE_VULKAN_DSD_FIR_PRECISE_F32_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_dsd_fir_precise_f32.comp.spv")
    set(RATE_VULKAN_DSD_FIR_REFERENCE_DD_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_dsd_fir_reference_dd.comp.spv")
    set(RATE_VULKAN_CUBIC_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_cubic_f64.comp.spv")
    set(RATE_VULKAN_CUBIC_F32_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_cubic_f32.comp.spv")
    set(RATE_VULKAN_CUBIC_PRECISE_F32_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_cubic_precise_f32.comp.spv")
    set(RATE_VULKAN_CUBIC_REFERENCE_DD_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_cubic_reference_dd.comp.spv")
    set(RATE_VULKAN_SELECT_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_select_f64.comp.spv")
    set(RATE_VULKAN_SELECT_F32_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_select_f32.comp.spv")
    set(RATE_VULKAN_SELECT_PRECISE_F32_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_select_precise_f32.comp.spv")
    set(RATE_VULKAN_SELECT_REFERENCE_DD_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_select_reference_dd.comp.spv")
    set(RATE_VULKAN_PREPARE_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_prepare_f64.comp.spv")
    set(RATE_VULKAN_PREPARE_F32_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_prepare_f32.comp.spv")
    set(RATE_VULKAN_PREPARE_PRECISE_F32_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_prepare_precise_f32.comp.spv")
    set(RATE_VULKAN_PREPARE_REFERENCE_DD_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_prepare_reference_dd.comp.spv")
    set(RATE_VULKAN_STREAM_APPEND_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_stream_append_f64.comp.spv")
    set(RATE_VULKAN_STREAM_APPEND_F32_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_stream_append_f32.comp.spv")
    set(RATE_VULKAN_STREAM_APPEND_PRECISE_F32_SPV "${CMAKE_CURRENT_BINARY_DIR}/rate_stream_append_precise_f32.comp.spv")
    set(RATE_VULKAN_STREAM_APPEND_REFERENCE_DD_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_stream_append_reference_dd.comp.spv")
    set(SDM_VULKAN_MASH_INC "${CMAKE_CURRENT_BINARY_DIR}/sdm_mash2_fsm_spv.inc")
    set(SDM_VULKAN_RESIDENT_INC "${CMAKE_CURRENT_BINARY_DIR}/sdm_resident_f32_spv.inc")
    set(SDM_VULKAN_RESIDENT_PRECISE_F32_INC "${CMAKE_CURRENT_BINARY_DIR}/sdm_resident_precise_f32_spv.inc")
    set(SDM_VULKAN_RESIDENT_REFERENCE_DD_INC "${CMAKE_CURRENT_BINARY_DIR}/sdm_resident_reference_dd_spv.inc")
    set(SDM_VULKAN_RESIDENT_F64_INC "${CMAKE_CURRENT_BINARY_DIR}/sdm_resident_f64_spv.inc")
    set(FIR_VULKAN_PARTITION_F32_INC "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_f32_spv.inc")
    set(FIR_VULKAN_PARTITION_PRECISE_F32_INC "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_precise_f32_spv.inc")
    set(FIR_VULKAN_PARTITION_PRECISE_F64_INC "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_precise_f64_spv.inc")
    set(FIR_VULKAN_PARTITION_REFERENCE_DD_INC "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_reference_dd_spv.inc")
    set(FIR_VULKAN_SPECTRUM_MULTIPLY_REFERENCE_DD_INC
        "${CMAKE_CURRENT_BINARY_DIR}/fir_spectrum_multiply_reference_dd_spv.inc")
    set(RATE_VULKAN_POLYPHASE_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_f64_spv.inc")
    set(RATE_VULKAN_POLYPHASE_F32_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_f32_spv.inc")
    set(RATE_VULKAN_POLYPHASE_PRECISE_F32_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_precise_f32_spv.inc")
    set(RATE_VULKAN_POLYPHASE_REFERENCE_DD_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_reference_dd_spv.inc")
    set(RATE_VULKAN_DSD_FIR_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_dsd_fir_f64_spv.inc")
    set(RATE_VULKAN_DSD_FIR_F32_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_dsd_fir_f32_spv.inc")
    set(RATE_VULKAN_DSD_FIR_PRECISE_F32_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_dsd_fir_precise_f32_spv.inc")
    set(RATE_VULKAN_DSD_FIR_REFERENCE_DD_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_dsd_fir_reference_dd_spv.inc")
    set(RATE_VULKAN_CUBIC_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_cubic_f64_spv.inc")
    set(RATE_VULKAN_CUBIC_F32_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_cubic_f32_spv.inc")
    set(RATE_VULKAN_CUBIC_PRECISE_F32_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_cubic_precise_f32_spv.inc")
    set(RATE_VULKAN_CUBIC_REFERENCE_DD_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_cubic_reference_dd_spv.inc")
    set(RATE_VULKAN_SELECT_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_select_f64_spv.inc")
    set(RATE_VULKAN_SELECT_F32_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_select_f32_spv.inc")
    set(RATE_VULKAN_SELECT_PRECISE_F32_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_select_precise_f32_spv.inc")
    set(RATE_VULKAN_SELECT_REFERENCE_DD_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_select_reference_dd_spv.inc")
    set(RATE_VULKAN_PREPARE_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_prepare_f64_spv.inc")
    set(RATE_VULKAN_PREPARE_F32_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_prepare_f32_spv.inc")
    set(RATE_VULKAN_PREPARE_PRECISE_F32_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_prepare_precise_f32_spv.inc")
    set(RATE_VULKAN_PREPARE_REFERENCE_DD_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_prepare_reference_dd_spv.inc")
    set(RATE_VULKAN_STREAM_APPEND_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_stream_append_f64_spv.inc")
    set(RATE_VULKAN_STREAM_APPEND_F32_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_stream_append_f32_spv.inc")
    set(RATE_VULKAN_STREAM_APPEND_PRECISE_F32_INC "${CMAKE_CURRENT_BINARY_DIR}/rate_stream_append_precise_f32_spv.inc")
    set(RATE_VULKAN_STREAM_APPEND_REFERENCE_DD_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_stream_append_reference_dd_spv.inc")

    add_executable(sox_embed_binary "${CMAKE_CURRENT_SOURCE_DIR}/embed_binary.c")
    if(MSVC)
        target_compile_definitions(sox_embed_binary PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_custom_command(
        OUTPUT "${SDM_VULKAN_MASH_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${SDM_VULKAN_MASH_SPV}" "${SDM_VULKAN_MASH_SOURCE}"
        DEPENDS "${SDM_VULKAN_MASH_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SDM_VULKAN_RESIDENT_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${SDM_VULKAN_RESIDENT_SPV}" "${SDM_VULKAN_RESIDENT_SOURCE}"
        DEPENDS "${SDM_VULKAN_RESIDENT_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SDM_VULKAN_RESIDENT_PRECISE_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${SDM_VULKAN_RESIDENT_PRECISE_F32_SPV}" "${SDM_VULKAN_RESIDENT_PRECISE_F32_SOURCE}"
        DEPENDS "${SDM_VULKAN_RESIDENT_PRECISE_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SDM_VULKAN_RESIDENT_F64_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${SDM_VULKAN_RESIDENT_F64_SPV}" "${SDM_VULKAN_RESIDENT_F64_SOURCE}"
        DEPENDS "${SDM_VULKAN_RESIDENT_F64_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SDM_VULKAN_RESIDENT_REFERENCE_DD_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${SDM_VULKAN_RESIDENT_REFERENCE_DD_SPV}" "${SDM_VULKAN_RESIDENT_REFERENCE_DD_SOURCE}"
        DEPENDS "${SDM_VULKAN_RESIDENT_REFERENCE_DD_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_PARTITION_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${FIR_VULKAN_PARTITION_F32_SPV}"
                "${FIR_VULKAN_PARTITION_F32_SOURCE}"
        DEPENDS "${FIR_VULKAN_PARTITION_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_PARTITION_PRECISE_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${FIR_VULKAN_PARTITION_PRECISE_F32_SPV}"
                "${FIR_VULKAN_PARTITION_PRECISE_F32_SOURCE}"
        DEPENDS "${FIR_VULKAN_PARTITION_PRECISE_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_PARTITION_PRECISE_F64_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${FIR_VULKAN_PARTITION_PRECISE_F64_SPV}"
                "${FIR_VULKAN_PARTITION_PRECISE_F64_SOURCE}"
        DEPENDS "${FIR_VULKAN_PARTITION_PRECISE_F64_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_PARTITION_REFERENCE_DD_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${FIR_VULKAN_PARTITION_REFERENCE_DD_SPV}"
                "${FIR_VULKAN_PARTITION_REFERENCE_DD_SOURCE}"
        DEPENDS "${FIR_VULKAN_PARTITION_REFERENCE_DD_SOURCE}"
                "${VULKAN_DOUBLE_DOUBLE_HEADER}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_SPECTRUM_MULTIPLY_REFERENCE_DD_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${FIR_VULKAN_SPECTRUM_MULTIPLY_REFERENCE_DD_SPV}"
                "${FIR_VULKAN_SPECTRUM_MULTIPLY_REFERENCE_DD_SOURCE}"
        DEPENDS "${FIR_VULKAN_SPECTRUM_MULTIPLY_REFERENCE_DD_SOURCE}"
                "${VULKAN_DOUBLE_DOUBLE_HEADER}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_POLYPHASE_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${RATE_VULKAN_POLYPHASE_SPV}" "${RATE_VULKAN_POLYPHASE_SOURCE}"
        DEPENDS "${RATE_VULKAN_POLYPHASE_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_POLYPHASE_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${RATE_VULKAN_POLYPHASE_F32_SPV}" "${RATE_VULKAN_POLYPHASE_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_POLYPHASE_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_POLYPHASE_PRECISE_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_POLYPHASE_PRECISE_F32_SPV}"
                "${RATE_VULKAN_POLYPHASE_PRECISE_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_POLYPHASE_PRECISE_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_POLYPHASE_REFERENCE_DD_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_POLYPHASE_REFERENCE_DD_SPV}"
                "${RATE_VULKAN_POLYPHASE_REFERENCE_DD_SOURCE}"
        DEPENDS "${RATE_VULKAN_POLYPHASE_REFERENCE_DD_SOURCE}"
                "${VULKAN_DOUBLE_DOUBLE_HEADER}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_DSD_FIR_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${RATE_VULKAN_DSD_FIR_SPV}" "${RATE_VULKAN_DSD_FIR_SOURCE}"
        DEPENDS "${RATE_VULKAN_DSD_FIR_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_DSD_FIR_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${RATE_VULKAN_DSD_FIR_F32_SPV}" "${RATE_VULKAN_DSD_FIR_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_DSD_FIR_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_DSD_FIR_PRECISE_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${RATE_VULKAN_DSD_FIR_PRECISE_F32_SPV}" "${RATE_VULKAN_DSD_FIR_PRECISE_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_DSD_FIR_PRECISE_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_DSD_FIR_REFERENCE_DD_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_DSD_FIR_REFERENCE_DD_SPV}"
                "${RATE_VULKAN_DSD_FIR_REFERENCE_DD_SOURCE}"
        DEPENDS "${RATE_VULKAN_DSD_FIR_REFERENCE_DD_SOURCE}"
                "${VULKAN_DOUBLE_DOUBLE_HEADER}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_CUBIC_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${RATE_VULKAN_CUBIC_SPV}" "${RATE_VULKAN_CUBIC_SOURCE}"
        DEPENDS "${RATE_VULKAN_CUBIC_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_CUBIC_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${RATE_VULKAN_CUBIC_F32_SPV}" "${RATE_VULKAN_CUBIC_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_CUBIC_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_CUBIC_PRECISE_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_CUBIC_PRECISE_F32_SPV}"
                "${RATE_VULKAN_CUBIC_PRECISE_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_CUBIC_PRECISE_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_CUBIC_REFERENCE_DD_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_CUBIC_REFERENCE_DD_SPV}"
                "${RATE_VULKAN_CUBIC_REFERENCE_DD_SOURCE}"
        DEPENDS "${RATE_VULKAN_CUBIC_REFERENCE_DD_SOURCE}"
                "${VULKAN_DOUBLE_DOUBLE_HEADER}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_SELECT_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${RATE_VULKAN_SELECT_SPV}" "${RATE_VULKAN_SELECT_SOURCE}"
        DEPENDS "${RATE_VULKAN_SELECT_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_SELECT_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${RATE_VULKAN_SELECT_F32_SPV}" "${RATE_VULKAN_SELECT_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_SELECT_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_SELECT_PRECISE_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_SELECT_PRECISE_F32_SPV}"
                "${RATE_VULKAN_SELECT_PRECISE_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_SELECT_PRECISE_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_SELECT_REFERENCE_DD_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_SELECT_REFERENCE_DD_SPV}"
                "${RATE_VULKAN_SELECT_REFERENCE_DD_SOURCE}"
        DEPENDS "${RATE_VULKAN_SELECT_REFERENCE_DD_SOURCE}"
                "${VULKAN_DOUBLE_DOUBLE_HEADER}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_PREPARE_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${RATE_VULKAN_PREPARE_SPV}" "${RATE_VULKAN_PREPARE_SOURCE}"
        DEPENDS "${RATE_VULKAN_PREPARE_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_PREPARE_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${RATE_VULKAN_PREPARE_F32_SPV}" "${RATE_VULKAN_PREPARE_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_PREPARE_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_PREPARE_PRECISE_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_PREPARE_PRECISE_F32_SPV}"
                "${RATE_VULKAN_PREPARE_PRECISE_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_PREPARE_PRECISE_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_PREPARE_REFERENCE_DD_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_PREPARE_REFERENCE_DD_SPV}"
                "${RATE_VULKAN_PREPARE_REFERENCE_DD_SOURCE}"
        DEPENDS "${RATE_VULKAN_PREPARE_REFERENCE_DD_SOURCE}"
                "${VULKAN_DOUBLE_DOUBLE_HEADER}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_STREAM_APPEND_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${RATE_VULKAN_STREAM_APPEND_SPV}" "${RATE_VULKAN_STREAM_APPEND_SOURCE}"
        DEPENDS "${RATE_VULKAN_STREAM_APPEND_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_STREAM_APPEND_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${RATE_VULKAN_STREAM_APPEND_F32_SPV}" "${RATE_VULKAN_STREAM_APPEND_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_STREAM_APPEND_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_STREAM_APPEND_PRECISE_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_STREAM_APPEND_PRECISE_F32_SPV}"
                "${RATE_VULKAN_STREAM_APPEND_PRECISE_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_STREAM_APPEND_PRECISE_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_STREAM_APPEND_REFERENCE_DD_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_STREAM_APPEND_REFERENCE_DD_SPV}"
                "${RATE_VULKAN_STREAM_APPEND_REFERENCE_DD_SOURCE}"
        DEPENDS "${RATE_VULKAN_STREAM_APPEND_REFERENCE_DD_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SDM_VULKAN_MASH_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${SDM_VULKAN_MASH_SPV}" "${SDM_VULKAN_MASH_INC}"
                sdm_vulkan_mash_spv
        DEPENDS sox_embed_binary "${SDM_VULKAN_MASH_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SDM_VULKAN_RESIDENT_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${SDM_VULKAN_RESIDENT_SPV}" "${SDM_VULKAN_RESIDENT_INC}" sdm_vulkan_resident_spv
        DEPENDS sox_embed_binary "${SDM_VULKAN_RESIDENT_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SDM_VULKAN_RESIDENT_PRECISE_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${SDM_VULKAN_RESIDENT_PRECISE_F32_SPV}" "${SDM_VULKAN_RESIDENT_PRECISE_F32_INC}" sdm_vulkan_resident_precise_f32_spv
        DEPENDS sox_embed_binary "${SDM_VULKAN_RESIDENT_PRECISE_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SDM_VULKAN_RESIDENT_F64_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${SDM_VULKAN_RESIDENT_F64_SPV}" "${SDM_VULKAN_RESIDENT_F64_INC}" sdm_vulkan_resident_f64_spv
        DEPENDS sox_embed_binary "${SDM_VULKAN_RESIDENT_F64_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SDM_VULKAN_RESIDENT_REFERENCE_DD_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${SDM_VULKAN_RESIDENT_REFERENCE_DD_SPV}" "${SDM_VULKAN_RESIDENT_REFERENCE_DD_INC}" sdm_vulkan_resident_reference_dd_spv
        DEPENDS sox_embed_binary "${SDM_VULKAN_RESIDENT_REFERENCE_DD_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_PARTITION_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${FIR_VULKAN_PARTITION_F32_SPV}"
                "${FIR_VULKAN_PARTITION_F32_INC}"
                fir_partition_f32_spv
        DEPENDS sox_embed_binary "${FIR_VULKAN_PARTITION_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_PARTITION_PRECISE_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${FIR_VULKAN_PARTITION_PRECISE_F32_SPV}"
                "${FIR_VULKAN_PARTITION_PRECISE_F32_INC}"
                fir_partition_precise_f32_spv
        DEPENDS sox_embed_binary
                "${FIR_VULKAN_PARTITION_PRECISE_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_PARTITION_PRECISE_F64_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${FIR_VULKAN_PARTITION_PRECISE_F64_SPV}"
                "${FIR_VULKAN_PARTITION_PRECISE_F64_INC}"
                fir_partition_precise_f64_spv
        DEPENDS sox_embed_binary
                "${FIR_VULKAN_PARTITION_PRECISE_F64_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_PARTITION_REFERENCE_DD_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${FIR_VULKAN_PARTITION_REFERENCE_DD_SPV}"
                "${FIR_VULKAN_PARTITION_REFERENCE_DD_INC}"
                fir_partition_reference_dd_spv
        DEPENDS sox_embed_binary
                "${FIR_VULKAN_PARTITION_REFERENCE_DD_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_SPECTRUM_MULTIPLY_REFERENCE_DD_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${FIR_VULKAN_SPECTRUM_MULTIPLY_REFERENCE_DD_SPV}"
                "${FIR_VULKAN_SPECTRUM_MULTIPLY_REFERENCE_DD_INC}"
                fir_spectrum_multiply_reference_dd_spv
        DEPENDS sox_embed_binary
                "${FIR_VULKAN_SPECTRUM_MULTIPLY_REFERENCE_DD_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_POLYPHASE_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${RATE_VULKAN_POLYPHASE_SPV}" "${RATE_VULKAN_POLYPHASE_INC}" rate_polyphase_f64_spv
        DEPENDS sox_embed_binary "${RATE_VULKAN_POLYPHASE_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_POLYPHASE_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${RATE_VULKAN_POLYPHASE_F32_SPV}" "${RATE_VULKAN_POLYPHASE_F32_INC}" rate_polyphase_f32_spv
        DEPENDS sox_embed_binary "${RATE_VULKAN_POLYPHASE_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_POLYPHASE_PRECISE_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_POLYPHASE_PRECISE_F32_SPV}"
                "${RATE_VULKAN_POLYPHASE_PRECISE_F32_INC}"
                rate_polyphase_precise_f32_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_POLYPHASE_PRECISE_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_POLYPHASE_REFERENCE_DD_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_POLYPHASE_REFERENCE_DD_SPV}"
                "${RATE_VULKAN_POLYPHASE_REFERENCE_DD_INC}"
                rate_polyphase_reference_dd_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_POLYPHASE_REFERENCE_DD_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_DSD_FIR_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${RATE_VULKAN_DSD_FIR_SPV}" "${RATE_VULKAN_DSD_FIR_INC}" rate_dsd_fir_f64_spv
        DEPENDS sox_embed_binary "${RATE_VULKAN_DSD_FIR_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_DSD_FIR_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${RATE_VULKAN_DSD_FIR_F32_SPV}" "${RATE_VULKAN_DSD_FIR_F32_INC}" rate_dsd_fir_f32_spv
        DEPENDS sox_embed_binary "${RATE_VULKAN_DSD_FIR_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_DSD_FIR_PRECISE_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_DSD_FIR_PRECISE_F32_SPV}"
                "${RATE_VULKAN_DSD_FIR_PRECISE_F32_INC}"
                rate_dsd_fir_precise_f32_spv
        DEPENDS sox_embed_binary "${RATE_VULKAN_DSD_FIR_PRECISE_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_DSD_FIR_REFERENCE_DD_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_DSD_FIR_REFERENCE_DD_SPV}"
                "${RATE_VULKAN_DSD_FIR_REFERENCE_DD_INC}"
                rate_dsd_fir_reference_dd_spv
        DEPENDS sox_embed_binary "${RATE_VULKAN_DSD_FIR_REFERENCE_DD_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_CUBIC_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${RATE_VULKAN_CUBIC_SPV}" "${RATE_VULKAN_CUBIC_INC}" rate_cubic_f64_spv
        DEPENDS sox_embed_binary "${RATE_VULKAN_CUBIC_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_CUBIC_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${RATE_VULKAN_CUBIC_F32_SPV}" "${RATE_VULKAN_CUBIC_F32_INC}" rate_cubic_f32_spv
        DEPENDS sox_embed_binary "${RATE_VULKAN_CUBIC_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_CUBIC_PRECISE_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_CUBIC_PRECISE_F32_SPV}"
                "${RATE_VULKAN_CUBIC_PRECISE_F32_INC}"
                rate_cubic_precise_f32_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_CUBIC_PRECISE_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_CUBIC_REFERENCE_DD_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_CUBIC_REFERENCE_DD_SPV}"
                "${RATE_VULKAN_CUBIC_REFERENCE_DD_INC}"
                rate_cubic_reference_dd_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_CUBIC_REFERENCE_DD_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_SELECT_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${RATE_VULKAN_SELECT_SPV}" "${RATE_VULKAN_SELECT_INC}" rate_select_f64_spv
        DEPENDS sox_embed_binary "${RATE_VULKAN_SELECT_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_SELECT_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${RATE_VULKAN_SELECT_F32_SPV}" "${RATE_VULKAN_SELECT_F32_INC}" rate_select_f32_spv
        DEPENDS sox_embed_binary "${RATE_VULKAN_SELECT_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_SELECT_PRECISE_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_SELECT_PRECISE_F32_SPV}"
                "${RATE_VULKAN_SELECT_PRECISE_F32_INC}"
                rate_select_precise_f32_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_SELECT_PRECISE_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_SELECT_REFERENCE_DD_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_SELECT_REFERENCE_DD_SPV}"
                "${RATE_VULKAN_SELECT_REFERENCE_DD_INC}"
                rate_select_reference_dd_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_SELECT_REFERENCE_DD_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_PREPARE_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${RATE_VULKAN_PREPARE_SPV}" "${RATE_VULKAN_PREPARE_INC}" rate_prepare_f64_spv
        DEPENDS sox_embed_binary "${RATE_VULKAN_PREPARE_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_PREPARE_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${RATE_VULKAN_PREPARE_F32_SPV}" "${RATE_VULKAN_PREPARE_F32_INC}" rate_prepare_f32_spv
        DEPENDS sox_embed_binary "${RATE_VULKAN_PREPARE_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_PREPARE_PRECISE_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_PREPARE_PRECISE_F32_SPV}"
                "${RATE_VULKAN_PREPARE_PRECISE_F32_INC}"
                rate_prepare_precise_f32_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_PREPARE_PRECISE_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_PREPARE_REFERENCE_DD_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_PREPARE_REFERENCE_DD_SPV}"
                "${RATE_VULKAN_PREPARE_REFERENCE_DD_INC}"
                rate_prepare_reference_dd_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_PREPARE_REFERENCE_DD_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_STREAM_APPEND_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${RATE_VULKAN_STREAM_APPEND_SPV}" "${RATE_VULKAN_STREAM_APPEND_INC}" rate_stream_append_f64_spv
        DEPENDS sox_embed_binary "${RATE_VULKAN_STREAM_APPEND_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_STREAM_APPEND_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${RATE_VULKAN_STREAM_APPEND_F32_SPV}" "${RATE_VULKAN_STREAM_APPEND_F32_INC}" rate_stream_append_f32_spv
        DEPENDS sox_embed_binary "${RATE_VULKAN_STREAM_APPEND_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_STREAM_APPEND_PRECISE_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_STREAM_APPEND_PRECISE_F32_SPV}"
                "${RATE_VULKAN_STREAM_APPEND_PRECISE_F32_INC}"
                rate_stream_append_precise_f32_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_STREAM_APPEND_PRECISE_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_STREAM_APPEND_REFERENCE_DD_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_STREAM_APPEND_REFERENCE_DD_SPV}"
                "${RATE_VULKAN_STREAM_APPEND_REFERENCE_DD_INC}"
                rate_stream_append_reference_dd_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_STREAM_APPEND_REFERENCE_DD_SPV}"
        VERBATIM
    )
    add_custom_target(
        sdm_vulkan_shaders
        DEPENDS
            "${SDM_VULKAN_MASH_INC}"
            "${SDM_VULKAN_RESIDENT_INC}"
            "${SDM_VULKAN_RESIDENT_PRECISE_F32_INC}"
            "${SDM_VULKAN_RESIDENT_F64_INC}"
            "${SDM_VULKAN_RESIDENT_REFERENCE_DD_INC}"
            "${FIR_VULKAN_PARTITION_F32_INC}"
            "${FIR_VULKAN_PARTITION_PRECISE_F32_INC}"
            "${FIR_VULKAN_PARTITION_PRECISE_F64_INC}"
            "${FIR_VULKAN_PARTITION_REFERENCE_DD_INC}"
            "${FIR_VULKAN_SPECTRUM_MULTIPLY_REFERENCE_DD_INC}"
            "${RATE_VULKAN_DSD_FIR_INC}"
            "${RATE_VULKAN_DSD_FIR_F32_INC}"
            "${RATE_VULKAN_DSD_FIR_PRECISE_F32_INC}"
            "${RATE_VULKAN_DSD_FIR_REFERENCE_DD_INC}"
            "${RATE_VULKAN_CUBIC_INC}"
            "${RATE_VULKAN_CUBIC_F32_INC}"
            "${RATE_VULKAN_CUBIC_PRECISE_F32_INC}"
            "${RATE_VULKAN_CUBIC_REFERENCE_DD_INC}"
            "${RATE_VULKAN_POLYPHASE_INC}"
            "${RATE_VULKAN_POLYPHASE_F32_INC}"
            "${RATE_VULKAN_POLYPHASE_PRECISE_F32_INC}"
            "${RATE_VULKAN_POLYPHASE_REFERENCE_DD_INC}"
            "${RATE_VULKAN_SELECT_INC}"
            "${RATE_VULKAN_SELECT_F32_INC}"
            "${RATE_VULKAN_SELECT_PRECISE_F32_INC}"
            "${RATE_VULKAN_SELECT_REFERENCE_DD_INC}"
            "${RATE_VULKAN_PREPARE_INC}"
            "${RATE_VULKAN_PREPARE_F32_INC}"
            "${RATE_VULKAN_PREPARE_PRECISE_F32_INC}"
            "${RATE_VULKAN_PREPARE_REFERENCE_DD_INC}"
            "${RATE_VULKAN_STREAM_APPEND_INC}"
            "${RATE_VULKAN_STREAM_APPEND_F32_INC}"
            "${RATE_VULKAN_STREAM_APPEND_PRECISE_F32_INC}"
            "${RATE_VULKAN_STREAM_APPEND_REFERENCE_DD_INC}"
    )
    # The reference profile needs a VkFFT instantiation whose host scalar type
    # is wider than double, which the shim in vulkan_dd/ provides in C++. Every
    # VkFFT entry point is static inline, so this second instantiation stays
    # internal to its own translation unit and leaves vulkan_fft.c untouched.
    enable_language(CXX)
    list(APPEND LIBSOX_EFFECTS_SOURCES
        fir_vulkan.c
        rate_cubic_vulkan.c
        rate_vulkan.c
        rate_polyphase_vulkan.c
        rate_dsd_vulkan.c
        sdm_vulkan.c
        vulkan_fft.c
        vulkan_fft_cache.c
        vulkan_fft_dd.cpp
        rate_design_dd.cpp
        vulkan_engine.c
    )
    set_source_files_properties(vulkan_fft_dd.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/vulkan_dd"
        COMPILE_DEFINITIONS VKFFT_USE_DOUBLEDOUBLE_FP128)
    # The filter designer needs the same shim, and nothing else from VkFFT.
    set_source_files_properties(rate_design_dd.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/vulkan_dd")
    if(IS_WINDOWS)
        list(APPEND LIBSOX_EFFECTS_SOURCES glslang_dynamic.c)
    endif()
    list(APPEND LIBSOX_EFFECTS_HEADERS
        rate_design_dd.h
        fir_vulkan.h
        rate_cubic_vulkan.h
        rate_vulkan.h
        rate_polyphase_vulkan.h
        rate_dsd_vulkan.h
        sdm_vulkan.h
        vulkan_effect_chain.h
        vulkan_fft.h
        vulkan_fft_cache.h
        vulkan_engine.h
    )
endif()

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

if(HAVE_VULKAN)
    add_dependencies(libsox sdm_vulkan_shaders)
    # VkFFT is multi-API and picks its backend from this number; 0 is Vulkan.
    target_compile_definitions(libsox PRIVATE VKFFT_BACKEND=0)
    target_link_libraries(libsox PRIVATE Vulkan::Vulkan)
    if(NOT IS_WINDOWS)
        target_link_libraries(libsox PRIVATE "${GLSLANG_LIBRARY}")
    endif()
    target_include_directories(libsox PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_include_directories(libsox SYSTEM PRIVATE "${VKFFT_INCLUDE_DIR}")
    if(IS_WINDOWS)
        target_include_directories(libsox SYSTEM PRIVATE "$ENV{VULKAN_SDK}/Include/glslang/Include")
    else()
        target_include_directories(libsox SYSTEM PRIVATE
            "${GLSLANG_INCLUDE_DIR}"
            "${GLSLANG_INCLUDE_DIR}/glslang/Include")
    endif()
endif()

# The target name already carries "lib": without the platform prefix the
# files are libsox.a, libsox.dylib and libsox.so.4 on Unix and libsox.lib and
# libsox.dll on Windows (MSVC has no prefix, MinGW's import library becomes
# libsox.dll.a).
set_target_properties(libsox PROPERTIES
    OUTPUT_NAME libsox
    PREFIX ""
    IMPORT_PREFIX ""
    VERSION ${SOX_VERSION_MAJOR}.${SOX_VERSION_MINOR}.${SOX_VERSION_PATCH}
    SOVERSION ${SOX_VERSION_MAJOR}
)

# Include directories
target_include_directories(libsox PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

# Add the configured static dependency prefix to the library search path.
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
    # Some MSVC CMake generators find OpenMP and define HAVE_OPENMP but leave
    # the imported target's compile option out of a static-library vcxproj.
    # The pragmas then compile as comments and --multi-threaded runs on one
    # thread.  Reapply the flag CMake detected to the target that contains
    # the parallel effects loops.
    if(MSVC AND OpenMP_C_FLAGS)
        target_compile_options(libsox PRIVATE ${OpenMP_C_FLAGS})
    endif()
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
    target_sources(libsox PRIVATE flac.c codec-flac.c)
    target_link_libraries(libsox PRIVATE ${FLAC_LIBRARIES})
    target_include_directories(libsox PRIVATE ${FLAC_INCLUDE_DIRS})
    target_compile_definitions(libsox PRIVATE STATIC_FLAC FLAC__NO_DLL)
endif()

# Ogg container, with the Vorbis and Opus codecs it and Matroska hold
if(HAVE_OGG_VORBIS OR HAVE_OPUS)
    target_sources(libsox PRIVATE ogg.c)
    target_compile_definitions(libsox PRIVATE STATIC_OGG)
endif()
if(HAVE_OGG_VORBIS)
    target_sources(libsox PRIVATE codec-vorbis.c)
    target_link_libraries(libsox PRIVATE ${OGG_VORBIS_LIBRARIES})
    target_include_directories(libsox PRIVATE ${OGG_VORBIS_INCLUDE_DIRS})
    target_compile_definitions(libsox PRIVATE STATIC_OGG_VORBIS)
endif()
if(HAVE_OPUS)
    target_sources(libsox PRIVATE codec-opus.c)
    target_link_libraries(libsox PRIVATE ${OPUS_LIBRARIES})
    target_include_directories(libsox PRIVATE ${OPUS_INCLUDE_DIRS})
    target_compile_definitions(libsox PRIVATE STATIC_OPUS)
endif()

# FFmpeg libavcodec formats.
#
# STATIC_FFMPEG_CODECS is what formats.h tests to register these handlers in
# the built-in table instead of as libltdl plugins, like STATIC_MP3 and the
# rest above; it is unrelated to how FFmpeg itself was built.  The handlers
# are compiled into libsox either way.
if(HAVE_FFMPEG_CODECS)
    target_sources(libsox PRIVATE
        ffmpeg-codec.c
        codec-ffmpeg.c
        latm-common.c
        aac.c
        ac3.c
        dts-common.c
        dts.c
        dtshd.c
        eac3.c
        latm.c
        mlp-common.c
        mlp.c
        truehd.c
        usac.c
    )
    target_link_libraries(libsox PRIVATE ${FFMPEG_CODECS_LIBRARIES})
    target_include_directories(libsox PRIVATE ${FFMPEG_CODECS_INCLUDE_DIRS})
    target_compile_definitions(libsox PRIVATE STATIC_FFMPEG_CODECS)
    # A static libavutil calls into bcrypt for its random number generator,
    # which the Windows SDK does not link implicitly.
    if(IS_WINDOWS)
        target_link_libraries(libsox PRIVATE bcrypt)
    endif()
endif()

# Monkey's Audio format, with the SDK's C++ frame decoder and encoder
if(HAVE_APE)
    enable_language(CXX)
    target_sources(libsox PRIVATE ape.c codec-ape.c ape-mac.cpp)
    # The SDK runs its own worker threads; glibc before 2.34 keeps them in libpthread
    find_package(Threads REQUIRED)
    target_link_libraries(libsox PRIVATE ${MAC_LIBRARY} Threads::Threads)
    target_include_directories(libsox PRIVATE ${MAC_INCLUDE_DIR})
    target_compile_definitions(libsox PRIVATE STATIC_APE)
    # The SDK builds itself with PLATFORM_LINUX on every Unix but macOS, while
    # its All.h detects only __linux__ and falls back to Windows elsewhere
    # (FreeBSD): SoX's side has to name the same platform.
    if(NOT WIN32 AND NOT APPLE)
        target_compile_definitions(libsox PRIVATE PLATFORM_LINUX)
    endif()
endif()

# Musepack format
if(HAVE_MUSEPACK)
    target_sources(libsox PRIVATE musepack.c codec-musepack.c)
    target_link_libraries(libsox PRIVATE ${MPCDEC_LIBRARY})
    target_include_directories(libsox PRIVATE ${MPCDEC_INCLUDE_DIR})
    target_compile_definitions(libsox PRIVATE STATIC_MUSEPACK)
    if(HAVE_MPC_ENCODER)
        target_link_libraries(libsox PRIVATE ${MPCENC_LIBRARY} ${MPCPSY_LIBRARY})
        target_include_directories(libsox PRIVATE ${MPCENC_INCLUDE_DIR})
        # The math the libraries were built with, as mpcenc is.
        set_source_files_properties(codec-musepack.c PROPERTIES COMPILE_DEFINITIONS FAST_MATH)
    endif()
endif()

# MP3 format (mad/lame/twolame)
if(HAVE_MP3)
    target_sources(libsox PRIVATE mp3.c codec-mp3.c mpeg-audio.c)
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
        target_compile_definitions(libsox PRIVATE LIBTWOLAME_STATIC)
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
    target_sources(libsox PRIVATE wavpack.c codec-wavpack.c)
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
        win32-unicode.c
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

# The commit the binary was built from, for --diagnostics to record.  Resolved
# at configure time; a build without git has no sox.commit key, which means
# "not measured".
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=8 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE SOX_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(SOX_GIT_COMMIT)
        target_compile_definitions(sox_exe PRIVATE SOX_GIT_COMMIT="${SOX_GIT_COMMIT}")
    endif()
endif()

set_target_properties(sox_exe PROPERTIES
    OUTPUT_NAME sox
)
# Add static_libs to link path for sox_exe
if(EXISTS "${STATIC_LIBS_DIR}/lib")
    target_link_directories(sox_exe PRIVATE "${STATIC_LIBS_DIR}/lib")
endif()

target_link_libraries(sox_exe PRIVATE libsox)

# sox.c includes libsox's internal headers, and soxomp.h includes <omp.h>
# whenever HAVE_OPENMP is set; with Homebrew's libomp that header is outside
# the compiler's default search path.
if(HAVE_OPENMP AND OpenMP_C_FOUND)
    target_link_libraries(sox_exe PRIVATE OpenMP::OpenMP_C)
endif()

# glslang_dynamic.c resolves the DLL by name at run time, so these two steps
# copy it next to the executable, for the build tree and for an install.
# Without it a Vulkan effect fails when VkFFT first compiles a shader.
if(HAVE_VULKAN AND IS_WINDOWS)
    add_custom_command(TARGET sox_exe POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${GLSLANG_RUNTIME_DLL}" "$<TARGET_FILE_DIR:sox_exe>"
        VERBATIM
    )
endif()

# pthread is needed by WavPack on the BSD platforms.
if(HAVE_WAVPACK AND CMAKE_SYSTEM_NAME MATCHES "FreeBSD|OpenBSD|NetBSD|DragonFly")
    target_link_libraries(libsox PRIVATE pthread)
endif()

# =====================================================================
# pkg-config metadata
# =====================================================================

# sox.pc describes what a static libsox needs at link time, taken from what
# libsox links: a PkgConfig:: target becomes its modules in Requires.private,
# anything else -L and -l (or -framework) entries in Libs.private.  Imported
# and in-tree targets are followed through their link interfaces.
function(sox_pc_collect items)
    foreach(item IN LISTS items)
        if(item MATCHES "^\\$<LINK_ONLY:(.*)>$")
            set(item "${CMAKE_MATCH_1}")
        endif()
        if(item STREQUAL "" OR item MATCHES "\\$<")
            continue()
        endif()

        if(TARGET "${item}")
            if(item MATCHES "^PkgConfig::(.*)$")
                set(modules "${CMAKE_MATCH_1}_PC_MODULES")
                if(DEFINED ${modules})
                    set_property(GLOBAL APPEND PROPERTY SOX_PC_REQUIRES ${${modules}})
                    continue()
                endif()
            endif()

            # A target is followed every time it is named, so that its
            # libraries end up after the last library that needs them; the
            # guard only stops a target from being followed inside itself.
            get_property(visiting GLOBAL PROPERTY SOX_PC_VISITING)
            if("${item}" IN_LIST visiting)
                continue()
            endif()
            set_property(GLOBAL APPEND PROPERTY SOX_PC_VISITING "${item}")

            get_target_property(aliased "${item}" ALIASED_TARGET)
            get_target_property(type "${item}" TYPE)
            get_target_property(imported "${item}" IMPORTED)
            if(aliased)
                sox_pc_collect("${aliased}")
            elseif(imported)
                if(NOT type STREQUAL "INTERFACE_LIBRARY")
                    get_target_property(configs "${item}" IMPORTED_CONFIGURATIONS)
                    set(location "")
                    if(configs)
                        if("RELEASE" IN_LIST configs)
                            set(config RELEASE)
                        else()
                            list(GET configs 0 config)
                        endif()
                        get_target_property(location "${item}" IMPORTED_LOCATION_${config})
                    endif()
                    if(NOT location)
                        get_target_property(location "${item}" IMPORTED_LOCATION)
                    endif()
                    if(location)
                        sox_pc_collect("${location}")
                    endif()
                endif()
            elseif(type STREQUAL "STATIC_LIBRARY")
                get_target_property(name "${item}" OUTPUT_NAME)
                if(NOT name)
                    set(name "${item}")
                endif()
                set_property(GLOBAL APPEND PROPERTY SOX_PC_LIBS "-l${name}")
            endif()
            if(NOT aliased)
                get_target_property(interface "${item}" INTERFACE_LINK_LIBRARIES)
                if(interface)
                    sox_pc_collect("${interface}")
                endif()
            endif()

            get_property(visiting GLOBAL PROPERTY SOX_PC_VISITING)
            list(REMOVE_ITEM visiting "${item}")
            set_property(GLOBAL PROPERTY SOX_PC_VISITING "${visiting}")
        elseif(item MATCHES "^-")
            set_property(GLOBAL APPEND PROPERTY SOX_PC_LIBS "${item}")
        elseif(IS_ABSOLUTE "${item}")
            get_filename_component(dir "${item}" DIRECTORY)
            get_filename_component(name "${item}" NAME_WE)
            if(item MATCHES "\\.framework$")
                set_property(GLOBAL APPEND PROPERTY SOX_PC_LIBS "-framework ${name}")
                continue()
            endif()
            if(CMAKE_STATIC_LIBRARY_PREFIX AND name MATCHES "^${CMAKE_STATIC_LIBRARY_PREFIX}(.+)$")
                set(name "${CMAKE_MATCH_1}")
            endif()
            if(NOT dir IN_LIST CMAKE_C_IMPLICIT_LINK_DIRECTORIES)
                set_property(GLOBAL APPEND PROPERTY SOX_PC_DIRS "-L${dir}")
            endif()
            set_property(GLOBAL APPEND PROPERTY SOX_PC_LIBS "-l${name}")
        else()
            set_property(GLOBAL APPEND PROPERTY SOX_PC_LIBS "-l${item}")
        endif()
    endforeach()
endfunction()

# A shared libsox carries its own dependencies, so its private fields stay
# empty and a consumer needs no .pc file of them.
get_target_property(SOX_PC_TYPE libsox TYPE)
if(SOX_PC_TYPE STREQUAL "STATIC_LIBRARY")
    get_target_property(SOX_PC_LINK_DIRECTORIES libsox LINK_DIRECTORIES)
    foreach(dir IN LISTS SOX_PC_LINK_DIRECTORIES)
        set_property(GLOBAL APPEND PROPERTY SOX_PC_DIRS "-L${dir}")
    endforeach()
    get_target_property(SOX_PC_LINK_LIBRARIES libsox LINK_LIBRARIES)
    sox_pc_collect("${SOX_PC_LINK_LIBRARIES}")
    # The C++ sources (Monkey's Audio) need the C++ runtime that a C link of
    # a static libsox does not add.
    get_property(SOX_PC_LANGUAGES GLOBAL PROPERTY ENABLED_LANGUAGES)
    if("CXX" IN_LIST SOX_PC_LANGUAGES)
        foreach(lib IN LISTS CMAKE_CXX_IMPLICIT_LINK_LIBRARIES)
            if(NOT lib IN_LIST CMAKE_C_IMPLICIT_LINK_LIBRARIES)
                sox_pc_collect("${lib}")
            endif()
        endforeach()
    endif()
endif()

get_property(SOX_PC_DIRS GLOBAL PROPERTY SOX_PC_DIRS)
get_property(SOX_PC_LIBS GLOBAL PROPERTY SOX_PC_LIBS)
get_property(SOX_PC_REQUIRES GLOBAL PROPERTY SOX_PC_REQUIRES)
list(REMOVE_DUPLICATES SOX_PC_DIRS)
# A static link resolves left to right, so a library named twice stays
# where it is named last.
list(REVERSE SOX_PC_LIBS)
list(REMOVE_DUPLICATES SOX_PC_LIBS)
list(REVERSE SOX_PC_LIBS)
list(REMOVE_DUPLICATES SOX_PC_REQUIRES)
string(REPLACE ";" " " SOX_PC_LIBS_PRIVATE "${SOX_PC_DIRS};${SOX_PC_LIBS}")
string(STRIP "${SOX_PC_LIBS_PRIVATE}" SOX_PC_LIBS_PRIVATE)
# pkg_check_modules takes "libavcodec>=61", a .pc file "libavcodec >= 61".
list(TRANSFORM SOX_PC_REQUIRES REPLACE "^([^<>=!]+)([<>=!]+)(.+)$" "\\1 \\2 \\3")
string(REPLACE ";" ", " SOX_PC_REQUIRES_PRIVATE "${SOX_PC_REQUIRES}")

if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(SOX_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
else()
    set(SOX_PC_LIBDIR "\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}")
endif()
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(SOX_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
else()
    set(SOX_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()

configure_file(
    ${PROJECT_SOURCE_DIR}/sox.pc.in
    ${PROJECT_BINARY_DIR}/sox.pc
    @ONLY
)

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

if(HAVE_VULKAN AND IS_WINDOWS)
    install(FILES "${GLSLANG_RUNTIME_DLL}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# Install header
install(FILES sox.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES ${PROJECT_BINARY_DIR}/sox.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
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
