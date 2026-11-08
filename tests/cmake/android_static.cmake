file(STRINGS "${SOURCE_DIRECTORY}/platform/android/gradle.properties" ndk_property REGEX "^huxeruiNdkVersion=")
string(REPLACE "huxeruiNdkVersion=" "" ndk_version "${ndk_property}")
set(android_sdk "$ENV{ANDROID_HOME}")
if (NOT android_sdk)
    set(android_sdk "$ENV{ANDROID_SDK_ROOT}")
endif ()
file(TO_CMAKE_PATH "${android_sdk}/ndk/${ndk_version}" ndk)
if (NOT EXISTS "${ndk}/build/cmake/android.toolchain.cmake")
    message(STATUS "HuxerUI Android NDK is unavailable")
    return()
endif ()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(test_root "${WORK_DIRECTORY}/android-static-${nonce}")
file(MAKE_DIRECTORY "${test_root}/source")
file(WRITE "${test_root}/source/jni.cpp"
        "#include <jni.h>\nextern \"C\" JNIEXPORT jint JNICALL Java_org_huxerui_test_Probe_read(JNIEnv*, jclass) { return 42; }\n")
file(WRITE "${test_root}/source/unused.cpp" "extern \"C\" int UnusedArchiveMember() { return 7; }\n")
file(WRITE "${test_root}/source/app.cpp" "extern \"C\" int PrivateApplicationFunction() { return 1; }\n")
file(WRITE "${test_root}/source/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.20)\nproject(AndroidStaticConsumer LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 20)\n"
        "include(\"${SOURCE_DIRECTORY}/cmake/HuxerUIOptimization.cmake\")\n"
        "add_library(core STATIC jni.cpp unused.cpp)\n"
        "_huxerui_configure_optimization(core)\n_huxerui_configure_android_static_target(core)\n"
        "add_library(consumer SHARED app.cpp)\n_huxerui_configure_optimization(consumer)\n"
        "target_link_libraries(consumer PRIVATE core)\n"
        "file(GENERATE OUTPUT \"\${CMAKE_BINARY_DIR}/artifact.txt\" CONTENT \"$<TARGET_FILE:consumer>\")\n")
file(GLOB toolchains "${ndk}/toolchains/llvm/prebuilt/*")
list(GET toolchains 0 toolchain)
find_program(nm NAMES llvm-nm PATHS "${toolchain}/bin" NO_DEFAULT_PATH REQUIRED)
find_program(readelf NAMES llvm-readelf PATHS "${toolchain}/bin" NO_DEFAULT_PATH REQUIRED)
foreach (abi IN ITEMS arm64-v8a x86_64)
    foreach (lto IN ITEMS OFF ON)
        set(build "${test_root}/${abi}-${lto}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -S "${test_root}/source" -B "${build}" -G Ninja
                "-DCMAKE_MAKE_PROGRAM=${HOST_NINJA}"
                "-DCMAKE_TOOLCHAIN_FILE=${ndk}/build/cmake/android.toolchain.cmake"
                -DCMAKE_BUILD_TYPE=Release "-DANDROID_ABI=${abi}" -DANDROID_PLATFORM=android-23
                -DANDROID_STL=c++_static "-DHUXERUI_ENABLE_LTO=${lto}"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
        if (NOT result EQUAL 0)
            message(FATAL_ERROR "Android static consumer configure failed: ${output}${error}")
        endif ()
        execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --parallel 2
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
        if (NOT result EQUAL 0)
            message(FATAL_ERROR "Android static consumer build failed: ${output}${error}")
        endif ()
        file(READ "${build}/artifact.txt" artifact)
        execute_process(COMMAND "${nm}" -D --defined-only "${artifact}"
                RESULT_VARIABLE result OUTPUT_VARIABLE symbols ERROR_VARIABLE error)
        if (NOT result EQUAL 0 OR NOT symbols MATCHES "Java_org_huxerui_test_Probe_read"
                OR symbols MATCHES "UnusedArchiveMember|PrivateApplicationFunction")
            message(FATAL_ERROR "Android static consumer has incorrect JNI exports: ${symbols}${error}")
        endif ()
        execute_process(COMMAND "${readelf}" -d "${artifact}"
                RESULT_VARIABLE result OUTPUT_VARIABLE dynamic ERROR_VARIABLE error)
        if (NOT result EQUAL 0 OR dynamic MATCHES "libhuxerui\\.so|libc\\+\\+_shared\\.so")
            message(FATAL_ERROR "Android static consumer has an unexpected dependency: ${dynamic}${error}")
        endif ()
    endforeach ()
endforeach ()
