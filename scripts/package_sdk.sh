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

for command_name in cmake cpack java jar emcmake emcc; do
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
android_extract_directory="$build_directory/android-aar"
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
reset_owned_directory "$android_extract_directory"
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
[ -f "$android_directory/gradlew" ] || fail "HuxerUI Android Gradle wrapper is missing"
gradle_variant=$(printf '%s' "$configuration" | tr '[:upper:]' '[:lower:]')
(
  cd "$android_directory"
  run sh ./gradlew ":HuxerUI:assemble$configuration" -PhuxeruiBuildNative=true --no-daemon
)

aar_count=$(find "$android_directory/huxerui/build/outputs/aar" -maxdepth 1 \
  -type f -name "*-$gradle_variant.aar" -print | wc -l | tr -d ' ')
[ "$aar_count" -eq 1 ] || fail "expected one HuxerUI Android $configuration AAR, found $aar_count"
aar_path=$(find "$android_directory/huxerui/build/outputs/aar" -maxdepth 1 \
  -type f -name "*-$gradle_variant.aar" -print -quit)
(
  cd "$android_extract_directory"
  run jar -xf "$aar_path"
)

android_artifact_directory="$platform_artifact_root/android"
mkdir -p -- "$android_artifact_directory"
for abi in arm64-v8a x86_64; do
  abi_output="$android_artifact_directory/$abi"
  mkdir -p -- "$abi_output"
  shared_library="$android_extract_directory/jni/$abi/libhuxerui.so"
  [ -f "$shared_library" ] || fail "HuxerUI Android AAR is missing $abi/libhuxerui.so"
  cp -- "$shared_library" "$abi_output/libhuxerui.so"
done

rm -rf -- "$android_extract_directory/jni" "$android_extract_directory/prefab"
run jar --create --file "$android_artifact_directory/HuxerUI.aar" \
  --no-manifest -C "$android_extract_directory" .

run emcmake cmake -S "$source_directory" -B "$web_build_directory" \
  "-DCMAKE_BUILD_TYPE=$configuration" \
  -DHUXERUI_BUILD_SHARED=OFF \
  -DHUXERUI_BUILD_STATIC=ON \
  -DHUXERUI_BUILD_CLI=OFF \
  -DHUXERUI_BUILD_EXAMPLES=OFF \
  -DHUXERUI_BUILD_TESTS=OFF
run cmake --build "$web_build_directory" --target huxerui_static --parallel "$jobs"
web_library="$web_build_directory/lib/libhuxerui_static.a"
[ -f "$web_library" ] || fail "HuxerUI Web build did not produce libhuxerui_static.a"
web_artifact_directory="$platform_artifact_root/web/emscripten-$web_version"
mkdir -p -- "$web_artifact_directory"
cp -- "$web_library" "$web_artifact_directory/libhuxerui.a"

if [ "$host_system" = Darwin ]; then
  run sh "$source_directory/scripts/build_ios_xcframework.sh" \
    "$source_directory" \
    "$ios_build_directory" \
    "$platform_artifact_root/ios/HuxerUI.xcframework" \
    "$configuration" \
    "$jobs"
fi

run cmake -S "$source_directory" -B "$host_build_directory" \
  "-DCMAKE_BUILD_TYPE=$configuration" \
  -DHUXERUI_BUILD_CLI=ON \
  -DHUXERUI_BUILD_EXAMPLES=OFF \
  -DHUXERUI_BUILD_TESTS=OFF \
  "-DHUXERUI_INTERNAL_SDK_ARTIFACT_ROOT=$platform_artifact_root"
run cmake --build "$host_build_directory" --config "$configuration" --parallel "$jobs"
run cpack --config "$host_build_directory/CPackConfig.cmake" \
  -C "$configuration" -G TGZ -B "$output_directory"

printf 'HuxerUI SDK package written to %s\n' "$output_directory"
