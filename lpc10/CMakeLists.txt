# liblpc10 - LPC10 codec library (in-tree build)

set(LIBLPC10_SOURCES
    analys.c
    bsynz.c
    chanwr.c
    dcbias.c
    decode.c
    deemp.c
    difmag.c
    dyptrk.c
    encode.c
    energy.c
    f2clib.c
    ham84.c
    hp100.c
    invert.c
    irc2pc.c
    ivfilt.c
    lpcdec.c
    lpcenc.c
    lpcini.c
    lpfilt.c
    median.c
    mload.c
    onset.c
    pitsyn.c
    placea.c
    placev.c
    preemp.c
    prepro.c
    random.c
    rcchk.c
    synths.c
    tbdm.c
    voicin.c
    vparms.c
)

set(LIBLPC10_HEADERS
    f2c.h
    lpc10.h
)

add_library(lpc10_internal STATIC ${LIBLPC10_SOURCES} ${LIBLPC10_HEADERS})

target_include_directories(lpc10_internal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

# Alias for use in parent project
add_library(SoX::lpc10 ALIAS lpc10_internal)

# Install for export
install(TARGETS lpc10_internal
    EXPORT SoXTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
