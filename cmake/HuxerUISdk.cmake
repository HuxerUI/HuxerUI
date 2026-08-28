include_guard(GLOBAL)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(HUXERUI_WEB_EMSCRIPTEN_VERSION "4.0.19")

if (ANDROID)
    if (NOT ANDROID_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR "HuxerUI Android host SDK supports arm64-v8a only")
    endif ()
    set(HUXERUI_SDK_HOST_PLATFORM "android")
    set(HUXERUI_SDK_HOST_ARCHITECTURE "${ANDROID_ABI}")
else ()
    _huxerui_resolve_host(HUXERUI_SDK_HOST_PLATFORM HUXERUI_SDK_HOST_ARCHITECTURE)
endif ()

if (PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR
        AND HUXERUI_BUILD_CLI
        AND (WIN32 OR (APPLE AND NOT IOS) OR (UNIX AND NOT APPLE AND NOT EMSCRIPTEN)))
    set(HUXERUI_SDK_PACKAGE_FILE_NAME
            "huxerui-sdk-${PROJECT_VERSION}-${HUXERUI_SDK_HOST_PLATFORM}-${HUXERUI_SDK_HOST_ARCHITECTURE}"
    )
    if (WIN32)
        set(HUXERUI_SDK_PACKAGE_GENERATOR "ZIP")
        set(HUXERUI_SDK_PACKAGE_EXTENSION "zip")
    else ()
        set(HUXERUI_SDK_PACKAGE_GENERATOR "TGZ")
        set(HUXERUI_SDK_PACKAGE_EXTENSION "tar.gz")
    endif ()
endif ()

if (TARGET ${HUXERUI_SHARED_LIB_NAME}
        AND NOT HUXERUI_SDK_HOST_PLATFORM STREQUAL "android")
    install(TARGETS ${HUXERUI_SHARED_LIB_NAME}
            EXPORT HuxerUISharedTargets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT HuxerUILibraries
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT HuxerUILibraries
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT HuxerUILibraries
    )
endif ()
if (TARGET ${HUXERUI_STATIC_LIB_NAME}
        AND NOT HUXERUI_SDK_HOST_PLATFORM STREQUAL "android")
    install(TARGETS ${HUXERUI_STATIC_LIB_NAME}
            EXPORT HuxerUIStaticTargets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT HuxerUILibraries
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT HuxerUILibraries
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT HuxerUILibraries
    )
endif ()

install(DIRECTORY "${HUXERUI_PUBLIC_HEADER_DIR}"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)
install(DIRECTORY "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/huxerui/resources"
)
install(DIRECTORY "${HUXERUI_PROJECT_DIR}/skills/huxerui-app-development"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/huxerui/skills"
)
install(FILES "${HUXERUI_PROJECT_DIR}/LICENSE" DESTINATION ".")

set(HUXERUI_INSTALL_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/HuxerUI")
configure_package_config_file(
        "${HUXERUI_PROJECT_DIR}/cmake/HuxerUIConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/HuxerUIConfig.cmake"
        INSTALL_DESTINATION "${HUXERUI_INSTALL_CMAKE_DIR}"
)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET ${HUXERUI_STATIC_LIB_NAME})
    configure_file(
            "${HUXERUI_PROJECT_DIR}/cmake/HuxerUILinuxStaticDependencies.cmake.in"
            "${CMAKE_CURRENT_BINARY_DIR}/HuxerUILinuxStaticDependencies.cmake"
            @ONLY
    )
endif ()
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/HuxerUIConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion
        ARCH_INDEPENDENT
)

if (TARGET ${HUXERUI_SHARED_LIB_NAME}
        AND NOT HUXERUI_SDK_HOST_PLATFORM STREQUAL "android")
    install(EXPORT HuxerUISharedTargets
            FILE HuxerUISharedTargets.cmake
            NAMESPACE HuxerUI::
            DESTINATION "${HUXERUI_INSTALL_CMAKE_DIR}"
            COMPONENT HuxerUILibraries
    )
endif ()
if (TARGET ${HUXERUI_STATIC_LIB_NAME}
        AND NOT HUXERUI_SDK_HOST_PLATFORM STREQUAL "android")
    install(EXPORT HuxerUIStaticTargets
            FILE HuxerUIStaticTargets.cmake
            NAMESPACE HuxerUI::
            DESTINATION "${HUXERUI_INSTALL_CMAKE_DIR}"
            COMPONENT HuxerUILibraries
    )
endif ()
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/HuxerUIConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/HuxerUIConfigVersion.cmake"
        "${HUXERUI_PROJECT_DIR}/cmake/HuxerUIApp.cmake"
        "${HUXERUI_PROJECT_DIR}/cmake/HuxerUICodegen.cmake"
        "${HUXERUI_PROJECT_DIR}/cmake/HuxerUILibraries.cmake"
        "${HUXERUI_PROJECT_DIR}/cmake/HuxerUIResourceBuild.cmake"
        "${HUXERUI_PROJECT_DIR}/cmake/HuxerUIResources.cmake"
        DESTINATION "${HUXERUI_INSTALL_CMAKE_DIR}"
)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET ${HUXERUI_STATIC_LIB_NAME})
    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/HuxerUILinuxStaticDependencies.cmake"
            DESTINATION "${HUXERUI_INSTALL_CMAKE_DIR}"
    )
endif ()
install(FILES
        "${HUXERUI_PROJECT_DIR}/platform/web/web_file.js"
        DESTINATION "${HUXERUI_INSTALL_CMAKE_DIR}"
        RENAME HuxerUIWebFileSystem.js
)

if (TARGET huxerui_cli)
    install(TARGETS huxerui_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif ()

if (HUXERUI_INTERNAL_SDK_ARTIFACT_ROOT)
    file(TO_CMAKE_PATH "${HUXERUI_INTERNAL_SDK_ARTIFACT_ROOT}" HUXERUI_SDK_ARTIFACT_ROOT)
    set(HUXERUI_ANDROID_ARTIFACT_ROOT "${HUXERUI_SDK_ARTIFACT_ROOT}/android")
    set(HUXERUI_WEB_ARTIFACT_ROOT
            "${HUXERUI_SDK_ARTIFACT_ROOT}/web/emscripten-${HUXERUI_WEB_EMSCRIPTEN_VERSION}"
    )
    foreach (HUXERUI_REQUIRED_PLATFORM_ARTIFACT IN ITEMS
            "${HUXERUI_ANDROID_ARTIFACT_ROOT}/HuxerUI.aar"
            "${HUXERUI_ANDROID_ARTIFACT_ROOT}/arm64-v8a/libhuxerui.so"
            "${HUXERUI_ANDROID_ARTIFACT_ROOT}/x86_64/libhuxerui.so"
            "${HUXERUI_WEB_ARTIFACT_ROOT}/libhuxerui.a"
    )
        if (NOT EXISTS "${HUXERUI_REQUIRED_PLATFORM_ARTIFACT}")
            message(FATAL_ERROR "HuxerUI SDK platform artifact is missing: ${HUXERUI_REQUIRED_PLATFORM_ARTIFACT}")
        endif ()
    endforeach ()
    install(DIRECTORY "${HUXERUI_ANDROID_ARTIFACT_ROOT}/"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/huxerui/platform/android"
    )
    install(DIRECTORY "${HUXERUI_WEB_ARTIFACT_ROOT}/"
            DESTINATION
            "${CMAKE_INSTALL_DATADIR}/huxerui/platform/web/emscripten-${HUXERUI_WEB_EMSCRIPTEN_VERSION}"
    )
    if (APPLE AND NOT IOS)
        set(HUXERUI_IOS_XCFRAMEWORK
                "${HUXERUI_SDK_ARTIFACT_ROOT}/ios/HuxerUI.xcframework"
        )
        foreach (HUXERUI_REQUIRED_IOS_ARTIFACT IN ITEMS
                "${HUXERUI_IOS_XCFRAMEWORK}/Info.plist"
                "${HUXERUI_IOS_XCFRAMEWORK}/ios-arm64/Headers/huxerui/data.h"
                "${HUXERUI_IOS_XCFRAMEWORK}/ios-arm64/Headers/huxerui/huxerui.h"
                "${HUXERUI_IOS_XCFRAMEWORK}/ios-arm64/libhuxerui_static.a"
                "${HUXERUI_IOS_XCFRAMEWORK}/ios-arm64_x86_64-simulator/Headers/huxerui/data.h"
                "${HUXERUI_IOS_XCFRAMEWORK}/ios-arm64_x86_64-simulator/Headers/huxerui/huxerui.h"
                "${HUXERUI_IOS_XCFRAMEWORK}/ios-arm64_x86_64-simulator/libhuxerui_static.a"
        )
            if (NOT EXISTS "${HUXERUI_REQUIRED_IOS_ARTIFACT}")
                message(FATAL_ERROR
                        "HuxerUI SDK iOS artifact is missing: ${HUXERUI_REQUIRED_IOS_ARTIFACT}"
                )
            endif ()
        endforeach ()
        install(DIRECTORY "${HUXERUI_IOS_XCFRAMEWORK}"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/huxerui/platform/ios"
        )
    endif ()
endif ()

install(DIRECTORY
        "${HUXERUI_PROJECT_DIR}/tools/prebuilt/${HUXERUI_SDK_HOST_PLATFORM}/${HUXERUI_SDK_HOST_ARCHITECTURE}/"
        DESTINATION
        "${CMAKE_INSTALL_DATADIR}/huxerui/tools/${HUXERUI_SDK_HOST_PLATFORM}/${HUXERUI_SDK_HOST_ARCHITECTURE}"
        USE_SOURCE_PERMISSIONS
        PATTERN ".DS_Store" EXCLUDE
)

if (HUXERUI_SDK_PACKAGE_FILE_NAME)
    set(CPACK_PACKAGE_NAME "HuxerUI SDK")
    set(CPACK_PACKAGE_VENDOR "HuxerUI")
    set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/HuxerUI/HuxerUI")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "HuxerUI cross-platform application development SDK")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME "${HUXERUI_SDK_PACKAGE_FILE_NAME}")
    set(CPACK_PACKAGE_CHECKSUM "SHA256")
    set(CPACK_RESOURCE_FILE_LICENSE "${HUXERUI_PROJECT_DIR}/LICENSE")
    set(CPACK_GENERATOR "${HUXERUI_SDK_PACKAGE_GENERATOR}")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    set(CPACK_MONOLITHIC_INSTALL ON)
    if (HUXERUI_SDK_HOST_PLATFORM STREQUAL "android")
        set(CPACK_STRIP_FILES "${CMAKE_INSTALL_BINDIR}/huxerui")
    endif ()
    if (WIN32 AND HUXERUI_INTERNAL_PACKAGE_WINDOWS_DEBUG)
        get_property(HUXERUI_SDK_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
        if (NOT HUXERUI_SDK_MULTI_CONFIG)
            message(FATAL_ERROR
                    "HuxerUI Windows SDK packaging with Debug libraries requires a multi-config generator")
        endif ()
        set(HUXERUI_WINDOWS_DEBUG_INSTALL_SCRIPT
                "${CMAKE_CURRENT_BINARY_DIR}/HuxerUIInstallWindowsDebug.cmake"
        )
        file(WRITE "${HUXERUI_WINDOWS_DEBUG_INSTALL_SCRIPT}"
                "execute_process(\n"
                "  COMMAND \"${CMAKE_COMMAND}\" --install \"${PROJECT_BINARY_DIR}\" --config Debug"
                " --component HuxerUILibraries --prefix \"\${CMAKE_INSTALL_PREFIX}\"\n"
                "  RESULT_VARIABLE install_result\n"
                "  OUTPUT_VARIABLE install_output\n"
                "  ERROR_VARIABLE install_error\n"
                ")\n"
                "if (NOT install_result EQUAL 0)\n"
                "  message(FATAL_ERROR \"HuxerUI Windows Debug SDK installation failed:\\n"
                "\${install_output}\${install_error}\")\n"
                "endif ()\n"
        )
        set(CPACK_INSTALL_SCRIPTS "${HUXERUI_WINDOWS_DEBUG_INSTALL_SCRIPT}")
    endif ()
    include(CPack)
endif ()
