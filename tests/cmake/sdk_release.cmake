foreach (required_variable IN ITEMS SOURCE_DIRECTORY WORK_DIRECTORY VALIDATOR_PATH PROJECT_VERSION)
    if (NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif ()
endforeach ()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef TEST_NONCE)
set(TEST_ROOT "${WORK_DIRECTORY}/hux-release-${TEST_NONCE}")
set(ASSET_DIRECTORY "${TEST_ROOT}/assets")
set(OUTPUT_FILE "${TEST_ROOT}/release-output")
file(MAKE_DIRECTORY "${ASSET_DIRECTORY}")

set(ARCHIVE_NAMES
        "huxerui-sdk-${PROJECT_VERSION}-macos-arm64.tar.gz"
        "huxerui-sdk-${PROJECT_VERSION}-macos-x86_64.tar.gz"
        "huxerui-sdk-${PROJECT_VERSION}-windows-x86_64.zip"
        "huxerui-sdk-${PROJECT_VERSION}-linux-x86_64.tar.gz"
)
foreach (archive_name IN LISTS ARCHIVE_NAMES)
    file(WRITE "${ASSET_DIRECTORY}/${archive_name}" "${archive_name}\n")
    file(SHA256 "${ASSET_DIRECTORY}/${archive_name}" ARCHIVE_CHECKSUM)
    file(WRITE "${ASSET_DIRECTORY}/${archive_name}.sha256" "${ARCHIVE_CHECKSUM}  ${archive_name}\n")
endforeach ()
configure_file("${SOURCE_DIRECTORY}/install.sh" "${ASSET_DIRECTORY}/install.sh" COPYONLY)
configure_file("${SOURCE_DIRECTORY}/install.ps1" "${ASSET_DIRECTORY}/install.ps1" COPYONLY)

execute_process(
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIRECTORY=${SOURCE_DIRECTORY}"
                "-DRELEASE_TAG=v${PROJECT_VERSION}"
                "-DOUTPUT_FILE=${OUTPUT_FILE}"
                "-DASSET_DIRECTORY=${ASSET_DIRECTORY}"
                -P "${VALIDATOR_PATH}"
        RESULT_VARIABLE VALID_RESULT
        OUTPUT_VARIABLE VALID_OUTPUT
        ERROR_VARIABLE VALID_ERROR
)
if (NOT VALID_RESULT EQUAL 0)
    message(FATAL_ERROR "Valid SDK release was rejected:\n${VALID_OUTPUT}${VALID_ERROR}")
endif ()
file(READ "${OUTPUT_FILE}" RELEASE_OUTPUT)
if (NOT RELEASE_OUTPUT STREQUAL "version=${PROJECT_VERSION}\n")
    message(FATAL_ERROR "SDK release output contains an unexpected version: ${RELEASE_OUTPUT}")
endif ()

execute_process(
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIRECTORY=${SOURCE_DIRECTORY}"
                "-DRELEASE_TAG=v999.999.999"
                -P "${VALIDATOR_PATH}"
        RESULT_VARIABLE VERSION_RESULT
        OUTPUT_VARIABLE VERSION_OUTPUT
        ERROR_VARIABLE VERSION_ERROR
)
if (VERSION_RESULT EQUAL 0 OR NOT VERSION_ERROR MATCHES "does not match project version")
    message(FATAL_ERROR "SDK release accepted a mismatched version:\n${VERSION_OUTPUT}${VERSION_ERROR}")
endif ()

set(UNEXPECTED_ASSET "${ASSET_DIRECTORY}/unexpected.txt")
file(WRITE "${UNEXPECTED_ASSET}" "unexpected\n")
execute_process(
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIRECTORY=${SOURCE_DIRECTORY}"
                "-DRELEASE_TAG=v${PROJECT_VERSION}"
                "-DASSET_DIRECTORY=${ASSET_DIRECTORY}"
                -P "${VALIDATOR_PATH}"
        RESULT_VARIABLE ASSET_RESULT
        OUTPUT_VARIABLE ASSET_OUTPUT
        ERROR_VARIABLE ASSET_ERROR
)
if (ASSET_RESULT EQUAL 0 OR NOT ASSET_ERROR MATCHES "do not match the expected set")
    message(FATAL_ERROR "SDK release accepted an unexpected asset:\n${ASSET_OUTPUT}${ASSET_ERROR}")
endif ()
file(REMOVE "${UNEXPECTED_ASSET}")

list(GET ARCHIVE_NAMES 0 CORRUPT_ARCHIVE)
file(APPEND "${ASSET_DIRECTORY}/${CORRUPT_ARCHIVE}" "corrupt")
execute_process(
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIRECTORY=${SOURCE_DIRECTORY}"
                "-DRELEASE_TAG=v${PROJECT_VERSION}"
                "-DASSET_DIRECTORY=${ASSET_DIRECTORY}"
                -P "${VALIDATOR_PATH}"
        RESULT_VARIABLE CHECKSUM_RESULT
        OUTPUT_VARIABLE CHECKSUM_OUTPUT
        ERROR_VARIABLE CHECKSUM_ERROR
)
if (CHECKSUM_RESULT EQUAL 0 OR NOT CHECKSUM_ERROR MATCHES "release checksum does not match")
    message(FATAL_ERROR "SDK release accepted a mismatched checksum:\n${CHECKSUM_OUTPUT}${CHECKSUM_ERROR}")
endif ()

file(REMOVE_RECURSE "${TEST_ROOT}")
