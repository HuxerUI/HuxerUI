if (CMAKE_CROSSCOMPILING)
    return()
endif ()

if (WIN32)
    find_program(HUXERUI_POWERSHELL_COMMAND NAMES pwsh powershell)
    if (HUXERUI_POWERSHELL_COMMAND)
        add_test(NAME HuxerUIToolBuildScriptTests
                COMMAND "${HUXERUI_POWERSHELL_COMMAND}"
                        -NoProfile
                        -ExecutionPolicy Bypass
                        -File "${HUXERUI_PROJECT_DIR}/scripts/build_tools.ps1"
                        -h
        )
        add_test(NAME HuxerUISdkPackageScriptTests
                COMMAND "${HUXERUI_POWERSHELL_COMMAND}"
                        -NoProfile
                        -ExecutionPolicy Bypass
                        -File "${HUXERUI_PROJECT_DIR}/scripts/package_sdk.ps1"
                        -Help
        )
    endif ()
elseif (UNIX)
    add_test(NAME HuxerUIToolBuildScriptTests
            COMMAND sh "${HUXERUI_PROJECT_DIR}/scripts/build_tools.sh" -h
    )
    add_test(NAME HuxerUIToolBuildGeneratorTests
            COMMAND sh
                    "${HUXERUI_PROJECT_DIR}/tests/scripts/build_tools_generator.sh"
                    "${HUXERUI_PROJECT_DIR}"
    )
    add_test(NAME HuxerUISdkPackageScriptTests
            COMMAND sh "${HUXERUI_PROJECT_DIR}/scripts/package_sdk.sh" --help
    )
endif ()

if (TARGET huxerui_cli_core AND UNIX)
    add_test(NAME HuxerUISdkUpdateTests
            COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/sdk_update.sh"
                    "${HUXERUI_PROJECT_DIR}/install.sh" "$<TARGET_FILE:huxerui_cli>"
    )
endif ()
get_property(script_tests DIRECTORY PROPERTY TESTS)
foreach (script_test IN LISTS script_tests)
    set_tests_properties(${script_test} PROPERTIES LABELS "tools;scripts")
endforeach ()
