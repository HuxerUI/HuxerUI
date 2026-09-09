#!/bin/sh

set -eu

usage() {
  cat <<'EOF'
Usage: package_sdk.sh [--build-dir <path>] [--output-dir <path>]
                      [--configuration Debug|Release] [--jobs <count>]

Builds Android and Web target artifacts, plus iOS on macOS, then packages a
complete SDK for the current host. Android SDK/NDK, Java, and Emscripten 4.0.19
must be installed. macOS packaging also requires Xcode.
EOF
}

fail() {
  printf 'HuxerUI SDK packaging: %s\n' "$1" >&2
  exit 1
}

source_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
build_directory="$source_directory/build/sdk"
output_directory=
configuration=Release
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')

while [ "$#" -gt 0 ]; do
  case "$1" in
  --build-dir)
    [ "$#" -ge 2 ] || fail "--build-dir requires a path"
    build_directory=$2
    shift 2
    ;;
  --output-dir)
    [ "$#" -ge 2 ] || fail "--output-dir requires a path"
    output_directory=$2
    shift 2
    ;;
  --configuration)
    [ "$#" -ge 2 ] || fail "--configuration requires Debug or Release"
    configuration=$2
    shift 2
    ;;
  --jobs)
    [ "$#" -ge 2 ] || fail "--jobs requires a positive integer"
    jobs=$2
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

if [ -z "$output_directory" ]; then
  output_directory="$build_directory/packages"
fi

case "$configuration" in
Debug | Release) ;;
*) fail "--configuration requires Debug or Release" ;;
esac
case "$jobs" in
'' | *[!0-9]* | 0) fail "--jobs requires a positive integer" ;;
esac
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) fail "use scripts/package_sdk.ps1 on Windows" ;;
esac
host_system=$(uname -s)

for command_name in cmake cpack ninja java emcmake emcc; do
  command -v "$command_name" >/dev/null 2>&1 || fail "'$command_name' is required on PATH"
done

absolute_directory() {
  requested_path=$1
  case "$requested_path" in
  /*) ;;
  *) requested_path="$(pwd -P)/$requested_path" ;;
  esac
  mkdir -p -- "$requested_path"
  CDPATH= cd -- "$requested_path" && pwd -P
}

build_directory=$(absolute_directory "$build_directory")
output_directory=$(absolute_directory "$output_directory")
platform_artifact_root="$build_directory/platform-artifacts"
android_build_directory="$build_directory/android"
web_build_directory="$build_directory/web"
host_build_directory="$build_directory/host"
ios_build_directory="$build_directory/ios"

reset_owned_directory() {
  owned_path=$1
  case "$owned_path" in
  "$build_directory"/*) ;;
  *) fail "refusing to replace a directory outside the SDK build root: $owned_path" ;;
  esac
  rm -rf -- "$owned_path"
  mkdir -p -- "$owned_path"
}

run() {
  printf '> '
  printf '%s ' "$@"
  printf '\n'
  "$@"
}

reset_owned_directory "$platform_artifact_root"
reset_owned_directory "$web_build_directory"
reset_owned_directory "$host_build_directory"
if [ "$host_system" = Darwin ]; then
  command -v xcodebuild >/dev/null 2>&1 || fail "'xcodebuild' is required on PATH"
  reset_owned_directory "$ios_build_directory"
fi

web_version=$(sed -n 's/^set(HUXERUI_WEB_EMSCRIPTEN_VERSION "\([^"]*\)").*/\1/p' \
  "$source_directory/cmake/HuxerUISdk.cmake")
[ -n "$web_version" ] || fail "cannot resolve the HuxerUI Web Emscripten version"
emcc --version 2>&1 | grep "$web_version" >/dev/null || fail "Emscripten $web_version is required"

android_directory="$source_directory/platform/android"
android_properties="$android_directory/gradle.properties"
android_ndk_version=$(sed -n 's/^huxeruiNdkVersion=//p' "$android_properties" | tr -d '\r')
android_min_sdk=$(sed -n 's/^huxeruiMinSdk=//p' "$android_properties" | tr -d '\r')
android_stl=$(sed -n 's/^huxeruiStl=//p' "$android_properties" | tr -d '\r')
android_abis=$(sed -n 's/^huxeruiAbis=//p' "$android_properties" | tr -d '\r' | tr ',' ' ')
[ -n "$android_ndk_version" ] && [ -n "$android_min_sdk" ] && [ -n "$android_stl" ] && \
  [ -n "$android_abis" ] || fail "Android build properties are incomplete"
android_sdk=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}
[ -n "$android_sdk" ] || fail "ANDROID_HOME or ANDROID_SDK_ROOT is required"
android_ndk="$android_sdk/ndk/$android_ndk_version"
[ -f "$android_ndk/build/cmake/android.toolchain.cmake" ] || fail "Android NDK $android_ndk_version is missing"
gradle_variant=$(printf '%s' "$configuration" | tr '[:upper:]' '[:lower:]')
(
  cd "$android_directory"
  run sh ./gradlew ":HuxerUI:assemble$configuration" -PhuxeruiBuildNative=false --no-daemon
)
aar="$android_directory/huxerui/build/outputs/aar/HuxerUI-$gradle_variant.aar"
aar_entries=$(cmake -E tar tf "$aar")
if printf '%s\n' "$aar_entries" | grep -Eq '^(jni|prefab)/'; then
  fail "Android SDK AAR must be Java-only"
fi
android_artifact_directory="$platform_artifact_root/android"
mkdir -p -- "$android_artifact_directory"
cp -- "$aar" "$android_artifact_directory/HuxerUI.aar"
for abi in $android_abis; do
  abi_build="$android_build_directory/$gradle_variant/$abi"
  run cmake -S "$source_directory" -B "$abi_build" -G Ninja \
    "-DCMAKE_TOOLCHAIN_FILE=$android_ndk/build/cmake/android.toolchain.cmake" \
    "-DCMAKE_BUILD_TYPE=$configuration" -DCMAKE_INSTALL_LIBDIR=. \
    "-DANDROID_ABI=$abi" "-DANDROID_PLATFORM=android-$android_min_sdk" "-DANDROID_STL=$android_stl" \
    -DHUXERUI_BUILD_SHARED=ON -DHUXERUI_BUILD_STATIC=OFF -DHUXERUI_ENABLE_PROFILING=OFF \
    -DHUXERUI_BUILD_CLI=OFF -DHUXERUI_BUILD_EXAMPLES=OFF -DHUXERUI_BUILD_TESTS=OFF \
    -DHUXERUI_BUILD_TESTING_LIBRARY=ON -DHUXERUI_BUILD_TESTING_SMOKE_TESTS=OFF
  run cmake --build "$abi_build" --target huxerui huxerui_testing --parallel "$jobs"
  run cmake --install "$abi_build" --config "$configuration" --component HuxerUILibraries \
    --prefix "$android_artifact_directory/$abi" --strip
done

run emcmake cmake -S "$source_directory" -B "$web_build_directory" \
  -DHUXERUI_ENABLE_PROFILING=OFF \
  "-DCMAKE_BUILD_TYPE=$configuration" \
  -DHUXERUI_BUILD_SHARED=OFF \
  -DHUXERUI_BUILD_STATIC=ON \
  -DHUXERUI_BUILD_CLI=OFF \
  -DHUXERUI_BUILD_EXAMPLES=OFF \
  -DHUXERUI_BUILD_TESTING_LIBRARY=ON \
  -DHUXERUI_BUILD_TESTS=OFF
run cmake --build "$web_build_directory" --target huxerui_static huxerui_testing --parallel "$jobs"
web_library="$web_build_directory/lib/libhuxerui_static.a"
[ -f "$web_library" ] || fail "HuxerUI Web build did not produce libhuxerui_static.a"
web_artifact_directory="$platform_artifact_root/web/emscripten-$web_version"
mkdir -p -- "$web_artifact_directory"
cp -- "$web_library" "$web_artifact_directory/libhuxerui.a"
cp -- "$web_build_directory/lib/libhuxerui_testing.a" "$web_artifact_directory/libhuxerui_testing.a"

if [ "$host_system" = Darwin ]; then
  run sh "$source_directory/scripts/build_ios_xcframework.sh" \
    "$source_directory" \
    "$ios_build_directory" \
    "$platform_artifact_root/ios/HuxerUI.xcframework" \
    "$configuration" \
    "$jobs"
fi

run cmake -S "$source_directory" -B "$host_build_directory" \
  -DHUXERUI_ENABLE_PROFILING=OFF \
  "-DCMAKE_BUILD_TYPE=$configuration" \
  -DHUXERUI_BUILD_CLI=ON \
  -DHUXERUI_BUILD_EXAMPLES=OFF \
  -DHUXERUI_BUILD_TESTS=OFF \
  -DHUXERUI_BUILD_TESTING_LIBRARY=ON \
  "-DHUXERUI_INTERNAL_SDK_ARTIFACT_ROOT=$platform_artifact_root"
run cmake --build "$host_build_directory" --config "$configuration" --parallel "$jobs"
run cpack --config "$host_build_directory/CPackConfig.cmake" \
  -C "$configuration" -G TGZ -B "$output_directory"

printf 'HuxerUI SDK package written to %s\n' "$output_directory"
