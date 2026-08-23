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
file(TO_NATIVE_PATH "${SDK_ROOT}" SDK_ROOT_NATIVE)
foreach (required_path IN ITEMS
        "LICENSE"
        "${INSTALL_BINDIR}/huxerui${CLI_SUFFIX}"
        "include/huxerui/huxerui.h"
        "${INSTALL_LIBDIR}/cmake/HuxerUI/HuxerUIConfig.cmake"
        "${INSTALL_LIBDIR}/cmake/HuxerUI/HuxerUISharedTargets.cmake"
        "${INSTALL_LIBDIR}/cmake/HuxerUI/HuxerUIStaticTargets.cmake"
        "${INSTALL_LIBDIR}/cmake/HuxerUI/HuxerUIApp.cmake"
        "${INSTALL_LIBDIR}/cmake/HuxerUI/HuxerUICodegen.cmake"
        "${INSTALL_LIBDIR}/cmake/HuxerUI/HuxerUILibraries.cmake"
        "${INSTALL_LIBDIR}/cmake/HuxerUI/HuxerUIResourceBuild.cmake"
        "${INSTALL_LIBDIR}/cmake/HuxerUI/HuxerUIResources.cmake"
        "share/huxerui/resources/huxerui/resources.bin"
        "share/huxerui/tools/${HOST_SYSTEM}/${HOST_ARCHITECTURE}/hcg${CLI_SUFFIX}"
        "share/huxerui/tools/${HOST_SYSTEM}/${HOST_ARCHITECTURE}/hrc${CLI_SUFFIX}"
)
    if (NOT EXISTS "${SDK_ROOT}/${required_path}")
        message(FATAL_ERROR "SDK archive is missing ${required_path}")
    endif ()
endforeach ()

if (HOST_SYSTEM STREQUAL "windows" AND WINDOWS_DEBUG_INCLUDED)
    foreach (required_path IN ITEMS
            "bin/huxerui.dll"
            "bin/huxerui_debug.dll"
            "${INSTALL_LIBDIR}/huxerui.lib"
            "${INSTALL_LIBDIR}/huxerui_debug.lib"
            "${INSTALL_LIBDIR}/huxerui_static.lib"
            "${INSTALL_LIBDIR}/huxerui_static_debug.lib"
            "${INSTALL_LIBDIR}/cmake/HuxerUI/HuxerUISharedTargets-release.cmake"
            "${INSTALL_LIBDIR}/cmake/HuxerUI/HuxerUISharedTargets-debug.cmake"
            "${INSTALL_LIBDIR}/cmake/HuxerUI/HuxerUIStaticTargets-release.cmake"
            "${INSTALL_LIBDIR}/cmake/HuxerUI/HuxerUIStaticTargets-debug.cmake"
    )
        if (NOT EXISTS "${SDK_ROOT}/${required_path}")
            message(FATAL_ERROR "Windows SDK archive is missing ${required_path}")
        endif ()
    endforeach ()
endif ()

foreach (build_only_path IN ITEMS
        "${INSTALL_LIBDIR}/cmake/HuxerUI/HuxerUIBuild.cmake"
        "${INSTALL_LIBDIR}/cmake/HuxerUI/platform"
)
    if (EXISTS "${SDK_ROOT}/${build_only_path}")
        message(FATAL_ERROR "SDK archive contains source-build CMake logic: ${build_only_path}")
    endif ()
endforeach ()

if (PLATFORM_ARTIFACTS_INCLUDED)
    foreach (required_path IN ITEMS
            "share/huxerui/platform/android/HuxerUI.aar"
            "share/huxerui/platform/android/arm64-v8a/libhuxerui.so"
            "share/huxerui/platform/android/x86_64/libhuxerui.so"
            "share/huxerui/platform/web/emscripten-4.0.19/libhuxerui.a"
    )
        if (NOT EXISTS "${SDK_ROOT}/${required_path}")
            message(FATAL_ERROR "SDK archive is missing ${required_path}")
        endif ()
    endforeach ()

    foreach (unexpected_path IN ITEMS
            "share/huxerui/platform/android/arm64-v8a/libhuxerui_static.a"
            "share/huxerui/platform/android/x86_64/libhuxerui_static.a"
    )
        if (EXISTS "${SDK_ROOT}/${unexpected_path}")
            message(FATAL_ERROR "SDK archive contains unsupported Android static library: ${unexpected_path}")
        endif ()
    endforeach ()

    foreach (platform IN ITEMS android web)
        set(CONSUMER_SOURCE "${TEST_ROOT}/${platform}-consumer")
        set(CONSUMER_BUILD "${TEST_ROOT}/${platform}-consumer-build")
        file(MAKE_DIRECTORY "${CONSUMER_SOURCE}")
        if (platform STREQUAL "android")
            set(PLATFORM_CONFIGURATION "set(ANDROID TRUE)\nset(ANDROID_ABI arm64-v8a)")
            set(PLATFORM_ASSERTION
                    "if (TARGET HuxerUI::huxerui_static)\n  message(FATAL_ERROR \"Android SDK unexpectedly exposes HuxerUI::huxerui_static\")\nendif ()"
            )
            set(EXPECTED_LIBRARY
                    "${SDK_ROOT}/share/huxerui/platform/android/arm64-v8a/libhuxerui.so"
            )
        else ()
            set(PLATFORM_CONFIGURATION "set(EMSCRIPTEN TRUE)\nset(CMAKE_SIZEOF_VOID_P 4)")
            set(PLATFORM_ASSERTION)
            set(EXPECTED_LIBRARY
                    "${SDK_ROOT}/share/huxerui/platform/web/emscripten-4.0.19/libhuxerui.a"
            )
        endif ()
        file(WRITE "${CONSUMER_SOURCE}/CMakeLists.txt"
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(HuxerUITargetSdkConsumer LANGUAGES NONE)\n"
                "${PLATFORM_CONFIGURATION}\n"
                "find_package(HuxerUI CONFIG REQUIRED PATHS \"${SDK_ROOT}\" NO_DEFAULT_PATH)\n"
                "${PLATFORM_ASSERTION}\n"
                "get_target_property(location HuxerUI::huxerui IMPORTED_LOCATION)\n"
                "if (NOT location STREQUAL \"${EXPECTED_LIBRARY}\")\n"
                "  message(FATAL_ERROR \"Unexpected imported library: \${location}\")\n"
                "endif ()\n"
        )
        execute_process(
                COMMAND "${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE}" -B "${CONSUMER_BUILD}"
                RESULT_VARIABLE CONSUMER_RESULT
                OUTPUT_VARIABLE CONSUMER_OUTPUT
                ERROR_VARIABLE CONSUMER_ERROR
        )
        if (NOT CONSUMER_RESULT EQUAL 0)
            message(FATAL_ERROR
                    "Installed ${platform} SDK import failed:\n${CONSUMER_OUTPUT}${CONSUMER_ERROR}"
            )
        endif ()
    endforeach ()
endif ()

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
string(FIND "${DOCTOR_OUTPUT}" "[ok] HUXERUI_HOME (executable): ${SDK_ROOT_NATIVE}" SDK_OUTPUT_POSITION)
if (NOT DOCTOR_RESULT EQUAL 0 OR SDK_OUTPUT_POSITION LESS 0 OR DOCTOR_ERROR)
    message(FATAL_ERROR "Archived SDK self-discovery failed:\n${DOCTOR_OUTPUT}${DOCTOR_ERROR}")
endif ()

file(REMOVE_RECURSE "${TEST_ROOT}")
