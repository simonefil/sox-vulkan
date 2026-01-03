# libgsm - GSM codec library (in-tree build)

set(LIBGSM_SOURCES
    add.c
    code.c
    decode.c
    long_term.c
    lpc.c
    preprocess.c
    rpe.c
    gsm_destroy.c
    gsm_decode.c
    gsm_encode.c
    gsm_create.c
    gsm_option.c
    short_term.c
    table.c
)

set(LIBGSM_HEADERS
    private.h
    gsm.h
    aliases.h
)

add_library(gsm_internal STATIC ${LIBGSM_SOURCES} ${LIBGSM_HEADERS})

target_compile_definitions(gsm_internal PRIVATE
    SASR                # Signed arithmetic shift right
    USE_FLOAT_MUL       # Use float multiplication
    FAST                # Fast approximation mode
    WAV49               # WAV #49 format packing support
)

target_include_directories(gsm_internal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# Alias for use in parent project
add_library(SoX::gsm ALIAS gsm_internal)

# Install for export
install(TARGETS gsm_internal
    EXPORT SoXTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
