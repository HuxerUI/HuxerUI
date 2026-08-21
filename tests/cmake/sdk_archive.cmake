foreach (required_variable IN ITEMS
        BUILD_DIRECTORY BUILD_CONFIG WORK_DIRECTORY CPACK_COMMAND PACKAGE_FILE_NAME PACKAGE_GENERATOR
        PACKAGE_EXTENSION INSTALL_BINDIR INSTALL_LIBDIR CLI_SUFFIX HOST_SYSTEM HOST_ARCHITECTURE
)
    if (NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif ()
endforeach ()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef TEST_NONCE)
set(TEST_ROOT "${WORK_DIRECTORY}/hux-sdk-${TEST_NONCE}")
set(PACKAGE_DIRECTORY "${TEST_ROOT}/packages")
set(EXTRACT_DIRECTORY "${TEST_ROOT}/extract")
file(MAKE_DIRECTORY "${PACKAGE_DIRECTORY}" "${EXTRACT_DIRECTORY}")

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

set(ARCHIVE_NAME "${PACKAGE_FILE_NAME}.${PACKAGE_EXTENSION}")
set(ARCHIVE_PATH "${PACKAGE_DIRECTORY}/${ARCHIVE_NAME}")
set(CHECKSUM_PATH "${ARCHIVE_PATH}.sha256")
if (NOT EXISTS "${ARCHIVE_PATH}")
    message(FATAL_ERROR "SDK archive is missing: ${ARCHIVE_PATH}")
endif ()
if (NOT EXISTS "${CHECKSUM_PATH}")
    message(FATAL_ERROR "SDK archive checksum is missing: ${CHECKSUM_PATH}")
endif ()

file(SHA256 "${ARCHIVE_PATH}" ARCHIVE_CHECKSUM)
file(READ "${CHECKSUM_PATH}" CHECKSUM_CONTENT)
string(FIND "${CHECKSUM_CONTENT}" "${ARCHIVE_CHECKSUM}" CHECKSUM_POSITION)
string(FIND "${CHECKSUM_CONTENT}" "${ARCHIVE_NAME}" CHECKSUM_NAME_POSITION)
if (CHECKSUM_POSITION LESS 0 OR CHECKSUM_NAME_POSITION LESS 0)
    message(FATAL_ERROR "SDK archive checksum does not match ${ARCHIVE_NAME}")
endif ()

file(ARCHIVE_EXTRACT INPUT "${ARCHIVE_PATH}" DESTINATION "${EXTRACT_DIRECTORY}")
set(SDK_ROOT "${EXTRACT_DIRECTORY}/${PACKAGE_FILE_NAME}")
file(REAL_PATH "${SDK_ROOT}" SDK_ROOT)
foreach (required_path IN ITEMS
        "LICENSE"
        "${INSTALL_BINDIR}/huxerui${CLI_SUFFIX}"
        "include/huxerui/huxerui.h"
        "${INSTALL_LIBDIR}/cmake/HuxerUI/HuxerUIConfig.cmake"
        "share/huxerui/resources/huxerui/resources.bin"
        "share/huxerui/tools/${HOST_SYSTEM}/${HOST_ARCHITECTURE}/hcg${CLI_SUFFIX}"
        "share/huxerui/tools/${HOST_SYSTEM}/${HOST_ARCHITECTURE}/hrc${CLI_SUFFIX}"
)
    if (NOT EXISTS "${SDK_ROOT}/${required_path}")
        message(FATAL_ERROR "SDK archive is missing ${required_path}")
    endif ()
endforeach ()

file(GLOB HOST_DIRECTORIES
        LIST_DIRECTORIES TRUE
        RELATIVE "${SDK_ROOT}/share/huxerui/tools"
        "${SDK_ROOT}/share/huxerui/tools/*"
)
if (NOT HOST_DIRECTORIES STREQUAL HOST_SYSTEM)
    message(FATAL_ERROR "SDK archive contains unexpected host tool directories: ${HOST_DIRECTORIES}")
endif ()
file(GLOB HOST_ARCHITECTURE_DIRECTORIES
        LIST_DIRECTORIES TRUE
        RELATIVE "${SDK_ROOT}/share/huxerui/tools/${HOST_SYSTEM}"
        "${SDK_ROOT}/share/huxerui/tools/${HOST_SYSTEM}/*"
)
if (NOT HOST_ARCHITECTURE_DIRECTORIES STREQUAL HOST_ARCHITECTURE)
    message(FATAL_ERROR
            "SDK archive contains unexpected host tool architectures: ${HOST_ARCHITECTURE_DIRECTORIES}"
    )
endif ()

execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=HUXERUI_HOME
                "${SDK_ROOT}/${INSTALL_BINDIR}/huxerui${CLI_SUFFIX}" doctor
        WORKING_DIRECTORY "${TEST_ROOT}"
        RESULT_VARIABLE DOCTOR_RESULT
        OUTPUT_VARIABLE DOCTOR_OUTPUT
        ERROR_VARIABLE DOCTOR_ERROR
)
string(FIND "${DOCTOR_OUTPUT}" "[ok] HUXERUI_HOME (executable): ${SDK_ROOT}" SDK_OUTPUT_POSITION)
if (NOT DOCTOR_RESULT EQUAL 0 OR SDK_OUTPUT_POSITION LESS 0 OR DOCTOR_ERROR)
    message(FATAL_ERROR "Archived SDK self-discovery failed:\n${DOCTOR_OUTPUT}${DOCTOR_ERROR}")
endif ()

file(REMOVE_RECURSE "${TEST_ROOT}")
