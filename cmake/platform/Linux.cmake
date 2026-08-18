function(huxerui_platform_configure)
    find_package(PkgConfig REQUIRED)

    # Platform APIs stay system dynamic libraries. The graphics/text stack is
    # fetched from pinned git tags through FetchContent and built as static
    # libraries; cairo, fontconfig, and pixman keep their upstream meson build
    # system and are driven through ExternalProject.
    pkg_check_modules(HUXERUI_X11 REQUIRED IMPORTED_TARGET x11)
    pkg_check_modules(HUXERUI_XEXT REQUIRED IMPORTED_TARGET xext)
    pkg_check_modules(HUXERUI_XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
    pkg_check_modules(HUXERUI_XRANDR REQUIRED IMPORTED_TARGET xrandr)
    pkg_check_modules(HUXERUI_EGL REQUIRED IMPORTED_TARGET egl)
    pkg_check_modules(HUXERUI_GLES2 REQUIRED IMPORTED_TARGET glesv2)
    pkg_check_modules(HUXERUI_FCITX5_GCLIENT QUIET IMPORTED_TARGET Fcitx5GClient)

    if(TARGET PkgConfig::HUXERUI_FCITX5_GCLIENT)
        list(APPEND HUXERUI_PLATFORM_COMPILE_DEFINITIONS HUXERUI_HAS_FCITX5_GCLIENT=1)
    endif()

    include(FetchContent)
    include(ExternalProject)

    # Official CMake projects are added with FetchContent_MakeAvailable.
    FetchContent_Declare(zlib GIT_REPOSITORY https://github.com/madler/zlib.git GIT_TAG v1.3.2 GIT_SHALLOW TRUE)
    # libexpat keeps its CMakeLists.txt in an expat/ subdirectory.
    FetchContent_Declare(expat
            GIT_REPOSITORY https://github.com/libexpat/libexpat.git
            GIT_TAG R_2_8_2
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR expat
    )
    FetchContent_Declare(libpng GIT_REPOSITORY https://github.com/pnggroup/libpng.git GIT_TAG v1.6.58 GIT_SHALLOW TRUE)
    FetchContent_Declare(libjpeg-turbo GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo.git GIT_TAG 3.2.0 GIT_SHALLOW TRUE)
    FetchContent_Declare(freetype GIT_REPOSITORY https://github.com/freetype/freetype.git GIT_TAG VER-2-14-3 GIT_SHALLOW TRUE)
    FetchContent_Declare(harfbuzz GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git GIT_TAG 14.3.0 GIT_SHALLOW TRUE)

    # Meson-only projects: FetchContent downloads the pinned sources and
    # ExternalProject drives the upstream meson build.
    FetchContent_Declare(pixman GIT_REPOSITORY https://gitlab.freedesktop.org/pixman/pixman.git GIT_TAG pixman-0.46.4 GIT_SHALLOW TRUE)
    FetchContent_Declare(fontconfig GIT_REPOSITORY https://gitlab.freedesktop.org/fontconfig/fontconfig.git GIT_TAG a4e25ec391d417e4bca052fbfa5cd7ce5f7fd39e GIT_SHALLOW TRUE)
    FetchContent_Declare(cairo GIT_REPOSITORY https://gitlab.freedesktop.org/cairo/cairo.git GIT_TAG 1.18.4 GIT_SHALLOW TRUE)

    # Every library installs into one staging prefix; the meson projects
    # resolve their dependencies through the staged pkg-config files.
    set(HUXERUI_LINUX_STAGE "${CMAKE_CURRENT_BINARY_DIR}/linux-stage")

    # Native tools required by the fetched projects.
    find_program(HUXERUI_MESON meson)
    find_program(HUXERUI_NINJA ninja)
    if(NOT HUXERUI_MESON OR NOT HUXERUI_NINJA)
        message(FATAL_ERROR
                "HuxerUI Linux builds fetch cairo, fontconfig, and pixman from git and build them with meson; "
                "install meson and ninja")
    endif()
    find_program(HUXERUI_GPERF gperf)
    if(NOT HUXERUI_GPERF)
        message(FATAL_ERROR "HuxerUI Linux builds need gperf to build fontconfig from source; install gperf")
    endif()

    # The fetched CMake projects build as static, position-independent
    # Release libraries and install into the staging prefix with that prefix
    # baked into their generated pkg-config files. CMAKE_BUILD_TYPE is a
    # directory variable here so the fetched projects stay Release while the
    # host project keeps its own configuration.
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_BUILD_TYPE Release)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_INSTALL_PREFIX "${HUXERUI_LINUX_STAGE}")
    set(CMAKE_INSTALL_LIBDIR lib)

    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(EXPAT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(EXPAT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(EXPAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(EXPAT_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(PNG_SHARED OFF CACHE BOOL "" FORCE)
    set(PNG_STATIC ON CACHE BOOL "" FORCE)
    set(PNG_TESTS OFF CACHE BOOL "" FORCE)
    set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
    set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
    set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
    set(WITH_TURBOJPEG OFF CACHE BOOL "" FORCE)
    set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
    set(WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(WITH_DOCS OFF CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
    set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_RASTER OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_VECTOR OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_GPU OFF CACHE BOOL "" FORCE)

    # libpng resolves zlib through FindZLIB; hand the module the fetched build.
    FetchContent_MakeAvailable(zlib)
    set(ZLIB_LIBRARY "${zlib_BINARY_DIR}/libz.a" CACHE FILEPATH "" FORCE)
    set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE PATH "" FORCE)
    FetchContent_MakeAvailable(expat libpng freetype harfbuzz)
    # libpng appends a Debug postfix to its archive name. The fetched
    # libraries build as Release, but the staging target records their
    # outputs through the parent build type, so the postfix would leave a
    # dependency on a Debug-named archive that is never produced.
    set_target_properties(png_static PROPERTIES DEBUG_POSTFIX "")
    # libjpeg-turbo refuses add_subdirectory integration and must be driven
    # through ExternalProject; it installs into the staging prefix itself.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(HUXERUI_LIBJPEG_SIMD OFF)
    else()
        set(HUXERUI_LIBJPEG_SIMD ON)
    endif()
    set(HUXERUI_LIBJPEG_CMAKE_ARGS
            -S <SOURCE_DIR> -B <BINARY_DIR>
            -DCMAKE_BUILD_TYPE=Release
            -DENABLE_SHARED=OFF -DENABLE_STATIC=ON
            -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF -DWITH_DOCS=OFF
            -DWITH_SIMD=${HUXERUI_LIBJPEG_SIMD}
            -DCMAKE_INSTALL_PREFIX=${HUXERUI_LINUX_STAGE} -DCMAKE_INSTALL_LIBDIR=lib
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    )
    if(CMAKE_CROSSCOMPILING)
        list(APPEND HUXERUI_LIBJPEG_CMAKE_ARGS
                -DCMAKE_SYSTEM_NAME=Linux
                -DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                -DCMAKE_AR=${CMAKE_AR}
                -DCMAKE_RANLIB=${CMAKE_RANLIB}
        )
    endif()

    # Download libjpeg-turbo into the build tree so its source directory
    # exists for the ExternalProject that drives its configure/install.
    FetchContent_GetProperties(libjpeg-turbo)
    if(NOT libjpeg-turbo_POPULATED)
        FetchContent_Populate(libjpeg-turbo)
    endif()

    ExternalProject_Add(huxerui_libjpeg
            SOURCE_DIR "${libjpeg-turbo_SOURCE_DIR}"
            DOWNLOAD_COMMAND ""
            INSTALL_DIR "${HUXERUI_LINUX_STAGE}"
            CONFIGURE_COMMAND ${CMAKE_COMMAND} ${HUXERUI_LIBJPEG_CMAKE_ARGS}
            BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release
            INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config Release
            BUILD_BYPRODUCTS "${HUXERUI_LINUX_STAGE}/lib/libjpeg.a"
    )

    # Stage the CMake-built libraries so the meson projects find headers and
    # pkg-config files under one prefix.
    add_custom_target(huxerui_linux_stage_cmake_deps
            COMMAND ${CMAKE_COMMAND} --install "${zlib_BINARY_DIR}"
            COMMAND ${CMAKE_COMMAND} --install "${expat_BINARY_DIR}"
            COMMAND ${CMAKE_COMMAND} --install "${libpng_BINARY_DIR}"
            COMMAND ${CMAKE_COMMAND} --install "${freetype_BINARY_DIR}"
            COMMAND ${CMAKE_COMMAND} --install "${harfbuzz_BINARY_DIR}"
            BYPRODUCTS
                    "${HUXERUI_LINUX_STAGE}/lib/libz.a"
                    "${HUXERUI_LINUX_STAGE}/lib/libexpat.a"
                    "${HUXERUI_LINUX_STAGE}/lib/libpng16.a"
                    "${HUXERUI_LINUX_STAGE}/lib/libfreetype.a"
                    "${HUXERUI_LINUX_STAGE}/lib/libharfbuzz.a"
            DEPENDS zlibstatic expat png_static freetype harfbuzz
    )

    set(HUXERUI_MESON_CROSS_ARGS)
    if(CMAKE_CROSSCOMPILING)
        set(HUXERUI_MESON_CROSS_FILE "${CMAKE_CURRENT_BINARY_DIR}/meson-cross.ini")
        file(WRITE "${HUXERUI_MESON_CROSS_FILE}"
                "[binaries]\n"
                "c = '${CMAKE_C_COMPILER}'\n"
                "cpp = '${CMAKE_CXX_COMPILER}'\n"
                "ar = '${CMAKE_AR}'\n"
                "strip = '${CMAKE_STRIP}'\n"
                "[host_machine]\n"
                "system = 'linux'\n"
                "cpu_family = '${CMAKE_SYSTEM_PROCESSOR}'\n"
                "cpu = '${CMAKE_SYSTEM_PROCESSOR}'\n"
                "endian = 'little'\n"
        )
        list(APPEND HUXERUI_MESON_CROSS_ARGS "--cross-file=${HUXERUI_MESON_CROSS_FILE}")
    endif()

    # Meson resolves staged dependencies through pkg-config only; system
    # packages must not leak into the vendored build. Populate the meson
    # sources so their _SOURCE_DIR variables exist for ExternalProject.
    set(HUXERUI_MESON_PKGCONFIG_ENV
            ${CMAKE_COMMAND} -E env
            "PKG_CONFIG_LIBDIR=${HUXERUI_LINUX_STAGE}/lib/pkgconfig"
            "PKG_CONFIG_PATH="
    )
    FetchContent_GetProperties(pixman)
    if(NOT pixman_POPULATED)
        FetchContent_Populate(pixman)
    endif()
    FetchContent_GetProperties(fontconfig)
    if(NOT fontconfig_POPULATED)
        FetchContent_Populate(fontconfig)
    endif()
    FetchContent_GetProperties(cairo)
    if(NOT cairo_POPULATED)
        FetchContent_Populate(cairo)
    endif()

    ExternalProject_Add(huxerui_pixman
            SOURCE_DIR "${pixman_SOURCE_DIR}"
            DOWNLOAD_COMMAND ""
            INSTALL_DIR "${HUXERUI_LINUX_STAGE}"
            CONFIGURE_COMMAND ${HUXERUI_MESON_PKGCONFIG_ENV} ${HUXERUI_MESON} setup
                    --buildtype=release --default-library=static --wrap-mode=nodownload
                    -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dopenmp=disabled
                    -Dprefix=${HUXERUI_LINUX_STAGE} -Dlibdir=lib -Db_staticpic=true
                    ${HUXERUI_MESON_CROSS_ARGS}
                    <BINARY_DIR> <SOURCE_DIR>
            BUILD_COMMAND ${HUXERUI_MESON_PKGCONFIG_ENV} ${HUXERUI_MESON} compile -C <BINARY_DIR>
            INSTALL_COMMAND ${HUXERUI_MESON_PKGCONFIG_ENV} ${HUXERUI_MESON} install -C <BINARY_DIR>
            BUILD_BYPRODUCTS "${HUXERUI_LINUX_STAGE}/lib/libpixman-1.a"
            DEPENDS huxerui_linux_stage_cmake_deps
    )

    ExternalProject_Add(huxerui_fontconfig
            SOURCE_DIR "${fontconfig_SOURCE_DIR}"
            DOWNLOAD_COMMAND ""
            INSTALL_DIR "${HUXERUI_LINUX_STAGE}"
            CONFIGURE_COMMAND ${HUXERUI_MESON_PKGCONFIG_ENV} ${HUXERUI_MESON} setup
                    --buildtype=release --default-library=static --wrap-mode=nodownload
                    -Ddoc=disabled -Dtools=disabled -Dtests=disabled
                    -Dnls=disabled -Dcache-build=disabled -Dxml-backend=expat
                    -Dprefix=${HUXERUI_LINUX_STAGE} -Dlibdir=lib -Db_staticpic=true
                    ${HUXERUI_MESON_CROSS_ARGS}
                    <BINARY_DIR> <SOURCE_DIR>
            BUILD_COMMAND ${HUXERUI_MESON_PKGCONFIG_ENV} ${HUXERUI_MESON} compile -C <BINARY_DIR>
            INSTALL_COMMAND ${HUXERUI_MESON_PKGCONFIG_ENV} ${HUXERUI_MESON} install -C <BINARY_DIR>
            BUILD_BYPRODUCTS "${HUXERUI_LINUX_STAGE}/lib/libfontconfig.a"
            DEPENDS huxerui_linux_stage_cmake_deps
    )

    ExternalProject_Add(huxerui_cairo
            SOURCE_DIR "${cairo_SOURCE_DIR}"
            DOWNLOAD_COMMAND ""
            INSTALL_DIR "${HUXERUI_LINUX_STAGE}"
            CONFIGURE_COMMAND ${HUXERUI_MESON_PKGCONFIG_ENV} ${HUXERUI_MESON} setup
                    --buildtype=release --default-library=static --wrap-mode=nodownload
                    -Dtests=disabled -Dpng=disabled -Dzlib=disabled -Dglib=disabled
                    -Dxlib=disabled -Dxcb=disabled -Dquartz=disabled
                    -Dfontconfig=enabled -Dfreetype=enabled -Ddwrite=disabled
                    -Dprefix=${HUXERUI_LINUX_STAGE} -Dlibdir=lib -Db_staticpic=true
                    ${HUXERUI_MESON_CROSS_ARGS}
                    <BINARY_DIR> <SOURCE_DIR>
            BUILD_COMMAND ${HUXERUI_MESON_PKGCONFIG_ENV} ${HUXERUI_MESON} compile -C <BINARY_DIR>
            INSTALL_COMMAND ${HUXERUI_MESON_PKGCONFIG_ENV} ${HUXERUI_MESON} install -C <BINARY_DIR>
            BUILD_BYPRODUCTS "${HUXERUI_LINUX_STAGE}/lib/libcairo.a"
            DEPENDS huxerui_pixman huxerui_fontconfig
    )

    add_custom_target(huxerui_linux_deps
            DEPENDS huxerui_linux_stage_cmake_deps huxerui_libjpeg huxerui_pixman huxerui_fontconfig huxerui_cairo
    )
    # The core object library is created after platform configuration; defer
    # the dependency until the directory has finished configuring.
    cmake_language(DEFER CALL add_dependencies huxerui_core_objects huxerui_linux_deps)

    set(HUXERUI_PLATFORM_SOURCE_FILES
            "${HUXERUI_PROJECT_DIR}/platform/linux/linux_adapter.cpp"
            "${HUXERUI_PROJECT_DIR}/platform/linux/linux_external_texture.cpp"
            "${HUXERUI_PROJECT_DIR}/platform/linux/linux_renderer.cpp"
            "${HUXERUI_PROJECT_DIR}/platform/linux/linux_text_input.cpp"
            "${HUXERUI_PROJECT_DIR}/platform/linux/linux_ui_dispatcher.cpp"
            PARENT_SCOPE
    )
    set(HUXERUI_PLATFORM_INCLUDE_DIRECTORIES
            ${HUXERUI_X11_INCLUDE_DIRS}
            ${HUXERUI_XKBCOMMON_INCLUDE_DIRS}
            ${HUXERUI_XRANDR_INCLUDE_DIRS}
            ${HUXERUI_EGL_INCLUDE_DIRS}
            ${HUXERUI_GLES2_INCLUDE_DIRS}
            ${HUXERUI_FCITX5_GCLIENT_INCLUDE_DIRS}
            "${HUXERUI_LINUX_STAGE}/include"
            "${HUXERUI_LINUX_STAGE}/include/freetype2"
            PARENT_SCOPE
    )

    set(HUXERUI_PLATFORM_COMPILE_DEFINITIONS
            ${HUXERUI_PLATFORM_COMPILE_DEFINITIONS}
            PARENT_SCOPE
    )

    # Static-link order follows the dependency chain: cairo -> pixman,
    # fontconfig -> freetype -> zlib, harfbuzz -> freetype, libpng -> zlib.
    # The archives do not exist until build time, so paths are assembled
    # directly; huxerui_linux_deps above orders their construction.
    foreach(lib IN ITEMS cairo fontconfig harfbuzz freetype png16 jpeg expat z pixman-1)
        list(APPEND HUXERUI_PLATFORM_LINK_LIBRARIES
                "${HUXERUI_LINUX_STAGE}/lib/lib${lib}.a"
        )
    endforeach()
    list(APPEND HUXERUI_PLATFORM_LINK_LIBRARIES m pthread dl)
    if(TARGET PkgConfig::HUXERUI_FCITX5_GCLIENT)
        list(APPEND HUXERUI_PLATFORM_LINK_LIBRARIES PkgConfig::HUXERUI_FCITX5_GCLIENT)
    endif()

    set(HUXERUI_PLATFORM_LINK_LIBRARIES
            ${HUXERUI_PLATFORM_LINK_LIBRARIES}
            PkgConfig::HUXERUI_X11
            PkgConfig::HUXERUI_XEXT
            PkgConfig::HUXERUI_XKBCOMMON
            PkgConfig::HUXERUI_XRANDR
            PkgConfig::HUXERUI_EGL
            PkgConfig::HUXERUI_GLES2
            PARENT_SCOPE
    )
endfunction()
