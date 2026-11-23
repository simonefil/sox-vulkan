# Writes OUTPUT with the part of SOURCE (waveaudio.c) that winmm-driver.c
# compiles, as the awk in run does: from the private state through stop(),
# up to check_format.  Run with cmake -P, so the build needs no awk.
file(READ "${SOURCE}" _text)
string(FIND "${_text}" "\ntypedef struct waveaudio_priv_t" _begin)
string(FIND "${_text}" "\nstatic unsigned check_format" _end)
if(_begin EQUAL -1 OR _end LESS _begin)
    message(FATAL_ERROR "waveaudio teardown not found in ${SOURCE}")
endif()
math(EXPR _begin "${_begin} + 1")
math(EXPR _length "${_end} + 1 - ${_begin}")
string(SUBSTRING "${_text}" ${_begin} ${_length} _section)
string(FIND "${_section}" "\nstatic int stop(" _stop)
if(_stop EQUAL -1)
    message(FATAL_ERROR "waveaudio teardown not found in ${SOURCE}")
endif()
file(WRITE "${OUTPUT}" "${_section}")
