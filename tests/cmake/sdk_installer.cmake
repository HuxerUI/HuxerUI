foreach (required_variable IN ITEMS
        BUILD_DIRECTORY BUILD_CONFIG WORK_DIRECTORY CPACK_COMMAND PACKAGE_FILE_NAME PACKAGE_GENERATOR
        PACKAGE_EXTENSION INSTALLER_PATH HOST_SYSTEM HOST_ARCHITECTURE
)
    if (NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif ()
endforeach ()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef TEST_NONCE)
set(TEST_ROOT "${WORK_DIRECTORY}/hux-installer-${TEST_NONCE}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(REAL_PATH "${TEST_ROOT}" TEST_ROOT)
set(PACKAGE_DIRECTORY "${TEST_ROOT}/packages")
set(INSTALL_PREFIX "${TEST_ROOT}/HuxerUI SDK/it's custom")
set(INVALID_PREFIX "${TEST_ROOT}/existing directory")
set(PROFILE_PATH "${TEST_ROOT}/profile")
file(MAKE_DIRECTORY "${PACKAGE_DIRECTORY}")
file(WRITE "${PROFILE_PATH}" "export HUXERUI_TEST_VALUE=preserved\n")

set(PACKAGE_COMMAND
        "${CPACK_COMMAND}"
        --config "${BUILD_DIRECTORY}/CPackConfig.cmake"
        -G "${PACKAGE_GENERATOR}"
        -B "${PACKAGE_DIRECTORY}"
)
if (BUILD_CONFIG)
    list(APPEND PACKAGE_COMMAND -C "${BUILD_CONFIG}")
endif ()
execute_process(
        COMMAND ${PACKAGE_COMMAND}
        RESULT_VARIABLE PACKAGE_RESULT
        OUTPUT_VARIABLE PACKAGE_OUTPUT
        ERROR_VARIABLE PACKAGE_ERROR
)
if (NOT PACKAGE_RESULT EQUAL 0)
    message(FATAL_ERROR "SDK archive generation failed:\n${PACKAGE_OUTPUT}${PACKAGE_ERROR}")
endif ()

set(ARCHIVE_PATH "${PACKAGE_DIRECTORY}/${PACKAGE_FILE_NAME}.${PACKAGE_EXTENSION}")
if (NOT EXISTS "${ARCHIVE_PATH}" OR NOT EXISTS "${ARCHIVE_PATH}.sha256")
    message(FATAL_ERROR "SDK installer test archive or checksum is missing")
endif ()

execute_process(
        COMMAND "${INSTALLER_PATH}"
                --archive "${ARCHIVE_PATH}"
                --prefix "${INSTALL_PREFIX}"
                --profile "${PROFILE_PATH}"
                --yes
        RESULT_VARIABLE INSTALL_RESULT
        OUTPUT_VARIABLE INSTALL_OUTPUT
        ERROR_VARIABLE INSTALL_ERROR
)
if (NOT INSTALL_RESULT EQUAL 0)
    message(FATAL_ERROR "SDK installation failed:\n${INSTALL_OUTPUT}${INSTALL_ERROR}")
endif ()
if (NOT EXISTS "${INSTALL_PREFIX}/bin/huxerui")
    message(FATAL_ERROR "SDK installer did not publish the CLI")
endif ()

file(READ "${PROFILE_PATH}" PROFILE_CONTENT)
if (NOT PROFILE_CONTENT MATCHES "export HUXERUI_TEST_VALUE=preserved")
    message(FATAL_ERROR "SDK installer replaced unrelated profile content")
endif ()
string(REGEX MATCHALL "# huxerui-sdk environment begin" PROFILE_MARKERS "${PROFILE_CONTENT}")
list(LENGTH PROFILE_MARKERS PROFILE_MARKER_COUNT)
if (NOT PROFILE_MARKER_COUNT EQUAL 1)
    message(FATAL_ERROR "SDK installer wrote an unexpected number of profile blocks")
endif ()

set(VERIFY_SCRIPT "${TEST_ROOT}/verify.sh")
file(WRITE "${VERIFY_SCRIPT}" [=[#!/bin/sh
set -eu
. "$1"
test "$HUXERUI_HOME" = "$2"
test "$HUXERUI_TEST_VALUE" = preserved
huxerui --version
]=])
execute_process(
        COMMAND /bin/sh "${VERIFY_SCRIPT}" "${PROFILE_PATH}" "${INSTALL_PREFIX}"
        RESULT_VARIABLE VERIFY_RESULT
        OUTPUT_VARIABLE VERIFY_OUTPUT
        ERROR_VARIABLE VERIFY_ERROR
)
if (NOT VERIFY_RESULT EQUAL 0 OR NOT VERIFY_OUTPUT MATCHES "huxerui [0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR "Installed SDK environment is invalid:\n${VERIFY_OUTPUT}${VERIFY_ERROR}")
endif ()

execute_process(
        COMMAND "${INSTALLER_PATH}"
                --archive "${ARCHIVE_PATH}"
                --prefix "${INSTALL_PREFIX}"
                --profile "${PROFILE_PATH}"
                --yes
        RESULT_VARIABLE REINSTALL_RESULT
        OUTPUT_VARIABLE REINSTALL_OUTPUT
        ERROR_VARIABLE REINSTALL_ERROR
)
if (NOT REINSTALL_RESULT EQUAL 0)
    message(FATAL_ERROR "SDK repeat installation failed:\n${REINSTALL_OUTPUT}${REINSTALL_ERROR}")
endif ()
file(READ "${PROFILE_PATH}" PROFILE_CONTENT)
string(REGEX MATCHALL "# huxerui-sdk environment begin" PROFILE_MARKERS "${PROFILE_CONTENT}")
list(LENGTH PROFILE_MARKERS PROFILE_MARKER_COUNT)
if (NOT PROFILE_MARKER_COUNT EQUAL 1)
    message(FATAL_ERROR "SDK repeat installation duplicated its profile block")
endif ()

file(WRITE "${INSTALL_PREFIX}/upgrade-marker.txt" "preserved\n")
set(VALID_PROFILE_CONTENT "${PROFILE_CONTENT}")
file(WRITE "${PROFILE_PATH}"
        "export HUXERUI_TEST_VALUE=preserved\n# huxerui-sdk environment begin\n"
)
execute_process(
        COMMAND "${INSTALLER_PATH}"
                --archive "${ARCHIVE_PATH}"
                --prefix "${INSTALL_PREFIX}"
                --profile "${PROFILE_PATH}"
                --yes
        RESULT_VARIABLE ROLLBACK_RESULT
        OUTPUT_VARIABLE ROLLBACK_OUTPUT
        ERROR_VARIABLE ROLLBACK_ERROR
)
if (ROLLBACK_RESULT EQUAL 0
        OR NOT ROLLBACK_ERROR MATCHES "profile contains an incomplete HuxerUI environment block")
    message(FATAL_ERROR "SDK installer did not reject an invalid profile:\n${ROLLBACK_OUTPUT}${ROLLBACK_ERROR}")
endif ()
if (NOT EXISTS "${INSTALL_PREFIX}/upgrade-marker.txt")
    message(FATAL_ERROR "SDK installer did not restore the previous SDK after an upgrade failure")
endif ()
file(WRITE "${PROFILE_PATH}" "${VALID_PROFILE_CONTENT}")

set(BAD_ARCHIVE_NAME "huxerui-sdk-invalid-${HOST_SYSTEM}-${HOST_ARCHITECTURE}.${PACKAGE_EXTENSION}")
set(BAD_ARCHIVE "${PACKAGE_DIRECTORY}/${BAD_ARCHIVE_NAME}")
set(BAD_CHECKSUM_PARENT "${TEST_ROOT}/checksum failure parent")
configure_file("${ARCHIVE_PATH}" "${BAD_ARCHIVE}" COPYONLY)
file(WRITE "${BAD_ARCHIVE}.sha256"
        "0000000000000000000000000000000000000000000000000000000000000000  ${BAD_ARCHIVE_NAME}\n"
)
execute_process(
        COMMAND "${INSTALLER_PATH}"
                --archive "${BAD_ARCHIVE}"
                --prefix "${BAD_CHECKSUM_PARENT}/HuxerUI"
                --profile "${PROFILE_PATH}"
                --yes
        RESULT_VARIABLE BAD_CHECKSUM_RESULT
        OUTPUT_VARIABLE BAD_CHECKSUM_OUTPUT
        ERROR_VARIABLE BAD_CHECKSUM_ERROR
)
if (BAD_CHECKSUM_RESULT EQUAL 0
        OR NOT BAD_CHECKSUM_ERROR MATCHES "archive checksum does not match")
    message(FATAL_ERROR
            "SDK installer accepted an invalid checksum:\n${BAD_CHECKSUM_OUTPUT}${BAD_CHECKSUM_ERROR}"
    )
endif ()
if (EXISTS "${BAD_CHECKSUM_PARENT}")
    message(FATAL_ERROR "SDK installer created an installation directory for an invalid checksum")
endif ()

file(MAKE_DIRECTORY "${INVALID_PREFIX}")
file(WRITE "${INVALID_PREFIX}/keep.txt" "keep\n")
execute_process(
        COMMAND "${INSTALLER_PATH}"
                --archive "${ARCHIVE_PATH}"
                --prefix "${INVALID_PREFIX}"
                --profile "${PROFILE_PATH}"
                --yes
        RESULT_VARIABLE INVALID_PREFIX_RESULT
        OUTPUT_VARIABLE INVALID_PREFIX_OUTPUT
        ERROR_VARIABLE INVALID_PREFIX_ERROR
)
if (INVALID_PREFIX_RESULT EQUAL 0
        OR NOT INVALID_PREFIX_ERROR MATCHES "installation prefix exists but is not a HuxerUI SDK")
    message(FATAL_ERROR
            "SDK installer accepted an unrelated directory:\n${INVALID_PREFIX_OUTPUT}${INVALID_PREFIX_ERROR}"
    )
endif ()
if (NOT EXISTS "${INVALID_PREFIX}/keep.txt")
    message(FATAL_ERROR "SDK installer modified an unrelated directory")
endif ()

execute_process(
        COMMAND "${INSTALLER_PATH}"
                --uninstall
                --prefix "${INSTALL_PREFIX}"
                --profile "${PROFILE_PATH}"
                --yes
        RESULT_VARIABLE UNINSTALL_RESULT
        OUTPUT_VARIABLE UNINSTALL_OUTPUT
        ERROR_VARIABLE UNINSTALL_ERROR
)
if (NOT UNINSTALL_RESULT EQUAL 0)
    message(FATAL_ERROR "SDK uninstall failed:\n${UNINSTALL_OUTPUT}${UNINSTALL_ERROR}")
endif ()
if (EXISTS "${INSTALL_PREFIX}")
    message(FATAL_ERROR "SDK uninstall left the installation prefix behind")
endif ()
file(READ "${PROFILE_PATH}" PROFILE_CONTENT)
if (NOT PROFILE_CONTENT STREQUAL "export HUXERUI_TEST_VALUE=preserved\n")
    message(FATAL_ERROR "SDK uninstall did not restore unrelated profile content")
endif ()

file(REMOVE_RECURSE "${TEST_ROOT}")
