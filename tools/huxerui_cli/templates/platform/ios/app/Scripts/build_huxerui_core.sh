set -eu

if [ -z "${HUXERUI_HOME:-}" ]; then
  echo "error: HUXERUI_HOME is not configured; set it in Config/Local.xcconfig" >&2
  exit 1
fi

# The integration that builds the application core: the one HUXERUI_BUILD_SYSTEM names, the project's only
# declaration, or CMake when the project declares both. Everything else in this shell is shared.
if [ -z "${HUXERUI_BUILD_SYSTEM:-}" ]; then
  if [ -f "$HUXERUI_PROJECT_ROOT/CMakeLists.txt" ]; then
    HUXERUI_BUILD_SYSTEM=cmake
  else
    HUXERUI_BUILD_SYSTEM=mcpp
  fi
fi

if [ "$HUXERUI_BUILD_SYSTEM" = mcpp ]; then
  # mcpp builds the core for arm64; its archive carries the C++ runtime the core was compiled against, and the
  # shell calls the core only through huxerui::RunApplication(), whose signature names no standard-library type.
  case "$PLATFORM_NAME" in
    iphonesimulator) HUXERUI_MCPP_TARGET=aarch64-ios-sim ;;
    iphoneos) HUXERUI_MCPP_TARGET=aarch64-ios ;;
    *) echo "error: the mcpp integration builds no core for $PLATFORM_NAME" >&2; exit 1 ;;
  esac
  for HUXERUI_ARCH in $ARCHS; do
    if [ "$HUXERUI_ARCH" != arm64 ]; then
      echo "error: the mcpp integration builds the iOS core for arm64 only, not $HUXERUI_ARCH" >&2
      exit 1
    fi
  done
  # The CLI passes the profile and features it resolved from its options; a build from Xcode itself takes the
  # configuration's own profile.
  if [ -z "${HUXERUI_MCPP_PROFILE:-}" ]; then
    HUXERUI_MCPP_PROFILE=dev
    if [ "$CONFIGURATION" = Release ]; then
      HUXERUI_MCPP_PROFILE=release
    fi
  fi
  set -- build --target "$HUXERUI_MCPP_TARGET" --profile "$HUXERUI_MCPP_PROFILE" --message-format json
  if [ -n "${HUXERUI_MCPP_FEATURES:-}" ]; then
    set -- "$@" --features "$HUXERUI_MCPP_FEATURES"
  fi
  mkdir -p "$HUXERUI_CORE_BUILD_DIR/huxerui-ios"
  HUXERUI_MCPP_REPORT="$HUXERUI_CORE_BUILD_DIR/mcpp-build.json"
  HUXERUI_MCPP_COMMAND=${HUXERUI_MCPP:-mcpp}
  # Xcode exports every build setting to this script, over 500 variables, and mcpp cannot start a program whose
  # environment holds more than 512 entries; it reports that as running out of memory. The settings describe this
  # shell's build, not the core's, and the iOS SDK and deployment target among them would also reach the compiler of
  # the build programs mcpp runs on this Mac. mcpp therefore gets the user's environment without them.
  (
    cd "$HUXERUI_PROJECT_ROOT"
    for HUXERUI_NAME in $(env | sed -n 's/^\([A-Za-z_][A-Za-z0-9_]*\)=.*/\1/p'); do
      case "$HUXERUI_NAME" in
        HOME | USER | LOGNAME | PATH | SHELL | TMPDIR | TERM | LANG | LC_* | DEVELOPER_DIR | MCPP_* | XLINGS_* | \
          *_proxy | *_PROXY | SSL_CERT_*) ;;
        *) unset "$HUXERUI_NAME" 2> /dev/null || true ;;
      esac
    done
    exec "$HUXERUI_MCPP_COMMAND" "$@"
  ) > "$HUXERUI_MCPP_REPORT"
  # The application's own artifact; dependencies such as the compiler runtime report library artifacts too.
  HUXERUI_MCPP_PROGRAM=$(grep '"event":"artifact"' "$HUXERUI_MCPP_REPORT" |
    grep -E '"kind":"(bin|app|shared)"' | sed -n 's/.*"path":"\([^"]*\)".*/\1/p' | head -n 1)
  if [ -z "$HUXERUI_MCPP_PROGRAM" ]; then
    echo "error: mcpp reported no application artifact; see $HUXERUI_MCPP_REPORT" >&2
    exit 1
  fi
  HUXERUI_MCPP_CORE=$(dirname "$HUXERUI_MCPP_PROGRAM")/huxerui-ios
  set -- "$HUXERUI_MCPP_CORE"/*/
  if [ "$#" -ne 1 ] || [ ! -f "$1/link.rsp" ]; then
    echo "error: mcpp produced no application core under $HUXERUI_MCPP_CORE" >&2
    exit 1
  fi
  # The core keeps the location mcpp gave it; the shell reads it where it reads a CMake core.
  rm -rf "$HUXERUI_CORE_BUILD_DIR/huxerui-ios/@TARGET_NAME@"
  ln -s "${1%/}" "$HUXERUI_CORE_BUILD_DIR/huxerui-ios/@TARGET_NAME@"
  exit 0
fi

HUXERUI_CMAKE_ARCHS=$(printf '%s' "$ARCHS" | tr ' ' ';')
"${HUXERUI_CMAKE_EXECUTABLE:-cmake}" -S "$HUXERUI_PROJECT_ROOT" -B "$HUXERUI_CORE_BUILD_DIR" \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT="$SDKROOT" \
  -DCMAKE_OSX_ARCHITECTURES="$HUXERUI_CMAKE_ARCHS" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$IPHONEOS_DEPLOYMENT_TARGET" \
  -DCMAKE_BUILD_TYPE="$CONFIGURATION" \
  -DHUXERUI_HOME="$HUXERUI_HOME" \
  -DHUXERUI_OPTIMIZATION="${HUXERUI_OPTIMIZATION:-default}" \
  -DHUXERUI_ENABLE_LTO="${HUXERUI_ENABLE_LTO:-OFF}" \
  -DHUXERUI_ENABLE_PROFILING="${HUXERUI_ENABLE_PROFILING:-OFF}" \
  -DHUXERUI_BUILD_SHARED=OFF \
  -DHUXERUI_BUILD_TESTS=OFF \
  -DHUXERUI_BUILD_EXAMPLES=OFF \
  -DHUXERUI_BUILD_CLI=OFF
# A bare --parallel lets Make start every compile at once, which exhausts memory on a small Mac.
"${HUXERUI_CMAKE_EXECUTABLE:-cmake}" --build "$HUXERUI_CORE_BUILD_DIR" --target @TARGET_NAME@_huxerui_ios_core --parallel "$(sysctl -n hw.logicalcpu)"
