include_guard(GLOBAL)

function(_huxerui_restore_wix_package package_name archive_sha256 destination)
    set(HUXERUI_WIX_REQUIRED_FILES ${ARGN})
    set(HUXERUI_WIX_PACKAGE_COMPLETE TRUE)
    foreach (HUXERUI_WIX_REQUIRED_FILE IN LISTS HUXERUI_WIX_REQUIRED_FILES)
        if (NOT EXISTS "${destination}/${HUXERUI_WIX_REQUIRED_FILE}")
            set(HUXERUI_WIX_PACKAGE_COMPLETE FALSE)
            break()
        endif ()
    endforeach ()
    if (HUXERUI_WIX_PACKAGE_COMPLETE)
        return()
    endif ()

    file(MAKE_DIRECTORY "${HUXERUI_WIX_ROOT}/downloads")
    string(TOLOWER "${package_name}" HUXERUI_WIX_PACKAGE_ID)
    set(HUXERUI_WIX_ARCHIVE
            "${HUXERUI_WIX_ROOT}/downloads/${HUXERUI_WIX_PACKAGE_ID}.${HUXERUI_WIX_VERSION}.nupkg"
    )
    file(DOWNLOAD
            "https://api.nuget.org/v3-flatcontainer/${HUXERUI_WIX_PACKAGE_ID}/${HUXERUI_WIX_VERSION}/${HUXERUI_WIX_PACKAGE_ID}.${HUXERUI_WIX_VERSION}.nupkg"
            "${HUXERUI_WIX_ARCHIVE}"
            EXPECTED_HASH "SHA256=${archive_sha256}"
            TLS_VERIFY ON
            SHOW_PROGRESS
    )
    file(REMOVE_RECURSE "${destination}")
    file(MAKE_DIRECTORY "${destination}")
    file(ARCHIVE_EXTRACT INPUT "${HUXERUI_WIX_ARCHIVE}" DESTINATION "${destination}")
    foreach (HUXERUI_WIX_REQUIRED_FILE IN LISTS HUXERUI_WIX_REQUIRED_FILES)
        if (NOT EXISTS "${destination}/${HUXERUI_WIX_REQUIRED_FILE}")
            message(FATAL_ERROR "Restored ${package_name} package is incomplete")
        endif ()
    endforeach ()
endfunction()

function(_huxerui_prepare_wix_dependencies)
    set(HUXERUI_WIX_VERSION "5.0.2")
    set(HUXERUI_WIX_TOOL_ARCHIVE_SHA256
            "f30ef0c74e2a986126539c5780be93ac24e8136eaf723b1937b26272703ae173"
    )
    set(HUXERUI_WIX_BOOTSTRAPPER_ARCHIVE_SHA256
            "6e0d3c68a68dcedde4a3a68de896f124a7b19c4a823fac49856e2ee77cb16256"
    )
    set(HUXERUI_WIX_DUTIL_ARCHIVE_SHA256
            "aa4f0668044318820e6c31ffef9f4141830c9fd8ebbe038281329423916547fe"
    )
    set(HUXERUI_WIX_TOOL_DIRECTORY "${HUXERUI_WIX_ROOT}/tool")
    set(HUXERUI_WIX_BOOTSTRAPPER_DIRECTORY "${HUXERUI_WIX_ROOT}/bootstrapper")
    set(HUXERUI_WIX_DUTIL_DIRECTORY "${HUXERUI_WIX_ROOT}/dutil")
    _huxerui_restore_wix_package(
            "wix"
            "${HUXERUI_WIX_TOOL_ARCHIVE_SHA256}"
            "${HUXERUI_WIX_TOOL_DIRECTORY}"
            "tools/net6.0/any/wix.exe"
    )
    _huxerui_restore_wix_package(
            "WixToolset.BootstrapperApplicationApi"
            "${HUXERUI_WIX_BOOTSTRAPPER_ARCHIVE_SHA256}"
            "${HUXERUI_WIX_BOOTSTRAPPER_DIRECTORY}"
            "build/native/include/BootstrapperApplication.h"
            "build/native/v14/x64/balutil.lib"
            "runtimes/win-x64/native/mbanative.dll"
    )
    _huxerui_restore_wix_package(
            "WixToolset.DUtil"
            "${HUXERUI_WIX_DUTIL_ARCHIVE_SHA256}"
            "${HUXERUI_WIX_DUTIL_DIRECTORY}"
            "build/native/include/dutil.h"
            "build/native/v14/x64/dutil.lib"
    )

    set(HUXERUI_WIX_EXECUTABLE "${HUXERUI_WIX_TOOL_DIRECTORY}/tools/net6.0/any/wix.exe")
    cmake_path(CONVERT "${HUXERUI_WIX_EXECUTABLE}" TO_CMAKE_PATH_LIST HUXERUI_WIX_EXECUTABLE NORMALIZE)
    execute_process(
            COMMAND "${HUXERUI_WIX_EXECUTABLE}" --version
            RESULT_VARIABLE HUXERUI_WIX_PROBE_RESULT
            OUTPUT_VARIABLE HUXERUI_WIX_PROBE_OUTPUT
            ERROR_VARIABLE HUXERUI_WIX_PROBE_ERROR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_STRIP_TRAILING_WHITESPACE
    )
    if (NOT HUXERUI_WIX_PROBE_RESULT STREQUAL "0")
        if (NOT HUXERUI_WIX_PROBE_ERROR)
            set(HUXERUI_WIX_PROBE_ERROR "${HUXERUI_WIX_PROBE_OUTPUT}")
        endif ()
        message(FATAL_ERROR
                "HuxerUI Windows packaging requires Microsoft.NETCore.App 6.0 or newer to run WiX: ${HUXERUI_WIX_PROBE_ERROR}"
        )
    endif ()

    set(HUXERUI_WIX_EXECUTABLE "${HUXERUI_WIX_EXECUTABLE}" PARENT_SCOPE)
    set(HUXERUI_WIX_BOOTSTRAPPER_INCLUDE
            "${HUXERUI_WIX_BOOTSTRAPPER_DIRECTORY}/build/native/include"
            PARENT_SCOPE
    )
    set(HUXERUI_WIX_BOOTSTRAPPER_LIBRARY
            "${HUXERUI_WIX_BOOTSTRAPPER_DIRECTORY}/build/native/v14/x64/balutil.lib"
            PARENT_SCOPE
    )
    set(HUXERUI_WIX_BOOTSTRAPPER_RUNTIME
            "${HUXERUI_WIX_BOOTSTRAPPER_DIRECTORY}/runtimes/win-x64/native/mbanative.dll"
            PARENT_SCOPE
    )
    set(HUXERUI_WIX_DUTIL_INCLUDE
            "${HUXERUI_WIX_DUTIL_DIRECTORY}/build/native/include"
            PARENT_SCOPE
    )
    set(HUXERUI_WIX_DUTIL_LIBRARY
            "${HUXERUI_WIX_DUTIL_DIRECTORY}/build/native/v14/x64/dutil.lib"
            PARENT_SCOPE
    )
endfunction()

function(huxerui_add_windows_installer target_name)
    if (NOT WIN32)
        message(FATAL_ERROR "huxerui_add_windows_installer() is available only on Windows")
    endif ()
    if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "HuxerUI Windows installer packaging currently supports x64 only")
    endif ()
    if (NOT HUXERUI_WIX_ROOT)
        message(FATAL_ERROR "huxerui_add_windows_installer() requires HUXERUI_WIX_ROOT")
    endif ()

    cmake_parse_arguments(HUXERUI_INSTALLER
            ""
            "RESOURCE_NAMESPACE;INTEGRATION_OUTPUT"
            "SOURCES;RESOURCES"
            ${ARGN}
    )
    if (NOT HUXERUI_INSTALLER_SOURCES)
        message(FATAL_ERROR "huxerui_add_windows_installer() requires SOURCES")
    endif ()
    if (NOT HUXERUI_INSTALLER_INTEGRATION_OUTPUT)
        message(FATAL_ERROR "huxerui_add_windows_installer() requires INTEGRATION_OUTPUT")
    endif ()
    _huxerui_prepare_wix_dependencies()

    if (EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../platform/windows/windows_installer.cpp")
        set(HUXERUI_WINDOWS_INSTALLER_SOURCE_DIRECTORY
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../platform/windows"
        )
    else ()
        set(HUXERUI_WINDOWS_INSTALLER_SOURCE_DIRECTORY "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    endif ()

    huxerui_add_app(${target_name}
            SOURCES
                ${HUXERUI_INSTALLER_SOURCES}
                "${HUXERUI_WINDOWS_INSTALLER_SOURCE_DIRECTORY}/windows_installer.cpp"
            RESOURCES
                ${HUXERUI_INSTALLER_RESOURCES}
            RESOURCE_NAMESPACE
                "${HUXERUI_INSTALLER_RESOURCE_NAMESPACE}"
    )
    target_include_directories(${target_name} PRIVATE
            "${HUXERUI_WINDOWS_INSTALLER_SOURCE_DIRECTORY}"
            "${HUXERUI_WIX_BOOTSTRAPPER_INCLUDE}"
            "${HUXERUI_WIX_DUTIL_INCLUDE}"
    )
    target_link_libraries(${target_name} PRIVATE
            "${HUXERUI_WIX_BOOTSTRAPPER_LIBRARY}"
            "${HUXERUI_WIX_DUTIL_LIBRARY}"
            version
    )
    target_compile_definitions(${target_name} PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
    )
    set_target_properties(${target_name} PROPERTIES WIN32_EXECUTABLE TRUE)
    add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${HUXERUI_WIX_BOOTSTRAPPER_RUNTIME}"
                    "$<TARGET_FILE_DIR:${target_name}>/mbanative.dll"
            VERBATIM
    )

    get_target_property(HUXERUI_INSTALLER_RESOURCE_PACKAGE
            ${target_name}
            HUXERUI_RESOURCE_PACKAGE
    )
    file(GENERATE
            OUTPUT "${HUXERUI_INSTALLER_INTEGRATION_OUTPUT}"
            CONTENT "{\n  \"schema\": 1,\n  \"wix\": \"${HUXERUI_WIX_EXECUTABLE}\",\n  \"installer\": \"$<TARGET_FILE:${target_name}>\",\n  \"installerResources\": \"${HUXERUI_INSTALLER_RESOURCE_PACKAGE}\",\n  \"installerResourcesName\": \"$<TARGET_FILE_BASE_NAME:${target_name}>.resources\"\n}\n"
    )
endfunction()
