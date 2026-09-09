if (NOT TARGET huxerui_cli_core)
    return()
endif ()
file(TO_CMAKE_PATH "${HUXERUI_PROJECT_DIR}" HUXERUI_TEST_SOURCE_DIRECTORY)
function(huxerui_add_cli_test_suite target_name test_name)
    huxerui_add_test_suite(${target_name} ${test_name}
            LIBRARIES huxerui_cli_core
            ${ARGN}
    )
    target_include_directories(${target_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(${target_name} PRIVATE
            HUXERUI_TEST_SOURCE_DIRECTORY="${HUXERUI_TEST_SOURCE_DIRECTORY}"
    )
endfunction()
huxerui_add_cli_test_suite(huxerui_cli_tests HuxerUICliTests
        LABELS tools cli
        SOURCES commands.cpp project.cpp sdk.cpp process.cpp
)
huxerui_add_cli_test_suite(huxerui_cli_windows_tests HuxerUICliWindowsTests
        LABELS tools cli platform windows portable
        SOURCES platform/windows.cpp
)
huxerui_add_cli_test_suite(huxerui_cli_linux_tests HuxerUICliLinuxTests
        LABELS tools cli platform linux portable
        SOURCES platform/linux.cpp
)
huxerui_add_cli_test_suite(huxerui_cli_macos_tests HuxerUICliMacOSTests
        LABELS tools cli platform macos portable
        SOURCES platform/macos.cpp
)
huxerui_add_cli_test_suite(huxerui_cli_android_tests HuxerUICliAndroidTests
        LABELS tools cli platform android portable
        SOURCES platform/android.cpp
)
huxerui_add_cli_test_suite(huxerui_cli_ios_tests HuxerUICliIosTests
        LABELS tools cli platform ios portable
        SOURCES platform/ios.cpp
)
huxerui_add_cli_test_suite(huxerui_cli_web_tests HuxerUICliWebTests
        LABELS tools cli platform web portable
        SOURCES platform/web.cpp
)
