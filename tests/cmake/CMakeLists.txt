add_test(NAME HuxerUIProfilingBuildTests
        COMMAND "${CMAKE_COMMAND}"
                "-DLIBRARY_FILE=$<TARGET_FILE:${HUXERUI_TEST_LIBRARY}>"
                "-DTEST_EXECUTABLE=$<TARGET_FILE:huxerui_runtime_tests>"
                "-DEXPECT_PROFILING=$<BOOL:${HUXERUI_ENABLE_PROFILING}>"
                "-DCROSSCOMPILING=$<BOOL:${CMAKE_CROSSCOMPILING}>"
                "-DBUILD_DIRECTORY=${PROJECT_BINARY_DIR}"
                "-DBUILD_CONFIG=$<CONFIG>"
                "-DSDK_INSTALL_AVAILABLE=$<BOOL:${HUXERUI_SDK_HOST_PLATFORM}>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/profiling.cmake"
)
set_tests_properties(HuxerUIProfilingBuildTests PROPERTIES LABELS "build")

if (CMAKE_CROSSCOMPILING)
    return()
endif ()

add_test(NAME HuxerUIResourceMergeTests
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
                "-DHOST_GENERATOR=${CMAKE_GENERATOR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/resource_merge.cmake"
)

add_test(NAME HuxerUILibraryValidationTests
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
                "-DHOST_GENERATOR=${CMAKE_GENERATOR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/library_validation.cmake"
)

if (NOT CMAKE_CONFIGURATION_TYPES)
    add_test(NAME HuxerUIAndroidAppIntegrationTests
            COMMAND "${CMAKE_COMMAND}"
                    "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                    "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
                    "-DHOST_GENERATOR=${CMAKE_GENERATOR}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/android_app_integration.cmake"
    )
endif ()

if (WIN32)
    set(HUXERUI_SOURCE_SUBDIRECTORY_WORK_DIRECTORY "$ENV{TEMP}")
else ()
    set(HUXERUI_SOURCE_SUBDIRECTORY_WORK_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif ()
if (WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME HuxerUIRuntimeDependencyTests
            COMMAND "${CMAKE_COMMAND}"
                    "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                    "-DWORK_DIRECTORY=${HUXERUI_SOURCE_SUBDIRECTORY_WORK_DIRECTORY}"
                    "-DHOST_GENERATOR=${CMAKE_GENERATOR}"
                    "-DHOST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                    "-DHOST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                    "-DHOST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/runtime_dependencies.cmake"
    )
endif ()
add_test(NAME HuxerUISourceSubdirectoryTests
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                "-DWORK_DIRECTORY=${HUXERUI_SOURCE_SUBDIRECTORY_WORK_DIRECTORY}"
                "-DHOST_GENERATOR=${CMAKE_GENERATOR}"
                "-DHOST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                "-DHOST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                "-DHOST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/source_subdirectory.cmake"
)
if (MSVC AND CMAKE_GENERATOR MATCHES "^Ninja")
    set(HUXERUI_SOURCE_SUBDIRECTORY_ENVIRONMENT)
    foreach (HUXERUI_MSVC_ENVIRONMENT_VARIABLE IN ITEMS PATH INCLUDE LIB LIBPATH)
        string(REPLACE ";" "\\;" HUXERUI_MSVC_ENVIRONMENT_VALUE
                "$ENV{${HUXERUI_MSVC_ENVIRONMENT_VARIABLE}}"
        )
        list(APPEND HUXERUI_SOURCE_SUBDIRECTORY_ENVIRONMENT
                "${HUXERUI_MSVC_ENVIRONMENT_VARIABLE}=${HUXERUI_MSVC_ENVIRONMENT_VALUE}"
        )
    endforeach ()
    set_tests_properties(HuxerUISourceSubdirectoryTests PROPERTIES
            ENVIRONMENT "${HUXERUI_SOURCE_SUBDIRECTORY_ENVIRONMENT}"
    )
    if (TEST HuxerUIRuntimeDependencyTests)
        set_tests_properties(HuxerUIRuntimeDependencyTests PROPERTIES
                ENVIRONMENT "${HUXERUI_SOURCE_SUBDIRECTORY_ENVIRONMENT}"
        )
    endif ()
endif ()

if (TARGET huxerui_cli_core)
    if (WIN32)
        set(HUXERUI_GENERATED_LIBRARY_PLATFORM windows)
    elseif (APPLE)
        set(HUXERUI_GENERATED_LIBRARY_PLATFORM macos)
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(HUXERUI_GENERATED_LIBRARY_PLATFORM linux)
    else ()
        unset(HUXERUI_GENERATED_LIBRARY_PLATFORM)
    endif ()
    if (TARGET huxerui_cli AND HUXERUI_GENERATED_LIBRARY_PLATFORM)
        add_test(NAME HuxerUIGeneratedLibraryTests
                COMMAND "${CMAKE_COMMAND}"
                        "-DCLI_EXECUTABLE=$<TARGET_FILE:huxerui_cli>"
                        "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                        "-DWORK_DIRECTORY=${HUXERUI_SOURCE_SUBDIRECTORY_WORK_DIRECTORY}"
                        "-DBUILD_CONFIG=$<CONFIG>"
                        "-DHOST_GENERATOR=${CMAKE_GENERATOR}"
                        "-DHOST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                        "-DHOST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                        "-DHOST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                        "-DPLATFORM_ID=${HUXERUI_GENERATED_LIBRARY_PLATFORM}"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/generated_library.cmake"
        )
        if (MSVC AND CMAKE_GENERATOR MATCHES "^Ninja")
            set_tests_properties(HuxerUIGeneratedLibraryTests PROPERTIES
                    ENVIRONMENT "${HUXERUI_SOURCE_SUBDIRECTORY_ENVIRONMENT}"
            )
        endif ()
    endif ()
endif ()

if (TARGET huxerui_cli AND NOT HUXERUI_ENABLE_PROFILING
        AND (WIN32 OR APPLE OR (UNIX AND NOT APPLE AND NOT EMSCRIPTEN)))
    if (WIN32)
        set(HUXERUI_INSTALLED_CONSUMER_PLATFORM windows)
    elseif (APPLE)
        set(HUXERUI_INSTALLED_CONSUMER_PLATFORM macos)
    else ()
        set(HUXERUI_INSTALLED_CONSUMER_PLATFORM linux)
    endif ()
    # SDK discovery must run outside the source tree and any ancestor application project.
    if (WIN32)
        set(HUXERUI_INSTALLED_CONSUMER_WORK_DIRECTORY "$ENV{TEMP}")
    elseif (DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
        set(HUXERUI_INSTALLED_CONSUMER_WORK_DIRECTORY "$ENV{TMPDIR}")
    else ()
        set(HUXERUI_INSTALLED_CONSUMER_WORK_DIRECTORY "/tmp")
    endif ()
    add_test(NAME HuxerUIInstalledConsumerTests
            COMMAND "${CMAKE_COMMAND}"
                    "-DBUILD_DIRECTORY=${PROJECT_BINARY_DIR}"
                    "-DBUILD_CONFIG=$<CONFIG>"
                    "-DWORK_DIRECTORY=${HUXERUI_INSTALLED_CONSUMER_WORK_DIRECTORY}"
                    "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                    "-DINSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
                    "-DCLI_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
                    "-DPLATFORM_ID=${HUXERUI_INSTALLED_CONSUMER_PLATFORM}"
                    "-DHOST_GENERATOR=${CMAKE_GENERATOR}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/installed_consumer.cmake"
    )
    # This test performs a fresh source build in addition to relocated SDK consumer checks.
    set_tests_properties(HuxerUIInstalledConsumerTests PROPERTIES TIMEOUT 1200)
endif ()

if (TARGET huxerui_cli AND HUXERUI_SDK_PACKAGE_FILE_NAME AND NOT HUXERUI_ENABLE_PROFILING)
    if (WIN32)
        set(HUXERUI_SDK_ARCHIVE_WORK_DIRECTORY "$ENV{TEMP}")
    elseif (DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
        set(HUXERUI_SDK_ARCHIVE_WORK_DIRECTORY "$ENV{TMPDIR}")
    else ()
        set(HUXERUI_SDK_ARCHIVE_WORK_DIRECTORY "/tmp")
    endif ()
    add_test(NAME HuxerUISdkArchiveTests
            COMMAND "${CMAKE_COMMAND}"
                    "-DBUILD_DIRECTORY=${PROJECT_BINARY_DIR}"
                    "-DBUILD_CONFIG=$<CONFIG>"
                    "-DWORK_DIRECTORY=${HUXERUI_SDK_ARCHIVE_WORK_DIRECTORY}"
                    "-DCPACK_COMMAND=${CMAKE_CPACK_COMMAND}"
                    "-DPACKAGE_FILE_NAME=${HUXERUI_SDK_PACKAGE_FILE_NAME}"
                    "-DPACKAGE_GENERATOR=${HUXERUI_SDK_PACKAGE_GENERATOR}"
                    "-DPACKAGE_EXTENSION=${HUXERUI_SDK_PACKAGE_EXTENSION}"
                    "-DINSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
                    "-DINSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
                    "-DCLI_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
                    "-DHOST_PLATFORM=${HUXERUI_SDK_HOST_PLATFORM}"
                    "-DHOST_ARCHITECTURE=${HUXERUI_SDK_HOST_ARCHITECTURE}"
                    "-DPLATFORM_ARTIFACTS_INCLUDED=$<BOOL:${HUXERUI_INTERNAL_SDK_ARTIFACT_ROOT}>"
                    "-DWINDOWS_DEBUG_INCLUDED=$<BOOL:${HUXERUI_INTERNAL_PACKAGE_WINDOWS_DEBUG}>"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/sdk_archive.cmake"
    )
    set_tests_properties(HuxerUISdkArchiveTests PROPERTIES RUN_SERIAL TRUE)

    add_test(NAME HuxerUISdkReleaseTests
            COMMAND "${CMAKE_COMMAND}"
                    "-DSOURCE_DIRECTORY=${HUXERUI_PROJECT_DIR}"
                    "-DWORK_DIRECTORY=${HUXERUI_SDK_ARCHIVE_WORK_DIRECTORY}"
                    "-DVALIDATOR_PATH=${HUXERUI_PROJECT_DIR}/cmake/ValidateSdkRelease.cmake"
                    "-DPROJECT_VERSION=${PROJECT_VERSION}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/sdk_release.cmake"
    )

    if (APPLE OR (UNIX AND NOT APPLE AND NOT EMSCRIPTEN))
        add_test(NAME HuxerUISdkInstallerTests
                COMMAND "${CMAKE_COMMAND}"
                        "-DBUILD_DIRECTORY=${PROJECT_BINARY_DIR}"
                        "-DBUILD_CONFIG=$<CONFIG>"
                        "-DWORK_DIRECTORY=${HUXERUI_SDK_ARCHIVE_WORK_DIRECTORY}"
                        "-DCPACK_COMMAND=${CMAKE_CPACK_COMMAND}"
                        "-DPACKAGE_FILE_NAME=${HUXERUI_SDK_PACKAGE_FILE_NAME}"
                        "-DPACKAGE_GENERATOR=${HUXERUI_SDK_PACKAGE_GENERATOR}"
                        "-DPACKAGE_EXTENSION=${HUXERUI_SDK_PACKAGE_EXTENSION}"
                        "-DINSTALLER_PATH=${HUXERUI_PROJECT_DIR}/install.sh"
                        "-DHOST_PLATFORM=${HUXERUI_SDK_HOST_PLATFORM}"
                        "-DHOST_ARCHITECTURE=${HUXERUI_SDK_HOST_ARCHITECTURE}"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/sdk_installer.cmake"
        )
        set_tests_properties(HuxerUISdkInstallerTests PROPERTIES RUN_SERIAL TRUE)
    endif ()
endif ()

get_property(integration_tests DIRECTORY PROPERTY TESTS)
foreach (integration_test IN LISTS integration_tests)
    set_tests_properties(${integration_test} PROPERTIES LABELS "build")
endforeach ()
if (TEST HuxerUIAndroidAppIntegrationTests)
    set_tests_properties(HuxerUIAndroidAppIntegrationTests PROPERTIES LABELS "build;platform;android;portable")
endif ()
