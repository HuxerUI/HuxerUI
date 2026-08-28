#!/bin/sh

set -eu

usage() {
  cat <<'EOF'
Usage: build_tools.sh [-p|--platform <platform>] [-a|--architecture <architecture>]
                      [-t|--toolchain <path>] [-n|--android-ndk <path>]
                      [-b|--build-dir <path>] [-o|--output-dir <path>]

Builds and installs the HuxerUI host tools for Windows, macOS, Linux, or Android.
EOF
}

fail() {
  printf 'HuxerUI host tools: %s\n' "$1" >&2
  exit 1
}

source_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
platform=""
architecture=""
toolchain_file=""
android_ndk=""
build_directory=""
output_directory=""

while [ "$#" -gt 0 ]; do
  case "$1" in
  -p | --platform)
    [ "$#" -ge 2 ] || fail "--platform requires a value"
    platform=$2
    shift 2
    ;;
  -a | --architecture)
    [ "$#" -ge 2 ] || fail "--architecture requires a value"
    architecture=$2
    shift 2
    ;;
  -t | --toolchain)
    [ "$#" -ge 2 ] || fail "--toolchain requires a path"
    toolchain_file=$2
    shift 2
    ;;
  -n | --android-ndk)
    [ "$#" -ge 2 ] || fail "--android-ndk requires a path"
    android_ndk=$2
    shift 2
    ;;
  -b | --build-dir)
    [ "$#" -ge 2 ] || fail "--build-dir requires a path"
    build_directory=$2
    shift 2
    ;;
  -o | --output-dir)
    [ "$#" -ge 2 ] || fail "--output-dir requires a path"
    output_directory=$2
    shift 2
    ;;
  -h | --help)
    usage
    exit 0
    ;;
  *)
    fail "unknown argument: $1"
    ;;
  esac
done

case "$(uname -s)" in
Darwin) native_platform="macos" ;;
Linux)
  if [ -n "${TERMUX_VERSION:-}" ] || { [ -n "${ANDROID_ROOT:-}" ] && [ -x /system/bin/getprop ]; }; then
    native_platform="android"
  else
    native_platform="linux"
  fi
  ;;
MINGW* | MSYS* | CYGWIN*) native_platform="windows" ;;
*) fail "unsupported host platform: $(uname -s)" ;;
esac

if [ -z "$platform" ]; then
  platform=$native_platform
fi

case "$(uname -m)" in
amd64 | x64 | x86_64) native_architecture="x86_64" ;;
aarch64 | arm64)
  case "$native_platform" in
  linux) native_architecture="aarch64" ;;
  android) native_architecture="arm64-v8a" ;;
  *) native_architecture="arm64" ;;
  esac
  ;;
*) fail "unsupported host architecture: $(uname -m)" ;;
esac

case "$platform" in
windows | macos | linux | android) ;;
*) fail "unsupported platform: $platform" ;;
esac

if [ -z "$architecture" ]; then
  case "$platform" in
  android) architecture="arm64-v8a" ;;
  windows) architecture="x86_64" ;;
  linux)
    if [ "$native_architecture" = "x86_64" ]; then
      architecture="x86_64"
    else
      architecture="aarch64"
    fi
    ;;
  macos)
    if [ "$native_architecture" = "x86_64" ]; then
      architecture="x86_64"
    else
      architecture="arm64"
    fi
    ;;
  esac
else
  case "$architecture" in
  amd64 | x64 | x86_64) architecture="x86_64" ;;
  aarch64 | arm64)
    case "$platform" in
    linux) architecture="aarch64" ;;
    android) architecture="arm64-v8a" ;;
    *) architecture="arm64" ;;
    esac
    ;;
  esac
fi

case "$platform/$architecture" in
windows/x86_64 | macos/arm64 | macos/x86_64 | linux/aarch64 | linux/x86_64 | android/arm64-v8a) ;;
*) fail "unsupported host tools package: $platform/$architecture" ;;
esac

if [ "$platform" != "$native_platform" ] && [ "$platform" != "android" ] && [ -z "$toolchain_file" ]; then
  fail "--toolchain is required to build $platform tools on $native_platform"
fi
if [ "$platform" = "linux" ] && [ "$architecture" != "$native_architecture" ] && [ -z "$toolchain_file" ]; then
  fail "--toolchain is required to build Linux $architecture tools on $native_architecture"
fi

if [ "$platform" = "android" ] && [ -z "$toolchain_file" ]; then
  if [ -z "$android_ndk" ]; then
    android_ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
  fi
  if [ -z "$android_ndk" ] && [ -n "${ANDROID_HOME:-}" ]; then
    ndk_version=$(sed -n 's/^huxeruiNdkVersion=//p' "$source_directory/platform/android/gradle.properties")
    [ -n "$ndk_version" ] || fail "cannot resolve the Android NDK version"
    android_ndk="$ANDROID_HOME/ndk/$ndk_version"
  fi
  [ -n "$android_ndk" ] || fail "set an Android NDK environment variable or pass --android-ndk"
  toolchain_file="$android_ndk/build/cmake/android.toolchain.cmake"
fi

if [ -n "$toolchain_file" ]; then
  [ -f "$toolchain_file" ] || fail "toolchain file is missing: $toolchain_file"
fi
command -v cmake >/dev/null 2>&1 || fail "cmake is required on PATH"
if [ -n "${CMAKE_GENERATOR:-}" ]; then
  cmake_generator=$CMAKE_GENERATOR
elif command -v ninja >/dev/null 2>&1; then
  cmake_generator=Ninja
elif command -v make >/dev/null 2>&1; then
  cmake_generator="Unix Makefiles"
else
  fail "no supported CMake generator is available; set CMAKE_GENERATOR or install ninja or make"
fi

if [ -z "$build_directory" ]; then
  build_directory="$source_directory/build/tools/$platform/$architecture"
fi
if [ -z "$output_directory" ]; then
  output_directory="$source_directory/tools/prebuilt/$platform/$architecture"
fi

absolute_directory() {
  requested_path=$1
  case "$requested_path" in
  /*) ;;
  *) requested_path="$(pwd -P)/$requested_path" ;;
  esac
  mkdir -p "$requested_path"
  CDPATH= cd -- "$requested_path" && pwd -P
}

build_directory=$(absolute_directory "$build_directory")
output_directory=$(absolute_directory "$output_directory")

build_tool() {
  source_path=$1
  tool_build_directory=$2
  target_name=$3
  set -- cmake -S "$source_path" -B "$tool_build_directory" -G "$cmake_generator" -DCMAKE_BUILD_TYPE=Release
  if [ -n "$toolchain_file" ]; then
    set -- "$@" "-DCMAKE_TOOLCHAIN_FILE=$toolchain_file"
  fi
  if [ "$platform" = "macos" ]; then
    set -- "$@" "-DCMAKE_OSX_ARCHITECTURES=$architecture"
  elif [ "$platform" = "android" ]; then
    set -- "$@" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
  fi
  "$@"
  cmake --build "$tool_build_directory" --target "$target_name"
  cmake --install "$tool_build_directory" --strip --prefix "$output_directory"
}

build_tool \
  "$source_directory/tools/codegen" \
  "$build_directory/hcg" \
  huxerui_codegen
build_tool \
  "$source_directory/tools/resource_compiler" \
  "$build_directory/hrc" \
  huxerui_resource_compiler

printf 'HuxerUI %s %s host tools written to %s\n' "$platform" "$architecture" "$output_directory"
