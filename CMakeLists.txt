cmake_minimum_required(VERSION 3.20)

if (CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum iOS deployment target")
endif ()

project(huxerui VERSION 0.1.0 LANGUAGES CXX)

if (APPLE)
    enable_language(OBJCXX)
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

if (PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR AND NOT ANDROID)
    set(HUXERUI_DEFAULT_BUILD_EXAMPLES ON)
    if (EMSCRIPTEN OR IOS)
        set(HUXERUI_DEFAULT_BUILD_TESTS OFF)
        set(HUXERUI_DEFAULT_BUILD_CLI OFF)
    else ()
        set(HUXERUI_DEFAULT_BUILD_TESTS ON)
        set(HUXERUI_DEFAULT_BUILD_CLI ON)
    endif ()
else ()
    set(HUXERUI_DEFAULT_BUILD_TESTS OFF)
    set(HUXERUI_DEFAULT_BUILD_EXAMPLES OFF)
    set(HUXERUI_DEFAULT_BUILD_CLI OFF)
endif ()

if (EMSCRIPTEN OR IOS)
    set(HUXERUI_DEFAULT_BUILD_SHARED OFF)
else ()
    set(HUXERUI_DEFAULT_BUILD_SHARED ON)
endif ()

option(HUXERUI_BUILD_SHARED "Build the HuxerUI shared library" ${HUXERUI_DEFAULT_BUILD_SHARED})
option(HUXERUI_BUILD_STATIC "Build the HuxerUI static library" ON)
option(HUXERUI_BUILD_TESTS "Build HuxerUI tests" ${HUXERUI_DEFAULT_BUILD_TESTS})
option(HUXERUI_BUILD_EXAMPLES "Build HuxerUI examples" ${HUXERUI_DEFAULT_BUILD_EXAMPLES})
option(HUXERUI_BUILD_CLI "Build the HuxerUI command-line tool" ${HUXERUI_DEFAULT_BUILD_CLI})
if (WIN32)
    option(HUXERUI_WINDOWS_7_COMPAT "Build the Windows backend for Windows 7 SP1 with Platform Update" OFF)
endif ()

set(HUXERUI_PROJECT_DIR "${PROJECT_SOURCE_DIR}")
set(HUXERUI_PUBLIC_INCLUDE_DIR "${HUXERUI_PROJECT_DIR}/include")
set(HUXERUI_PUBLIC_HEADER_DIR "${HUXERUI_PUBLIC_INCLUDE_DIR}/huxerui")
set(HUXERUI_SHARED_LIB_NAME "${PROJECT_NAME}")
set(HUXERUI_STATIC_LIB_NAME "${PROJECT_NAME}_static")

include(cmake/HuxerUITargets.cmake)
huxerui_configure_targets()
include(cmake/HuxerUIApp.cmake)
include(cmake/HuxerUIModules.cmake)

if (HUXERUI_MODULE_GRAPH_ONLY)
    return()
endif ()

if (HUXERUI_BUILD_CLI)
    add_subdirectory(tools/huxerui_cli)
endif ()

if (HUXERUI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

if (HUXERUI_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()

include(GNUInstallDirs)
set(HUXERUI_INSTALL_TARGETS)
if (TARGET ${HUXERUI_SHARED_LIB_NAME})
    list(APPEND HUXERUI_INSTALL_TARGETS ${HUXERUI_SHARED_LIB_NAME})
endif ()
if (TARGET ${HUXERUI_STATIC_LIB_NAME})
    list(APPEND HUXERUI_INSTALL_TARGETS ${HUXERUI_STATIC_LIB_NAME})
endif ()

if (HUXERUI_INSTALL_TARGETS)
    install(TARGETS ${HUXERUI_INSTALL_TARGETS}
            EXPORT HuxerUIImportedTargets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif ()

install(DIRECTORY "${HUXERUI_PUBLIC_HEADER_DIR}"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)
install(DIRECTORY "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/huxerui/resources"
)

include(CMakePackageConfigHelpers)
set(HUXERUI_INSTALL_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/HuxerUI")

configure_package_config_file(
        "${HUXERUI_PROJECT_DIR}/cmake/HuxerUIConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/HuxerUIConfig.cmake"
        INSTALL_DESTINATION "${HUXERUI_INSTALL_CMAKE_DIR}"
)
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/HuxerUIConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion
)

install(EXPORT HuxerUIImportedTargets
        FILE HuxerUIImportedTargets.cmake
        NAMESPACE HuxerUI::
        DESTINATION "${HUXERUI_INSTALL_CMAKE_DIR}"
)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/HuxerUIConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/HuxerUIConfigVersion.cmake"
        "${HUXERUI_PROJECT_DIR}/cmake/HuxerUIApp.cmake"
        "${HUXERUI_PROJECT_DIR}/cmake/HuxerUIModules.cmake"
        "${HUXERUI_PROJECT_DIR}/cmake/HuxerUIResources.cmake"
        "${HUXERUI_PROJECT_DIR}/cmake/HuxerUITargets.cmake"
        DESTINATION "${HUXERUI_INSTALL_CMAKE_DIR}"
)
install(FILES
        "${HUXERUI_PROJECT_DIR}/platform/web/web_file.js"
        DESTINATION "${HUXERUI_INSTALL_CMAKE_DIR}"
        RENAME HuxerUIWebFileSystem.js
)
install(DIRECTORY "${HUXERUI_PROJECT_DIR}/cmake/platform"
        DESTINATION "${HUXERUI_INSTALL_CMAKE_DIR}"
)
install(DIRECTORY "${HUXERUI_PROJECT_DIR}/tools/prebuilt/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/huxerui/tools"
        USE_SOURCE_PERMISSIONS
        PATTERN "README.md" EXCLUDE
        PATTERN ".DS_Store" EXCLUDE
)
