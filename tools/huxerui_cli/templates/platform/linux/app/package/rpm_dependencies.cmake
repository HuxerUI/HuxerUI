if (NOT CPACK_GENERATOR STREQUAL "RPM")
    return()
endif ()

file(GLOB_RECURSE staged_files LIST_DIRECTORIES FALSE
        "${CPACK_TEMPORARY_DIRECTORY}/${HUXERUI_PACKAGE_APP_DIRECTORY}/*")
set(elf_files)
foreach (file IN LISTS staged_files)
    if (IS_SYMLINK "${file}")
        continue()
    endif ()
    file(READ "${file}" magic LIMIT 4 HEX)
    if (magic STREQUAL "7f454c46")
        list(APPEND elf_files "${file}")
    endif ()
endforeach ()
if (NOT elf_files)
    return()
endif ()

find_program(RPMBUILD_EXECUTABLE rpmbuild REQUIRED)
execute_process(COMMAND "${RPMBUILD_EXECUTABLE}" --eval "%{_rpmconfigdir}"
        OUTPUT_VARIABLE rpm_config_directory OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${rpm_config_directory}/elfdeps" --provides ${elf_files}
        OUTPUT_VARIABLE private_provides OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
if (private_provides STREQUAL "")
    return()
endif ()

# Match the full RPM capability, including symbol versions and architecture. Escape it for both the regex and RPM spec.
string(REGEX REPLACE [=[([][.^$*+()?{}|\\])]=] [=[\\\\\1]=] dependency_filter "${private_provides}")
string(REPLACE "%" "%%" dependency_filter "${dependency_filter}")
string(REPLACE "\n" "|" dependency_filter "${dependency_filter}")
string(APPEND CPACK_RPM_SPEC_MORE_DEFINE
        "\n%global __provides_exclude ^(${dependency_filter})$%{?__provides_exclude:|%{__provides_exclude}}"
        "\n%global __requires_exclude ^(${dependency_filter})$%{?__requires_exclude:|%{__requires_exclude}}")
