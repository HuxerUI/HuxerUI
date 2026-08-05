foreach (required_variable IN ITEMS SOURCE_DIRECTORY WORK_DIRECTORY)
    if (NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif ()
endforeach ()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef TEST_NONCE)
set(TEST_ROOT "${WORK_DIRECTORY}/huxerui-android-plan-${TEST_NONCE}")
set(PLATFORM_FILE "${TEST_ROOT}/huxerui.cmake")
set(SOURCE_PLAN "${TEST_ROOT}/source.json")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(WRITE "${PLATFORM_FILE}"
        "set(HUXERUI_ANDROID_APPLICATION_ID \"org.huxerui.test\")\n"
        "set(HUXERUI_ANDROID_ABIS arm64-v8a)\n"
)

execute_process(
        COMMAND "${CMAKE_COMMAND}"
                "-DHUXERUI_PLATFORM_FILE=${PLATFORM_FILE}"
                "-DHUXERUI_PLATFORM_PLAN_OUTPUT=${SOURCE_PLAN}"
                "-DHUXERUI_SDK_ROOT=${SOURCE_DIRECTORY}"
                -P "${SOURCE_DIRECTORY}/cmake/HuxerUIAndroidPlan.cmake"
        RESULT_VARIABLE SOURCE_RESULT
        OUTPUT_VARIABLE SOURCE_OUTPUT
        ERROR_VARIABLE SOURCE_ERROR
)
if (NOT SOURCE_RESULT EQUAL 0)
    message(FATAL_ERROR
            "Source SDK Android plan failed:\n${SOURCE_OUTPUT}${SOURCE_ERROR}"
    )
endif ()
file(READ "${SOURCE_PLAN}" SOURCE_JSON)
string(JSON SOURCE_MODULE GET "${SOURCE_JSON}" huxeruiSourceDirectory)
string(JSON SOURCE_ABI GET "${SOURCE_JSON}" abis 0)
if (NOT SOURCE_MODULE STREQUAL "${SOURCE_DIRECTORY}/platform/android/huxerui"
        OR NOT SOURCE_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Source SDK Android plan contains unexpected values")
endif ()

set(INCOMPATIBLE_PLATFORM_FILE "${TEST_ROOT}/incompatible.cmake")
file(WRITE "${INCOMPATIBLE_PLATFORM_FILE}"
        "set(HUXERUI_ANDROID_APPLICATION_ID \"org.huxerui.test\")\n"
        "set(HUXERUI_ANDROID_ABIS armeabi-v7a)\n"
)
execute_process(
        COMMAND "${CMAKE_COMMAND}"
                "-DHUXERUI_PLATFORM_FILE=${INCOMPATIBLE_PLATFORM_FILE}"
                "-DHUXERUI_PLATFORM_PLAN_OUTPUT=${TEST_ROOT}/incompatible.json"
                "-DHUXERUI_SDK_ROOT=${SOURCE_DIRECTORY}"
                -P "${SOURCE_DIRECTORY}/cmake/HuxerUIAndroidPlan.cmake"
        RESULT_VARIABLE INCOMPATIBLE_RESULT
        OUTPUT_VARIABLE INCOMPATIBLE_OUTPUT
        ERROR_VARIABLE INCOMPATIBLE_ERROR
)
if (INCOMPATIBLE_RESULT EQUAL 0
        OR NOT INCOMPATIBLE_ERROR MATCHES
        "Android ABI armeabi-v7a is not provided by the HuxerUI SDK")
    message(FATAL_ERROR
            "Incompatible Android ABI was not rejected:\n${INCOMPATIBLE_OUTPUT}${INCOMPATIBLE_ERROR}"
    )
endif ()

file(REMOVE_RECURSE "${TEST_ROOT}")
