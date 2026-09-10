foreach (required IN ITEMS SOURCE_DIRECTORY WORK_DIRECTORY HOST_GENERATOR HOST_CXX_COMPILER)
    if (NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif ()
endforeach ()

function(run)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if (NOT result STREQUAL "0")
        message(FATAL_ERROR "Runtime dependency fixture failed (${ARGV}):\n${output}${error}")
    endif ()
endfunction()

function(install_fixture destination expected_error)
    set(command "${CMAKE_COMMAND}")
    if (ARGN)
        list(PREPEND command "${CMAKE_COMMAND}" -E env ${ARGN})
    endif ()
    execute_process(COMMAND ${command} --install "${build}" --config Release
            --prefix "${destination}" --component RuntimeFixture
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if (expected_error)
        string(REGEX REPLACE "[ \t\r\n]+" " " diagnostic "${output}${error}")
        if (result STREQUAL "0" OR NOT diagnostic MATCHES "${expected_error}")
            message(FATAL_ERROR "Expected runtime deployment failure '${expected_error}':\n${output}${error}")
        endif ()
    elseif (NOT result STREQUAL "0")
        message(FATAL_ERROR "Runtime deployment failed:\n${output}${error}")
    endif ()
endfunction()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(root "${WORK_DIRECTORY}/huxerui-runtime-${nonce}")
set(source "${root}/source with spaces")
set(build "${root}/build with spaces")
file(MAKE_DIRECTORY "${source}/application" "${source}/extras" "${source}/cmake")
file(TO_CMAKE_PATH "${SOURCE_DIRECTORY}" SOURCE_DIRECTORY)
if (WIN32)
    file(WRITE "${source}/cmake/InstallRequiredSystemLibraries.cmake" [=[
if(MSVC_CXX_ARCHITECTURE_ID)
  string(TOLOWER "${MSVC_CXX_ARCHITECTURE_ID}" CMAKE_MSVC_ARCH)
else()
  set(CMAKE_MSVC_ARCH x86)
endif()
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
if(NOT DEFINED MSVC_REDIST_DIR)
  set(MSVC_REDIST_DIR MSVC_REDIST_DIR-NOTFOUND CACHE PATH "")
endif()
]=])
endif ()
file(WRITE "${source}/application/leaf.cpp" [=[
#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
extern "C" EXPORT int Leaf() { return 21; }
]=])
file(WRITE "${source}/application/middle.cpp" [=[
#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
extern "C" int Leaf();
extern "C" EXPORT int Middle() { return Leaf() * 2; }
]=])
file(WRITE "${source}/application/module.cpp" [=[
#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
extern "C" int Leaf();
extern "C" EXPORT int Plugin() { return Leaf(); }
]=])
file(WRITE "${source}/application/app.cpp" [=[
#include <string>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
extern "C" int Middle();
#if defined(_WIN32)
extern "C" int DelayLeaf();
#endif
int main(int argc, char** argv) {
  if (argc != 2 || Middle() != 42) return 1;
  const std::string path(argv[1]);
#if defined(_WIN32)
  if (DelayLeaf() != 21) return 3;
  HMODULE module = LoadLibraryA(path.c_str());
  auto function = module ? reinterpret_cast<int(*)()>(GetProcAddress(module, "Plugin")) : nullptr;
#else
  void* module = dlopen(path.c_str(), RTLD_NOW);
  auto function = module ? reinterpret_cast<int(*)()>(dlsym(module, "Plugin")) : nullptr;
#endif
  return function && function() == 21 ? 0 : 2;
}
]=])
set(project [=[
include("@SOURCE_DIRECTORY@/cmake/HuxerUIRuntimeDependencies.cmake")
add_library(leaf SHARED leaf.cpp)
add_library(middle SHARED middle.cpp)
target_link_libraries(middle PRIVATE leaf)
add_library(module_leaf SHARED leaf.cpp)
add_library(codec MODULE module.cpp)
target_link_libraries(codec PRIVATE module_leaf)
add_executable(app app.cpp)
target_link_libraries(app PRIVATE middle ${CMAKE_DL_LIBS})
foreach(target IN ITEMS leaf middle module_leaf codec)
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dependencies/${target}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dependencies/${target}")
endforeach()
if(UNIX AND NOT APPLE)
  set_target_properties(leaf module_leaf PROPERTIES VERSION 1.2 SOVERSION 1)
endif()
set(leaf_payload "$<TARGET_FILE_NAME:leaf>")
set(module_payload "$<TARGET_FILE_NAME:codec>")
if(WIN32)
  set_target_properties(leaf PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Windows/System32")
  add_library(delay_leaf SHARED leaf.cpp)
  target_compile_definitions(delay_leaf PRIVATE Leaf=DelayLeaf)
  target_link_libraries(app PRIVATE delay_leaf delayimp)
  target_link_options(app PRIVATE "/DELAYLOAD:$<TARGET_FILE_NAME:delay_leaf>")
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/$<CONFIG>/delay.cmake" CONTENT
      "set(delay_leaf [==[$<TARGET_FILE:delay_leaf>]==])\n")
  set(content .)
  set(binary "$<TARGET_FILE_NAME:app>")
  set(module_directory plugins/codecs)
  set(library_directory .)
  install(TARGETS app RUNTIME DESTINATION . COMPONENT RuntimeFixture)
elseif(APPLE)
  set_target_properties(app PROPERTIES MACOSX_BUNDLE TRUE)
  set_target_properties(leaf PROPERTIES FRAMEWORK TRUE FRAMEWORK_VERSION A
      MACOSX_FRAMEWORK_IDENTIFIER org.huxerui.runtime.leaf)
  set_target_properties(codec PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION bundle PREFIX "")
  set_target_properties(module_leaf PROPERTIES VERSION 1.2 SOVERSION 1)
  set(leaf_payload "leaf.framework/Versions/A/leaf")
  set(module_payload "codec.bundle/Contents/MacOS/codec")
  set(content "$<TARGET_FILE_NAME:app>.app/Contents")
  set(binary "MacOS/$<TARGET_FILE_NAME:app>")
  set(module_directory PlugIns/codecs)
  set(library_directory Frameworks)
  install(TARGETS app BUNDLE DESTINATION . COMPONENT RuntimeFixture)
else()
  set(content usr)
  set(binary "bin/$<TARGET_FILE_NAME:app>")
  set(module_directory lib/app/codecs)
  set(library_directory lib)
  install(TARGETS app RUNTIME DESTINATION usr/bin COMPONENT RuntimeFixture)
endif()
_huxerui_install_runtime_dependencies(app RuntimeFixture "${content}" "${binary}")
huxerui_add_runtime_dependencies(app TARGETS codec
    DESTINATION "${module_directory}")
huxerui_add_runtime_dependencies(app FILES "$<$<CONFIG:Debug>:missing-debug-runtime>"
    SEARCH_DIRECTORIES "$<$<CONFIG:Debug>:missing-debug-directory>")
install(FILES resource.class mz-resource.bin elf-resource.bin
    DESTINATION "${content}/data" COMPONENT RuntimeFixture)
set(manual_destination "${content}/${library_directory}" PARENT_SCOPE)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/$<CONFIG>/paths.cmake" CONTENT
  "set(content [==[${content}]==])\nset(binary [==[${binary}]==])\nset(module [==[${module_directory}/${module_payload}]==])\nset(leaf [==[$<TARGET_FILE:leaf>]==])\nset(module_leaf [==[$<TARGET_FILE:module_leaf>]==])\nset(library_directory [==[${library_directory}]==])\nset(leaf_payload [==[${leaf_payload}]==])\nset(leaf_name [==[$<TARGET_FILE_NAME:leaf>]==])\nset(module_leaf_name [==[$<TARGET_FILE_NAME:module_leaf>]==])\n")
]=])
string(CONFIGURE "${project}" project @ONLY)
file(WRITE "${source}/application/CMakeLists.txt" "${project}")
file(WRITE "${source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.20)
project(runtime_fixture LANGUAGES CXX)
add_subdirectory(application)
if(MANUAL_FILE)
  install(FILES "${MANUAL_FILE}" DESTINATION "${manual_destination}" COMPONENT RuntimeFixture)
endif()
if(EXTRA_FILE)
  huxerui_add_runtime_dependencies(app FILES "${EXTRA_FILE}")
endif()
if(EXTRA_IMPORTED)
  add_subdirectory(extras)
endif()
]=])
file(WRITE "${source}/extras/CMakeLists.txt" [=[
add_library(vendor SHARED IMPORTED)
set_target_properties(vendor PROPERTIES IMPORTED_LOCATION "${EXTRA_IMPORTED}")
huxerui_add_runtime_dependencies(app TARGETS vendor)
]=])
string(ASCII 202 254 186 190 class_magic)
string(ASCII 127 69 76 70 elf_magic)
file(WRITE "${source}/application/resource.class" "${class_magic}This packaged resource is not a native runtime library.")
file(WRITE "${source}/application/mz-resource.bin" "MZ")
file(WRITE "${source}/application/elf-resource.bin" "${elf_magic}data")
set(configure "${CMAKE_COMMAND}" -S "${source}" -B "${build}" -G "${HOST_GENERATOR}"
        "-DCMAKE_CXX_COMPILER=${HOST_CXX_COMPILER}" -DCMAKE_BUILD_TYPE=Release)
if (WIN32)
    file(TO_CMAKE_PATH "${source}/cmake" test_module_directory)
    list(APPEND configure "-DCMAKE_MODULE_PATH=${test_module_directory}")
endif ()
if (HOST_GENERATOR_PLATFORM)
    list(APPEND configure -A "${HOST_GENERATOR_PLATFORM}")
endif ()
if (HOST_GENERATOR_TOOLSET)
    list(APPEND configure -T "${HOST_GENERATOR_TOOLSET}")
endif ()
run(${configure})
run("${CMAKE_COMMAND}" --build "${build}" --config Release --parallel 2)
include("${build}/Release/paths.cmake")
if (APPLE)
    find_program(codesign codesign REQUIRED)
    # Linker-generated ad-hoc signatures must not hide deployment signing-order errors on Apple silicon.
    run("${codesign}" --remove-signature "${module_leaf}")
endif ()
install_fixture("${root}/staging" "")
foreach (name IN ITEMS "${leaf_payload}" "${module_leaf_name}")
    if (NOT EXISTS "${root}/staging/${content}/${library_directory}/${name}")
        message(FATAL_ERROR "Runtime deployment omitted transitive dependency ${name}")
    endif ()
endforeach ()
if (WIN32 AND NOT EXISTS "${root}/staging/vcruntime140.dll")
    message(FATAL_ERROR "Runtime deployment omitted the application-local VC++ runtime")
endif ()
foreach (resource IN ITEMS resource.class mz-resource.bin elf-resource.bin)
    file(SHA256 "${source}/application/${resource}" expected)
    file(SHA256 "${root}/staging/${content}/data/${resource}" actual)
    if (NOT actual STREQUAL expected)
        message(FATAL_ERROR "Runtime deployment changed the data resource ${resource}")
    endif ()
endforeach ()
file(RENAME "${root}/staging" "${root}/relocated package")
file(RENAME "${build}" "${build}-hidden")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH --unset=DYLD_LIBRARY_PATH
        "${root}/relocated package/${content}/${binary}"
        "${root}/relocated package/${content}/${module}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
file(RENAME "${build}-hidden" "${build}")
if (NOT result STREQUAL "0")
    message(FATAL_ERROR "Relocated runtime failed without build outputs (${result}): ${output}${error}")
endif ()

get_filename_component(module_leaf_directory "${module_leaf}" DIRECTORY)
file(RENAME "${module_leaf_directory}" "${module_leaf_directory}.hidden")
install_fixture("${root}/missing-transitive" "unresolved runtime dependencies")
file(RENAME "${module_leaf_directory}.hidden" "${module_leaf_directory}")
run(${configure} "-DEXTRA_FILE=${root}/missing-library")
install_fixture("${root}/missing-explicit" "runtime dependency is missing")

file(MAKE_DIRECTORY "${root}/conflict")
file(COPY "${module_leaf}" DESTINATION "${root}/conflict")
file(APPEND "${root}/conflict/${module_leaf_name}" "different payload")
run(${configure} "-DEXTRA_FILE=${root}/conflict/${module_leaf_name}")
install_fixture("${root}/conflicting" "conflicting runtime payload")

if (WIN32)
    get_filename_component(compiler_directory "${HOST_CXX_COMPILER}" DIRECTORY)
    set(foreign_compiler "${compiler_directory}/../x86/cl.exe")
    set(foreign_linker "${compiler_directory}/../x86/link.exe")
    file(MAKE_DIRECTORY "${root}/foreign")
    file(WRITE "${root}/foreign/foreign.cpp" "extern \"C\" __declspec(dllexport) int Foreign() { return 1; }\n")
    run("${foreign_compiler}" /nologo /c /O2 /GS- "/Fo${root}/foreign/foreign.obj" "${root}/foreign/foreign.cpp")
    run("${foreign_linker}" /NOLOGO /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X86
            "/OUT:${root}/foreign/${leaf_name}" "${root}/foreign/foreign.obj")
elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    find_program(objcopy objcopy REQUIRED)
    file(MAKE_DIRECTORY "${root}/foreign")
    run("${objcopy}" --alt-machine-code=3 "${leaf}" "${root}/foreign/${leaf_name}")
endif ()
if (EXISTS "${root}/foreign/${leaf_name}")
    run(${configure} "-DEXTRA_FILE=${root}/foreign/${leaf_name}")
    install_fixture("${root}/wrong-architecture" "runtime architecture mismatch")
endif ()

file(RELATIVE_PATH relative_module "${source}" "${module_leaf}")
run(${configure} "-DEXTRA_FILE=$<$<CONFIG:Release>:${relative_module}>")
install_fixture("${root}/prebuilt-file" "")
run(${configure} -DEXTRA_FILE= "-DEXTRA_IMPORTED=${module_leaf}")
install_fixture("${root}/imported-subdirectory" "")
run(${configure} -DEXTRA_IMPORTED=)
install_fixture("${root}/repeat-fresh-staging" "")

set(system_file)
if (WIN32)
    file(TO_CMAKE_PATH "$ENV{SystemRoot}/System32/kernel32.dll" system_file)
elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    execute_process(COMMAND "${HOST_CXX_COMPILER}" -print-file-name=libc.so.6
            OUTPUT_VARIABLE system_file OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
endif ()
if (system_file)
    run(${configure} "-DEXTRA_FILE=${system_file}")
    install_fixture("${root}/explicit-system" "cannot explicitly deploy a system runtime")
    run(${configure} -DEXTRA_FILE= "-DMANUAL_FILE=${system_file}")
    install_fixture("${root}/manual-system" "packaged files must not contain a system runtime")
    run(${configure} -DMANUAL_FILE=)
endif ()

if (WIN32)
    # A simulated SystemRoot cannot initialize PowerShell; this case needs no VC++ redistributables.
    run(${configure} -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded)
    run("${CMAKE_COMMAND}" --build "${build}" --config Release --parallel 2)
    install_fixture("${root}/vendor-system-directory" "" "SystemRoot=${build}/Windows")
    if (NOT EXISTS "${root}/vendor-system-directory/${leaf_name}")
        message(FATAL_ERROR "Runtime deployment omitted a third-party DLL located in System32")
    endif ()
    run(${configure} -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL)
    run("${CMAKE_COMMAND}" --build "${build}" --config Release --parallel 2)
    include("${build}/Release/delay.cmake")
    file(RENAME "${delay_leaf}" "${delay_leaf}.hidden")
    install_fixture("${root}/missing-delay-load" "unresolved runtime dependencies")
    file(RENAME "${delay_leaf}.hidden" "${delay_leaf}")
    file(MAKE_DIRECTORY "${root}/empty-redist")
    run(${configure} "-DMSVC_REDIST_DIR=${root}/empty-redist")
    install_fixture("${root}/missing-redist" "VC\\+\\+ Release redistributable.*missing")
    run(${configure} -UMSVC_REDIST_DIR -DHUXERUI_WINDOWS_7_COMPAT=ON)
    run("${CMAKE_COMMAND}" --build "${build}" --config Release --parallel 2)
    install_fixture("${root}/windows-7" "Windows 7 redistribution is not configured")
    run(${configure} -DHUXERUI_WINDOWS_7_COMPAT=OFF)
    run("${CMAKE_COMMAND}" -S "${source}" -B "${build}" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebugDLL)
    run("${CMAKE_COMMAND}" --build "${build}" --config Release --parallel 2)
    install_fixture("${root}/debug-crt" "Debug VC\\+\\+ runtime")
endif ()

file(REAL_PATH "${WORK_DIRECTORY}" work_root)
file(REAL_PATH "${root}" cleanup_root)
file(RELATIVE_PATH cleanup_relative "${work_root}" "${cleanup_root}")
if (NOT cleanup_relative MATCHES "^huxerui-runtime-[0-9a-f]+$")
    message(FATAL_ERROR "Refusing to clean a runtime fixture outside its work directory: ${cleanup_root}")
endif ()
file(REMOVE_RECURSE "${cleanup_root}")
message(STATUS "HuxerUI runtime deployment fixtures passed")
