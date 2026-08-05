cmake_minimum_required(VERSION 3.20)

foreach (required_variable IN ITEMS HUXERUI_PLATFORM_FILE HUXERUI_PLATFORM_PLAN_OUTPUT HUXERUI_SDK_ROOT)
    if (NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "HuxerUIAndroidPlan.cmake requires ${required_variable}")
    endif ()
endforeach ()
if (NOT EXISTS "${HUXERUI_PLATFORM_FILE}")
    message(FATAL_ERROR
            "HuxerUI Android configuration does not exist: ${HUXERUI_PLATFORM_FILE}"
    )
endif ()

include("${HUXERUI_PLATFORM_FILE}")

function(_huxerui_android_json_escape input output)
    string(REPLACE "\\" "\\\\" value "${input}")
    string(REPLACE "\"" "\\\"" value "${value}")
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(_huxerui_android_property properties_file property_name output_variable)
    file(STRINGS "${properties_file}" HUXERUI_PROPERTY
            REGEX "^${property_name}="
            LIMIT_COUNT 1
    )
    if (NOT HUXERUI_PROPERTY)
        message(FATAL_ERROR
                "HuxerUI Android SDK property is missing: ${property_name}"
        )
    endif ()
    string(REGEX REPLACE "^[^=]+=" "" HUXERUI_PROPERTY "${HUXERUI_PROPERTY}")
    set(${output_variable} "${HUXERUI_PROPERTY}" PARENT_SCOPE)
endfunction()

if (NOT HUXERUI_ANDROID_APPLICATION_ID)
    message(FATAL_ERROR
            "Android configuration requires HUXERUI_ANDROID_APPLICATION_ID"
    )
endif ()

set(HUXERUI_ANDROID_SOURCE_MODULE
        "${HUXERUI_SDK_ROOT}/platform/android/huxerui"
)
set(HUXERUI_ANDROID_PROPERTIES
        "${HUXERUI_SDK_ROOT}/platform/android/gradle.properties"
)
if (NOT EXISTS "${HUXERUI_ANDROID_SOURCE_MODULE}/build.gradle"
        OR NOT EXISTS "${HUXERUI_ANDROID_PROPERTIES}")
    message(FATAL_ERROR
            "Android builds currently require a HuxerUI source SDK checkout"
    )
endif ()

set(HUXERUI_ANDROID_CMAKE_PACKAGE_DIRECTORY
        "${HUXERUI_SDK_ROOT}/cmake"
)
set(HUXERUI_ANDROID_HOST_TOOL_DIRECTORY
        "${HUXERUI_SDK_ROOT}/tools/prebuilt"
)
_huxerui_android_property(
        "${HUXERUI_ANDROID_PROPERTIES}"
        huxeruiCompileSdk
        HUXERUI_ANDROID_SDK_COMPILE_SDK
)
_huxerui_android_property(
        "${HUXERUI_ANDROID_PROPERTIES}"
        huxeruiMinCompileSdk
        HUXERUI_ANDROID_SDK_MIN_COMPILE_SDK
)
_huxerui_android_property(
        "${HUXERUI_ANDROID_PROPERTIES}"
        huxeruiMinSdk
        HUXERUI_ANDROID_SDK_MIN_SDK
)
_huxerui_android_property(
        "${HUXERUI_ANDROID_PROPERTIES}"
        huxeruiNdkVersion
        HUXERUI_ANDROID_SDK_NDK_VERSION
)
_huxerui_android_property(
        "${HUXERUI_ANDROID_PROPERTIES}"
        huxeruiAbis
        HUXERUI_ANDROID_SDK_ABIS
)
_huxerui_android_property(
        "${HUXERUI_ANDROID_PROPERTIES}"
        huxeruiStl
        HUXERUI_ANDROID_SDK_STL
)
string(REPLACE "," ";" HUXERUI_ANDROID_SDK_ABIS
        "${HUXERUI_ANDROID_SDK_ABIS}"
)

if (NOT EXISTS "${HUXERUI_ANDROID_CMAKE_PACKAGE_DIRECTORY}/HuxerUIApp.cmake")
    message(FATAL_ERROR
            "HuxerUI SDK CMake helpers are missing: ${HUXERUI_ANDROID_CMAKE_PACKAGE_DIRECTORY}"
    )
endif ()
if (NOT IS_DIRECTORY "${HUXERUI_ANDROID_HOST_TOOL_DIRECTORY}")
    message(FATAL_ERROR
            "HuxerUI SDK host tools are missing: ${HUXERUI_ANDROID_HOST_TOOL_DIRECTORY}"
    )
endif ()
if (NOT HUXERUI_ANDROID_COMPILE_SDK)
    set(HUXERUI_ANDROID_COMPILE_SDK "${HUXERUI_ANDROID_SDK_COMPILE_SDK}")
endif ()
if (NOT HUXERUI_ANDROID_MIN_SDK)
    set(HUXERUI_ANDROID_MIN_SDK "${HUXERUI_ANDROID_SDK_MIN_SDK}")
endif ()
if (NOT HUXERUI_ANDROID_TARGET_SDK)
    set(HUXERUI_ANDROID_TARGET_SDK "${HUXERUI_ANDROID_COMPILE_SDK}")
endif ()
if (NOT HUXERUI_ANDROID_NDK_VERSION)
    set(HUXERUI_ANDROID_NDK_VERSION "${HUXERUI_ANDROID_SDK_NDK_VERSION}")
endif ()
if (NOT HUXERUI_ANDROID_ABIS)
    set(HUXERUI_ANDROID_ABIS ${HUXERUI_ANDROID_SDK_ABIS})
endif ()

if (HUXERUI_ANDROID_COMPILE_SDK LESS HUXERUI_ANDROID_SDK_MIN_COMPILE_SDK)
    message(FATAL_ERROR
            "Android compileSdk ${HUXERUI_ANDROID_COMPILE_SDK} is lower than the HuxerUI SDK minCompileSdk ${HUXERUI_ANDROID_SDK_MIN_COMPILE_SDK}"
    )
endif ()
if (HUXERUI_ANDROID_MIN_SDK LESS HUXERUI_ANDROID_SDK_MIN_SDK)
    message(FATAL_ERROR
            "Android minSdk ${HUXERUI_ANDROID_MIN_SDK} is lower than the HuxerUI SDK requirement ${HUXERUI_ANDROID_SDK_MIN_SDK}"
    )
endif ()
if (HUXERUI_ANDROID_TARGET_SDK LESS HUXERUI_ANDROID_MIN_SDK
        OR HUXERUI_ANDROID_TARGET_SDK GREATER HUXERUI_ANDROID_COMPILE_SDK)
    message(FATAL_ERROR
            "Android targetSdk ${HUXERUI_ANDROID_TARGET_SDK} must be between minSdk ${HUXERUI_ANDROID_MIN_SDK} and compileSdk ${HUXERUI_ANDROID_COMPILE_SDK}"
    )
endif ()
if (NOT HUXERUI_ANDROID_NDK_VERSION STREQUAL HUXERUI_ANDROID_SDK_NDK_VERSION)
    message(FATAL_ERROR
            "Android NDK ${HUXERUI_ANDROID_NDK_VERSION} does not match the HuxerUI SDK NDK ${HUXERUI_ANDROID_SDK_NDK_VERSION}"
    )
endif ()
foreach (HUXERUI_ANDROID_ABI IN LISTS HUXERUI_ANDROID_ABIS)
    if (NOT HUXERUI_ANDROID_ABI IN_LIST HUXERUI_ANDROID_SDK_ABIS)
        message(FATAL_ERROR
                "Android ABI ${HUXERUI_ANDROID_ABI} is not provided by the HuxerUI SDK"
        )
    endif ()
endforeach ()

_huxerui_android_json_escape(
        "${HUXERUI_ANDROID_APPLICATION_ID}"
        HUXERUI_ANDROID_JSON_APPLICATION_ID
)
_huxerui_android_json_escape(
        "${HUXERUI_ANDROID_NDK_VERSION}"
        HUXERUI_ANDROID_JSON_NDK_VERSION
)
_huxerui_android_json_escape(
        "${HUXERUI_ANDROID_SDK_STL}"
        HUXERUI_ANDROID_JSON_STL
)
_huxerui_android_json_escape(
        "${HUXERUI_ANDROID_SOURCE_MODULE}"
        HUXERUI_ANDROID_JSON_SOURCE_MODULE
)
_huxerui_android_json_escape(
        "${HUXERUI_ANDROID_CMAKE_PACKAGE_DIRECTORY}"
        HUXERUI_ANDROID_JSON_CMAKE_PACKAGE_DIRECTORY
)
_huxerui_android_json_escape(
        "${HUXERUI_ANDROID_HOST_TOOL_DIRECTORY}"
        HUXERUI_ANDROID_JSON_HOST_TOOL_DIRECTORY
)
set(HUXERUI_ANDROID_JSON_ABIS)
foreach (HUXERUI_ANDROID_ABI IN LISTS HUXERUI_ANDROID_ABIS)
    _huxerui_android_json_escape(
            "${HUXERUI_ANDROID_ABI}"
            HUXERUI_ANDROID_JSON_ABI
    )
    if (HUXERUI_ANDROID_JSON_ABIS)
        string(APPEND HUXERUI_ANDROID_JSON_ABIS ", ")
    endif ()
    string(APPEND HUXERUI_ANDROID_JSON_ABIS
            "\"${HUXERUI_ANDROID_JSON_ABI}\""
    )
endforeach ()

get_filename_component(HUXERUI_PLATFORM_PLAN_DIRECTORY
        "${HUXERUI_PLATFORM_PLAN_OUTPUT}"
        DIRECTORY
)
file(MAKE_DIRECTORY "${HUXERUI_PLATFORM_PLAN_DIRECTORY}")
file(WRITE "${HUXERUI_PLATFORM_PLAN_OUTPUT}"
        "{\n  \"schema\": 1,\n  \"platform\": \"android\",\n  \"applicationId\": \"${HUXERUI_ANDROID_JSON_APPLICATION_ID}\",\n  \"compileSdk\": ${HUXERUI_ANDROID_COMPILE_SDK},\n  \"minSdk\": ${HUXERUI_ANDROID_MIN_SDK},\n  \"targetSdk\": ${HUXERUI_ANDROID_TARGET_SDK},\n  \"ndkVersion\": \"${HUXERUI_ANDROID_JSON_NDK_VERSION}\",\n  \"stl\": \"${HUXERUI_ANDROID_JSON_STL}\",\n  \"abis\": [${HUXERUI_ANDROID_JSON_ABIS}],\n  \"huxeruiSourceDirectory\": \"${HUXERUI_ANDROID_JSON_SOURCE_MODULE}\",\n  \"cmakePackageDirectory\": \"${HUXERUI_ANDROID_JSON_CMAKE_PACKAGE_DIRECTORY}\",\n  \"hostToolDirectory\": \"${HUXERUI_ANDROID_JSON_HOST_TOOL_DIRECTORY}\"\n}\n"
)
