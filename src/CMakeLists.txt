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
    ffmpeg-codec.h
)

if(HAVE_VULKAN)
    set(SDM_VULKAN_FIR_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/sdm_polyphase.comp")
    set(SDM_VULKAN_MASH_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/sdm_mash2_fsm.comp")
    set(SDM_VULKAN_RESIDENT_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/sdm_resident_f32.comp")
    set(SDM_VULKAN_RESIDENT_STRICT_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/sdm_resident_strict_f32.comp")
    set(SDM_VULKAN_SPECIALIZED_STREAM_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/sdm_specialized_stream_f64.comp")
    set(FIR_VULKAN_PARTITION_F64_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/fir_partition_f64.comp")
    set(FIR_VULKAN_PARTITION_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/fir_partition_f32.comp")
    set(FIR_VULKAN_PARTITION_ACCURATE_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/fir_partition_accurate_f32.comp")
    set(FIR_VULKAN_PARTITION_STRICT_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/fir_partition_strict_f32.comp")
    set(FIR_VULKAN_PARTITION_SPLIT_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/fir_partition_split_f32.comp")
    set(FIR_VULKAN_PARTITION_PRECISE_F64_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/fir_partition_precise_f64.comp")
    set(FIR_VULKAN_DIRECT_REFERENCE_F64_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/fir_direct_reference_f64.comp")
    set(FIR_VULKAN_DIRECT_STRICT_F64_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/fir_direct_strict_f64.comp")
    set(FIR_VULKAN_DIRECT_ACCURATE_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/fir_direct_accurate_f32.comp")
    set(RATE_VULKAN_POLYPHASE_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_polyphase_f64.comp")
    set(RATE_VULKAN_POLYPHASE_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_polyphase_f32.comp")
    set(RATE_VULKAN_POLYPHASE_ACCURATE_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_polyphase_accurate_f32.comp")
    set(RATE_VULKAN_POLYPHASE_STRICT_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_polyphase_strict_f32.comp")
    set(RATE_VULKAN_CUBIC_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_cubic_f64.comp")
    set(RATE_VULKAN_CUBIC_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_cubic_f32.comp")
    set(RATE_VULKAN_CUBIC_STRICT_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_cubic_strict_f32.comp")
    set(RATE_VULKAN_POLYPHASE_NORMALIZED_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_polyphase_normalized_f32.comp")
    set(RATE_VULKAN_SELECT_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_select_f64.comp")
    set(RATE_VULKAN_SELECT_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_select_f32.comp")
    set(RATE_VULKAN_SELECT_STRICT_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_select_strict_f32.comp")
    set(RATE_VULKAN_PREPARE_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_prepare_f64.comp")
    set(RATE_VULKAN_PREPARE_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_prepare_f32.comp")
    set(RATE_VULKAN_PREPARE_STRICT_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_prepare_strict_f32.comp")
    set(RATE_VULKAN_STREAM_APPEND_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_stream_append_f64.comp")
    set(RATE_VULKAN_STREAM_APPEND_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_stream_append_f32.comp")
    set(RATE_VULKAN_STREAM_APPEND_STRICT_F32_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/rate_stream_append_strict_f32.comp")
    set(SDM_VULKAN_FIR_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/sdm_polyphase.comp.spv")
    set(SDM_VULKAN_MASH_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/sdm_mash2_fsm.comp.spv")
    set(SDM_VULKAN_RESIDENT_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/sdm_resident_f32.comp.spv")
    set(SDM_VULKAN_RESIDENT_STRICT_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/sdm_resident_strict_f32.comp.spv")
    set(SDM_VULKAN_SPECIALIZED_STREAM_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/sdm_specialized_stream_f64.comp.spv")
    set(FIR_VULKAN_PARTITION_F64_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_f64.comp.spv")
    set(FIR_VULKAN_PARTITION_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_f32.comp.spv")
    set(FIR_VULKAN_PARTITION_ACCURATE_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_accurate_f32.comp.spv")
    set(FIR_VULKAN_PARTITION_STRICT_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_strict_f32.comp.spv")
    set(FIR_VULKAN_PARTITION_SPLIT_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_split_f32.comp.spv")
    set(FIR_VULKAN_PARTITION_PRECISE_F64_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_precise_f64.comp.spv")
    set(FIR_VULKAN_DIRECT_REFERENCE_F64_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/fir_direct_reference_f64.comp.spv")
    set(FIR_VULKAN_DIRECT_STRICT_F64_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/fir_direct_strict_f64.comp.spv")
    set(FIR_VULKAN_DIRECT_ACCURATE_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/fir_direct_accurate_f32.comp.spv")
    set(RATE_VULKAN_POLYPHASE_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_f64.comp.spv")
    set(RATE_VULKAN_POLYPHASE_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_f32.comp.spv")
    set(RATE_VULKAN_POLYPHASE_ACCURATE_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_accurate_f32.comp.spv")
    set(RATE_VULKAN_POLYPHASE_STRICT_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_strict_f32.comp.spv")
    set(RATE_VULKAN_CUBIC_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_cubic_f64.comp.spv")
    set(RATE_VULKAN_CUBIC_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_cubic_f32.comp.spv")
    set(RATE_VULKAN_CUBIC_STRICT_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_cubic_strict_f32.comp.spv")
    set(RATE_VULKAN_POLYPHASE_NORMALIZED_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_normalized_f32.comp.spv")
    set(RATE_VULKAN_SELECT_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_select_f64.comp.spv")
    set(RATE_VULKAN_SELECT_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_select_f32.comp.spv")
    set(RATE_VULKAN_SELECT_STRICT_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_select_strict_f32.comp.spv")
    set(RATE_VULKAN_PREPARE_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_prepare_f64.comp.spv")
    set(RATE_VULKAN_PREPARE_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_prepare_f32.comp.spv")
    set(RATE_VULKAN_PREPARE_STRICT_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_prepare_strict_f32.comp.spv")
    set(RATE_VULKAN_STREAM_APPEND_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_stream_append_f64.comp.spv")
    set(RATE_VULKAN_STREAM_APPEND_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_stream_append_f32.comp.spv")
    set(RATE_VULKAN_STREAM_APPEND_STRICT_F32_SPV
        "${CMAKE_CURRENT_BINARY_DIR}/rate_stream_append_strict_f32.comp.spv")
    set(SDM_VULKAN_FIR_INC
        "${CMAKE_CURRENT_BINARY_DIR}/sdm_polyphase_spv.inc")
    set(SDM_VULKAN_MASH_INC
        "${CMAKE_CURRENT_BINARY_DIR}/sdm_mash2_fsm_spv.inc")
    set(SDM_VULKAN_RESIDENT_INC
        "${CMAKE_CURRENT_BINARY_DIR}/sdm_resident_f32_spv.inc")
    set(SDM_VULKAN_RESIDENT_STRICT_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/sdm_resident_strict_f32_spv.inc")
    set(SDM_VULKAN_SPECIALIZED_STREAM_INC
        "${CMAKE_CURRENT_BINARY_DIR}/sdm_specialized_stream_f64_spv.inc")
    set(FIR_VULKAN_PARTITION_F64_INC
        "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_f64_spv.inc")
    set(FIR_VULKAN_PARTITION_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_f32_spv.inc")
    set(FIR_VULKAN_PARTITION_ACCURATE_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_accurate_f32_spv.inc")
    set(FIR_VULKAN_PARTITION_STRICT_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_strict_f32_spv.inc")
    set(FIR_VULKAN_PARTITION_SPLIT_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_split_f32_spv.inc")
    set(FIR_VULKAN_PARTITION_PRECISE_F64_INC
        "${CMAKE_CURRENT_BINARY_DIR}/fir_partition_precise_f64_spv.inc")
    set(FIR_VULKAN_DIRECT_REFERENCE_F64_INC
        "${CMAKE_CURRENT_BINARY_DIR}/fir_direct_reference_f64_spv.inc")
    set(FIR_VULKAN_DIRECT_STRICT_F64_INC
        "${CMAKE_CURRENT_BINARY_DIR}/fir_direct_strict_f64_spv.inc")
    set(FIR_VULKAN_DIRECT_ACCURATE_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/fir_direct_accurate_f32_spv.inc")
    set(RATE_VULKAN_POLYPHASE_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_f64_spv.inc")
    set(RATE_VULKAN_POLYPHASE_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_f32_spv.inc")
    set(RATE_VULKAN_POLYPHASE_ACCURATE_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_accurate_f32_spv.inc")
    set(RATE_VULKAN_POLYPHASE_STRICT_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_strict_f32_spv.inc")
    set(RATE_VULKAN_CUBIC_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_cubic_f64_spv.inc")
    set(RATE_VULKAN_CUBIC_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_cubic_f32_spv.inc")
    set(RATE_VULKAN_CUBIC_STRICT_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_cubic_strict_f32_spv.inc")
    set(RATE_VULKAN_POLYPHASE_NORMALIZED_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_polyphase_normalized_f32_spv.inc")
    set(RATE_VULKAN_SELECT_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_select_f64_spv.inc")
    set(RATE_VULKAN_SELECT_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_select_f32_spv.inc")
    set(RATE_VULKAN_SELECT_STRICT_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_select_strict_f32_spv.inc")
    set(RATE_VULKAN_PREPARE_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_prepare_f64_spv.inc")
    set(RATE_VULKAN_PREPARE_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_prepare_f32_spv.inc")
    set(RATE_VULKAN_PREPARE_STRICT_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_prepare_strict_f32_spv.inc")
    set(RATE_VULKAN_STREAM_APPEND_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_stream_append_f64_spv.inc")
    set(RATE_VULKAN_STREAM_APPEND_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_stream_append_f32_spv.inc")
    set(RATE_VULKAN_STREAM_APPEND_STRICT_F32_INC
        "${CMAKE_CURRENT_BINARY_DIR}/rate_stream_append_strict_f32_spv.inc")

    add_executable(sox_embed_binary
        "${CMAKE_CURRENT_SOURCE_DIR}/embed_binary.c")
    if(MSVC)
        target_compile_definitions(
            sox_embed_binary PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_custom_command(
        OUTPUT "${SDM_VULKAN_FIR_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${SDM_VULKAN_FIR_SPV}" "${SDM_VULKAN_FIR_SOURCE}"
        DEPENDS "${SDM_VULKAN_FIR_SOURCE}"
        VERBATIM
    )
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
        OUTPUT "${SDM_VULKAN_RESIDENT_STRICT_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${SDM_VULKAN_RESIDENT_STRICT_F32_SPV}" "${SDM_VULKAN_RESIDENT_STRICT_F32_SOURCE}"
        DEPENDS "${SDM_VULKAN_RESIDENT_STRICT_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SDM_VULKAN_SPECIALIZED_STREAM_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${SDM_VULKAN_SPECIALIZED_STREAM_SPV}" "${SDM_VULKAN_SPECIALIZED_STREAM_SOURCE}"
        DEPENDS "${SDM_VULKAN_SPECIALIZED_STREAM_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_PARTITION_F64_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${FIR_VULKAN_PARTITION_F64_SPV}"
                "${FIR_VULKAN_PARTITION_F64_SOURCE}"
        DEPENDS "${FIR_VULKAN_PARTITION_F64_SOURCE}"
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
        OUTPUT "${FIR_VULKAN_PARTITION_ACCURATE_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${FIR_VULKAN_PARTITION_ACCURATE_F32_SPV}"
                "${FIR_VULKAN_PARTITION_ACCURATE_F32_SOURCE}"
        DEPENDS "${FIR_VULKAN_PARTITION_ACCURATE_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_PARTITION_STRICT_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${FIR_VULKAN_PARTITION_STRICT_F32_SPV}"
                "${FIR_VULKAN_PARTITION_STRICT_F32_SOURCE}"
        DEPENDS "${FIR_VULKAN_PARTITION_STRICT_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_PARTITION_SPLIT_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${FIR_VULKAN_PARTITION_SPLIT_F32_SPV}"
                "${FIR_VULKAN_PARTITION_SPLIT_F32_SOURCE}"
        DEPENDS "${FIR_VULKAN_PARTITION_SPLIT_F32_SOURCE}"
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
        OUTPUT "${FIR_VULKAN_DIRECT_REFERENCE_F64_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${FIR_VULKAN_DIRECT_REFERENCE_F64_SPV}"
                "${FIR_VULKAN_DIRECT_REFERENCE_F64_SOURCE}"
        DEPENDS "${FIR_VULKAN_DIRECT_REFERENCE_F64_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_DIRECT_STRICT_F64_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${FIR_VULKAN_DIRECT_STRICT_F64_SPV}"
                "${FIR_VULKAN_DIRECT_STRICT_F64_SOURCE}"
        DEPENDS "${FIR_VULKAN_DIRECT_STRICT_F64_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_DIRECT_ACCURATE_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${FIR_VULKAN_DIRECT_ACCURATE_F32_SPV}"
                "${FIR_VULKAN_DIRECT_ACCURATE_F32_SOURCE}"
        DEPENDS "${FIR_VULKAN_DIRECT_ACCURATE_F32_SOURCE}"
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
        OUTPUT "${RATE_VULKAN_POLYPHASE_ACCURATE_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_POLYPHASE_ACCURATE_F32_SPV}"
                "${RATE_VULKAN_POLYPHASE_ACCURATE_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_POLYPHASE_ACCURATE_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_POLYPHASE_STRICT_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_POLYPHASE_STRICT_F32_SPV}"
                "${RATE_VULKAN_POLYPHASE_STRICT_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_POLYPHASE_STRICT_F32_SOURCE}"
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
        OUTPUT "${RATE_VULKAN_CUBIC_STRICT_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_CUBIC_STRICT_F32_SPV}"
                "${RATE_VULKAN_CUBIC_STRICT_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_CUBIC_STRICT_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_POLYPHASE_NORMALIZED_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1 -o "${RATE_VULKAN_POLYPHASE_NORMALIZED_SPV}" "${RATE_VULKAN_POLYPHASE_NORMALIZED_SOURCE}"
        DEPENDS "${RATE_VULKAN_POLYPHASE_NORMALIZED_SOURCE}"
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
        OUTPUT "${RATE_VULKAN_SELECT_STRICT_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_SELECT_STRICT_F32_SPV}"
                "${RATE_VULKAN_SELECT_STRICT_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_SELECT_STRICT_F32_SOURCE}"
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
        OUTPUT "${RATE_VULKAN_PREPARE_STRICT_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_PREPARE_STRICT_F32_SPV}"
                "${RATE_VULKAN_PREPARE_STRICT_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_PREPARE_STRICT_F32_SOURCE}"
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
        OUTPUT "${RATE_VULKAN_STREAM_APPEND_STRICT_F32_SPV}"
        COMMAND "${GLSLC_EXECUTABLE}" -O --target-env=vulkan1.1
                -o "${RATE_VULKAN_STREAM_APPEND_STRICT_F32_SPV}"
                "${RATE_VULKAN_STREAM_APPEND_STRICT_F32_SOURCE}"
        DEPENDS "${RATE_VULKAN_STREAM_APPEND_STRICT_F32_SOURCE}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SDM_VULKAN_FIR_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${SDM_VULKAN_FIR_SPV}" "${SDM_VULKAN_FIR_INC}"
                sdm_vulkan_fir_spv
        DEPENDS sox_embed_binary "${SDM_VULKAN_FIR_SPV}"
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
        OUTPUT "${SDM_VULKAN_RESIDENT_STRICT_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${SDM_VULKAN_RESIDENT_STRICT_F32_SPV}" "${SDM_VULKAN_RESIDENT_STRICT_F32_INC}" sdm_vulkan_resident_strict_f32_spv
        DEPENDS sox_embed_binary "${SDM_VULKAN_RESIDENT_STRICT_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SDM_VULKAN_SPECIALIZED_STREAM_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${SDM_VULKAN_SPECIALIZED_STREAM_SPV}" "${SDM_VULKAN_SPECIALIZED_STREAM_INC}" sdm_specialized_stream_f64_spv
        DEPENDS sox_embed_binary "${SDM_VULKAN_SPECIALIZED_STREAM_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_PARTITION_F64_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${FIR_VULKAN_PARTITION_F64_SPV}"
                "${FIR_VULKAN_PARTITION_F64_INC}"
                fir_partition_f64_spv
        DEPENDS sox_embed_binary "${FIR_VULKAN_PARTITION_F64_SPV}"
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
        OUTPUT "${FIR_VULKAN_PARTITION_ACCURATE_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${FIR_VULKAN_PARTITION_ACCURATE_F32_SPV}"
                "${FIR_VULKAN_PARTITION_ACCURATE_F32_INC}"
                fir_partition_accurate_f32_spv
        DEPENDS sox_embed_binary
                "${FIR_VULKAN_PARTITION_ACCURATE_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_PARTITION_STRICT_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${FIR_VULKAN_PARTITION_STRICT_F32_SPV}"
                "${FIR_VULKAN_PARTITION_STRICT_F32_INC}"
                fir_partition_strict_f32_spv
        DEPENDS sox_embed_binary
                "${FIR_VULKAN_PARTITION_STRICT_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_PARTITION_SPLIT_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${FIR_VULKAN_PARTITION_SPLIT_F32_SPV}"
                "${FIR_VULKAN_PARTITION_SPLIT_F32_INC}"
                fir_partition_split_f32_spv
        DEPENDS sox_embed_binary
                "${FIR_VULKAN_PARTITION_SPLIT_F32_SPV}"
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
        OUTPUT "${FIR_VULKAN_DIRECT_REFERENCE_F64_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${FIR_VULKAN_DIRECT_REFERENCE_F64_SPV}"
                "${FIR_VULKAN_DIRECT_REFERENCE_F64_INC}"
                fir_direct_reference_f64_spv
        DEPENDS sox_embed_binary
                "${FIR_VULKAN_DIRECT_REFERENCE_F64_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_DIRECT_STRICT_F64_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${FIR_VULKAN_DIRECT_STRICT_F64_SPV}"
                "${FIR_VULKAN_DIRECT_STRICT_F64_INC}"
                fir_direct_strict_f64_spv
        DEPENDS sox_embed_binary
                "${FIR_VULKAN_DIRECT_STRICT_F64_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FIR_VULKAN_DIRECT_ACCURATE_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${FIR_VULKAN_DIRECT_ACCURATE_F32_SPV}"
                "${FIR_VULKAN_DIRECT_ACCURATE_F32_INC}"
                fir_direct_accurate_f32_spv
        DEPENDS sox_embed_binary
                "${FIR_VULKAN_DIRECT_ACCURATE_F32_SPV}"
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
        OUTPUT "${RATE_VULKAN_POLYPHASE_ACCURATE_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_POLYPHASE_ACCURATE_F32_SPV}"
                "${RATE_VULKAN_POLYPHASE_ACCURATE_F32_INC}"
                rate_polyphase_accurate_f32_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_POLYPHASE_ACCURATE_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_POLYPHASE_STRICT_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_POLYPHASE_STRICT_F32_SPV}"
                "${RATE_VULKAN_POLYPHASE_STRICT_F32_INC}"
                rate_polyphase_strict_f32_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_POLYPHASE_STRICT_F32_SPV}"
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
        OUTPUT "${RATE_VULKAN_CUBIC_STRICT_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_CUBIC_STRICT_F32_SPV}"
                "${RATE_VULKAN_CUBIC_STRICT_F32_INC}"
                rate_cubic_strict_f32_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_CUBIC_STRICT_F32_SPV}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${RATE_VULKAN_POLYPHASE_NORMALIZED_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary> "${RATE_VULKAN_POLYPHASE_NORMALIZED_SPV}" "${RATE_VULKAN_POLYPHASE_NORMALIZED_INC}" rate_polyphase_normalized_f32_spv
        DEPENDS sox_embed_binary "${RATE_VULKAN_POLYPHASE_NORMALIZED_SPV}"
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
        OUTPUT "${RATE_VULKAN_SELECT_STRICT_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_SELECT_STRICT_F32_SPV}"
                "${RATE_VULKAN_SELECT_STRICT_F32_INC}"
                rate_select_strict_f32_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_SELECT_STRICT_F32_SPV}"
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
        OUTPUT "${RATE_VULKAN_PREPARE_STRICT_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_PREPARE_STRICT_F32_SPV}"
                "${RATE_VULKAN_PREPARE_STRICT_F32_INC}"
                rate_prepare_strict_f32_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_PREPARE_STRICT_F32_SPV}"
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
        OUTPUT "${RATE_VULKAN_STREAM_APPEND_STRICT_F32_INC}"
        COMMAND $<TARGET_FILE:sox_embed_binary>
                "${RATE_VULKAN_STREAM_APPEND_STRICT_F32_SPV}"
                "${RATE_VULKAN_STREAM_APPEND_STRICT_F32_INC}"
                rate_stream_append_strict_f32_spv
        DEPENDS sox_embed_binary
                "${RATE_VULKAN_STREAM_APPEND_STRICT_F32_SPV}"
        VERBATIM
    )
    add_custom_target(
        sdm_vulkan_shaders
        DEPENDS
            "${SDM_VULKAN_FIR_INC}"
            "${SDM_VULKAN_MASH_INC}"
            "${SDM_VULKAN_RESIDENT_INC}"
            "${SDM_VULKAN_RESIDENT_STRICT_F32_INC}"
            "${SDM_VULKAN_SPECIALIZED_STREAM_INC}"
            "${FIR_VULKAN_PARTITION_F64_INC}"
            "${FIR_VULKAN_PARTITION_F32_INC}"
            "${FIR_VULKAN_PARTITION_ACCURATE_F32_INC}"
            "${FIR_VULKAN_PARTITION_STRICT_F32_INC}"
            "${FIR_VULKAN_PARTITION_SPLIT_F32_INC}"
            "${FIR_VULKAN_PARTITION_PRECISE_F64_INC}"
            "${FIR_VULKAN_DIRECT_REFERENCE_F64_INC}"
            "${FIR_VULKAN_DIRECT_STRICT_F64_INC}"
            "${FIR_VULKAN_DIRECT_ACCURATE_F32_INC}"
            "${RATE_VULKAN_CUBIC_INC}"
            "${RATE_VULKAN_CUBIC_F32_INC}"
            "${RATE_VULKAN_CUBIC_STRICT_F32_INC}"
            "${RATE_VULKAN_POLYPHASE_INC}"
            "${RATE_VULKAN_POLYPHASE_F32_INC}"
            "${RATE_VULKAN_POLYPHASE_ACCURATE_F32_INC}"
            "${RATE_VULKAN_POLYPHASE_STRICT_F32_INC}"
            "${RATE_VULKAN_POLYPHASE_NORMALIZED_INC}"
            "${RATE_VULKAN_SELECT_INC}"
            "${RATE_VULKAN_SELECT_F32_INC}"
            "${RATE_VULKAN_SELECT_STRICT_F32_INC}"
            "${RATE_VULKAN_PREPARE_INC}"
            "${RATE_VULKAN_PREPARE_F32_INC}"
            "${RATE_VULKAN_PREPARE_STRICT_F32_INC}"
            "${RATE_VULKAN_STREAM_APPEND_INC}"
            "${RATE_VULKAN_STREAM_APPEND_F32_INC}"
            "${RATE_VULKAN_STREAM_APPEND_STRICT_F32_INC}"
    )
    list(APPEND LIBSOX_EFFECTS_SOURCES
        fir_vulkan.c
        rate_cubic_vulkan.c
        rate_vulkan.c
        rate_polyphase_vulkan.c
        sdm_vulkan.c
        vulkan_fft.c
        vulkan_engine.c
    )
    if(IS_WINDOWS)
        list(APPEND LIBSOX_EFFECTS_SOURCES glslang_dynamic.c)
    endif()
    list(APPEND LIBSOX_EFFECTS_HEADERS
        fir_vulkan.h
        rate_cubic_vulkan.h
        rate_vulkan.h
        rate_polyphase_vulkan.h
        sdm_vulkan.h
        vulkan_effect_chain.h
        vulkan_fft.h
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
    target_compile_definitions(libsox PRIVATE VKFFT_BACKEND=0)
    target_link_libraries(libsox PRIVATE Vulkan::Vulkan)
    if(IS_MACOS)
        target_link_libraries(libsox PRIVATE "${GLSLANG_LIBRARY}")
    endif()
    target_include_directories(libsox PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}")
    target_include_directories(libsox SYSTEM PRIVATE
        "${VKFFT_INCLUDE_DIR}")
    if(IS_WINDOWS)
        target_include_directories(libsox SYSTEM PRIVATE
            "$ENV{VULKAN_SDK}/Include/glslang/Include")
    else()
        target_include_directories(libsox SYSTEM PRIVATE
            "${GLSLANG_INCLUDE_DIR}"
            "${GLSLANG_INCLUDE_DIR}/glslang/Include")
    endif()
endif()

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

# FFmpeg libavcodec formats
if(HAVE_FFMPEG_CODECS)
    target_sources(libsox PRIVATE
        ffmpeg-codec.c
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
    if(IS_WINDOWS)
        target_link_libraries(libsox PRIVATE bcrypt)
    endif()
endif()

# FFmpeg libavformat containers
if(HAVE_FFMPEG_FORMATS)
    target_sources(libsox PRIVATE
        ffmpeg-container.c
        alac.c
    )
    target_link_libraries(libsox PRIVATE ${FFMPEG_FORMATS_LIBRARIES})
    target_include_directories(libsox PRIVATE ${FFMPEG_FORMATS_INCLUDE_DIRS})
    target_compile_definitions(libsox PRIVATE STATIC_FFMPEG_FORMATS)
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

option(
    SOX_BUILD_VULKAN_QUALITY_PROBES
    "Build test-only Vulkan numerical quality probes"
    OFF
)

if(SOX_BUILD_VULKAN_QUALITY_PROBES AND HAVE_VULKAN)
    add_executable(
        vulkan_prequant_probe
        ../tools/effects/vulkan_prequant_probe.c
    )
    target_include_directories(
        vulkan_prequant_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(vulkan_prequant_probe PRIVATE libsox)
endif()

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
    install(FILES "${GLSLANG_RUNTIME_DLL}"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

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
