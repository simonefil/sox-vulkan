# Regression suite from sox_ng (https://codeberg.org/sox_ng/sox_ng, test/).
#
# Each directory holding a "run" script becomes one CTest test, driven by
# check.sh against the sox built here.  check.sh stays usable by hand:
#
#     sox=build/src/sox sh test/check.sh
#
# The scripts need a POSIX shell, so without one the suite is not registered.

find_program(SOX_TEST_SH NAMES sh)
if(NOT SOX_TEST_SH)
    message(STATUS "Regression suite: no POSIX shell found, tests not registered")
    return()
endif()

# Helper used by bit-depth to write raw sample values
add_executable(sox_test_samples samples.c)
set_target_properties(sox_test_samples PROPERTIES OUTPUT_NAME samples)

# On Windows waveaudio-teardown takes its WinMM stub from here, since its run
# script needs awk and cc to build it.  teardown.inc is extracted from
# waveaudio.c at build time.
if(WIN32)
    set(_teardown_dir ${CMAKE_CURRENT_BINARY_DIR}/winmm-driver)
    add_custom_command(OUTPUT ${_teardown_dir}/teardown.inc
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE=${PROJECT_SOURCE_DIR}/src/waveaudio.c
            -DOUTPUT=${_teardown_dir}/teardown.inc
            -P ${CMAKE_CURRENT_SOURCE_DIR}/waveaudio-teardown/teardown-inc.cmake
        DEPENDS ${PROJECT_SOURCE_DIR}/src/waveaudio.c
            ${CMAKE_CURRENT_SOURCE_DIR}/waveaudio-teardown/teardown-inc.cmake
        VERBATIM)
    add_executable(sox_test_winmm_driver
        waveaudio-teardown/winmm-driver.c ${_teardown_dir}/teardown.inc)
    target_include_directories(sox_test_winmm_driver PRIVATE ${_teardown_dir})
    set_target_properties(sox_test_winmm_driver PROPERTIES OUTPUT_NAME winmm-driver)
    set(_sox_test_winmm_driver winmm_driver=$<TARGET_FILE:sox_test_winmm_driver>)
endif()

file(GLOB _sox_test_entries RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/*)
list(SORT _sox_test_entries)

foreach(_test IN LISTS _sox_test_entries)
    if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${_test}
            AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_test}/run)
        add_test(NAME ${_test}
            COMMAND ${CMAKE_COMMAND} -E env
                sox=$<TARGET_FILE:sox_exe>
                samples=$<TARGET_FILE:sox_test_samples>
                ${_sox_test_winmm_driver}
                ${SOX_TEST_SH} ${CMAKE_CURRENT_SOURCE_DIR}/check.sh -n ${_test}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        # check.sh exits 0 for a test that cannot run with this build (VOID)
        if(NOT CMAKE_VERSION VERSION_LESS 3.16)
            set_tests_properties(${_test} PROPERTIES
                SKIP_REGULAR_EXPRESSION "^${_test}: VOID")
        endif()
        set_tests_properties(${_test} PROPERTIES TIMEOUT 300)
    endif()
endforeach()
