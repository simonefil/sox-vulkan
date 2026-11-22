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
