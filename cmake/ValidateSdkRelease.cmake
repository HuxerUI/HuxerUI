foreach (required_variable IN ITEMS SOURCE_DIRECTORY RELEASE_TAG)
    if (NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif ()
endforeach ()

if (NOT RELEASE_TAG MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)$")
    message(FATAL_ERROR "HuxerUI release tag must use v<major>.<minor>.<patch>: ${RELEASE_TAG}")
endif ()
set(RELEASE_VERSION "${CMAKE_MATCH_1}")

file(READ "${SOURCE_DIRECTORY}/CMakeLists.txt" PROJECT_FILE)
string(REGEX MATCH
        "project\\([ \t\r\n]*huxerui[ \t\r\n]+VERSION[ \t\r\n]+([0-9]+\\.[0-9]+\\.[0-9]+)"
        PROJECT_DECLARATION
        "${PROJECT_FILE}"
)
if (NOT PROJECT_DECLARATION)
    message(FATAL_ERROR "HuxerUI project version was not found")
endif ()
set(PROJECT_VERSION "${CMAKE_MATCH_1}")
if (NOT RELEASE_VERSION STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR
            "HuxerUI release tag ${RELEASE_TAG} does not match project version ${PROJECT_VERSION}"
    )
endif ()

if (DEFINED OUTPUT_FILE AND NOT OUTPUT_FILE STREQUAL "")
    file(APPEND "${OUTPUT_FILE}" "version=${RELEASE_VERSION}\n")
endif ()

if (DEFINED ASSET_DIRECTORY AND NOT ASSET_DIRECTORY STREQUAL "")
    set(EXPECTED_ARCHIVES
            "huxerui-sdk-${RELEASE_VERSION}-macos-arm64.tar.gz"
            "huxerui-sdk-${RELEASE_VERSION}-macos-x86_64.tar.gz"
            "huxerui-sdk-${RELEASE_VERSION}-windows-x86_64.zip"
            "huxerui-sdk-${RELEASE_VERSION}-linux-x86_64.tar.gz"
    )
    set(EXPECTED_ASSETS install.ps1 install.sh)
    foreach (archive_name IN LISTS EXPECTED_ARCHIVES)
        list(APPEND EXPECTED_ASSETS "${archive_name}" "${archive_name}.sha256")
    endforeach ()
    list(SORT EXPECTED_ASSETS)

    file(GLOB ACTUAL_ASSETS
            LIST_DIRECTORIES TRUE
            RELATIVE "${ASSET_DIRECTORY}"
            "${ASSET_DIRECTORY}/*"
    )
    list(SORT ACTUAL_ASSETS)
    if (NOT ACTUAL_ASSETS STREQUAL EXPECTED_ASSETS)
        message(FATAL_ERROR
                "HuxerUI release assets do not match the expected set\n"
                "Expected: ${EXPECTED_ASSETS}\n"
                "Actual: ${ACTUAL_ASSETS}"
        )
    endif ()

    foreach (archive_name IN LISTS EXPECTED_ARCHIVES)
        file(SHA256 "${ASSET_DIRECTORY}/${archive_name}" ARCHIVE_CHECKSUM)
        file(READ "${ASSET_DIRECTORY}/${archive_name}.sha256" CHECKSUM_CONTENT)
        string(STRIP "${CHECKSUM_CONTENT}" CHECKSUM_CONTENT)
        if (NOT CHECKSUM_CONTENT STREQUAL "${ARCHIVE_CHECKSUM}  ${archive_name}")
            message(FATAL_ERROR "HuxerUI release checksum does not match ${archive_name}")
        endif ()
    endforeach ()
endif ()
