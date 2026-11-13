#!/bin/sh

set -eu

repository_url="https://github.com/HuxerUI/HuxerUI"
profile_begin="# huxerui-sdk environment begin"
profile_end="# huxerui-sdk environment end"

version=""
prefix=""
profile=""
explicit_profile=false
archive=""
assume_yes=false
uninstall=false
update=false
sdk_command=""
check=false
explicit_version=false
lock_directory=""
temporary_directory=""
staging_directory=""
backup_directory=""
published_directory=""

usage() {
  cat <<'EOF'
Usage:
  install.sh [--version <version>] [--prefix <path>] [--profile <path>] [--archive <path>] [--yes]
  install.sh --uninstall [--prefix <path>] [--profile <path>] [--yes]
  install.sh --update --prefix <path> [--version <version>] [--check] [--yes]
EOF
}

fail() {
  printf 'HuxerUI installer: %s\n' "$1" >&2
  exit 1
}

cleanup() {
  if [ -n "$temporary_directory" ] && [ -d "$temporary_directory" ]; then
    rm -rf "$temporary_directory"
  fi
  if [ -n "$staging_directory" ] && [ -d "$staging_directory" ]; then
    rm -rf "$staging_directory"
  fi
  if [ -n "$published_directory" ] && [ -d "$published_directory" ]; then
    rm -rf "$published_directory"
  fi
  if [ -n "$backup_directory" ] && [ -d "$backup_directory" ]; then
    if [ -n "$prefix" ] && [ ! -e "$prefix" ]; then
      mv "$backup_directory" "$prefix" ||
        printf 'HuxerUI installer: previous SDK remains at %s\n' "$backup_directory" >&2
    else
      printf 'HuxerUI installer: previous SDK remains at %s\n' "$backup_directory" >&2
    fi
  fi
  if [ -n "$lock_directory" ]; then
    rmdir "$lock_directory" || true
  fi
}

trap cleanup EXIT
trap 'exit 1' HUP INT TERM

require_value() {
  [ "$#" -ge 2 ] || fail "$1 requires a value"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
  --version)
    require_value "$@"
    version=$2
    explicit_version=true
    shift 2
    ;;
  --prefix)
    require_value "$@"
    prefix=$2
    shift 2
    ;;
  --profile)
    require_value "$@"
    profile=$2
    explicit_profile=true
    shift 2
    ;;
  --archive)
    require_value "$@"
    archive=$2
    shift 2
    ;;
  --yes)
    assume_yes=true
    shift
    ;;
  --uninstall)
    uninstall=true
    shift
    ;;
  --update)
    update=true
    shift
    ;;
  --sdk)
    require_value "$@"
    sdk_command=$2
    shift 2
    ;;
  --check)
    check=true
    shift
    ;;
  --help | -h)
    usage
    exit 0
    ;;
  *)
    fail "unknown argument: $1"
    ;;
  esac
done

if [ "$check" = true ] && [ "$update" != true ] && [ "$sdk_command" != update ]; then
  fail "--check requires an update"
fi
if [ -n "$sdk_command" ]; then
  case "$sdk_command" in list | use | update) ;; *) fail "unknown SDK command: $sdk_command" ;; esac
  [ -n "$prefix" ] && [ "$uninstall" = false ] && [ "$update" = false ] && [ -z "$archive" ] ||
    fail "SDK management requires the current SDK prefix"
fi
if [ "$update" = true ]; then
  [ -n "$prefix" ] || fail "--update requires --prefix"
  [ "$uninstall" = false ] || fail "--update cannot be combined with --uninstall"
fi

if [ "$uninstall" = true ] && { [ -n "$version" ] || [ -n "$archive" ]; }; then
  fail "--uninstall cannot be combined with --version or --archive"
fi
if [ -n "$version" ] && [ -n "$archive" ]; then
  fail "--version and --archive cannot be combined"
fi

case "$(uname -s)" in
Darwin)
  host_system="macos"
  ;;
Linux)
  if [ -n "${TERMUX_VERSION:-}" ] || { [ -n "${ANDROID_ROOT:-}" ] && [ -x /system/bin/getprop ]; }; then
    host_system="android"
  else
    host_system="linux"
  fi
  ;;
*)
  fail "unsupported operating system: $(uname -s)"
  ;;
esac

case "$(uname -m)" in
arm64 | aarch64)
  if [ "$host_system" = "linux" ]; then
    host_architecture="aarch64"
  elif [ "$host_system" = "android" ]; then
    host_architecture="arm64-v8a"
  else
    host_architecture="arm64"
  fi
  ;;
x86_64 | amd64)
  [ "$host_system" != "android" ] || fail "unsupported Android architecture: $(uname -m)"
  host_architecture="x86_64"
  ;;
*)
  fail "unsupported architecture: $(uname -m)"
  ;;
esac

[ -n "${HOME:-}" ] || fail "HOME is not configured"
if [ -z "$prefix" ]; then
  if [ "$host_system" = "macos" ]; then
    prefix="$HOME/Library/Developer/HuxerUI"
  else
    prefix="$HOME/.local/share/HuxerUI"
  fi
fi
if [ -z "$profile" ]; then
  case "${SHELL:-}" in
  */zsh)
    if [ "$host_system" = "android" ]; then
      profile="$HOME/.zshrc"
    else
      profile="$HOME/.zprofile"
    fi
    ;;
  */bash)
    if [ "$host_system" = "android" ]; then
      profile="$HOME/.bashrc"
    else
      profile="$HOME/.bash_profile"
    fi
    ;;
  *)
    profile="$HOME/.profile"
    ;;
  esac
fi

case "$prefix" in
/*) ;;
*) prefix="$(pwd -P)/$prefix" ;;
esac
case "$profile" in
/*) ;;
*) profile="$(pwd -P)/$profile" ;;
esac
case "/$prefix/" in
*"/../"* | *"/./"*) fail "--prefix must not contain . or .. path components" ;;
esac
case "$prefix" in
"" | "/" | "$HOME" | "$HOME/") fail "unsafe installation prefix: $prefix" ;;
esac
case "$profile" in
"" | "/") fail "unsafe profile path: $profile" ;;
esac

confirm() {
  if [ "$assume_yes" = true ]; then
    return
  fi
  if [ ! -r /dev/tty ]; then
    fail "confirmation requires an interactive terminal or --yes"
  fi
  printf 'Continue? [y/N] ' >/dev/tty
  answer=""
  IFS= read -r answer </dev/tty || true
  case "$answer" in
  y | Y | yes | YES | Yes) ;;
  *) fail "cancelled" ;;
  esac
}

is_sdk() {
  sdk_root=$1
  [ -x "$sdk_root/bin/huxerui" ] &&
    [ -f "$sdk_root/include/huxerui/huxerui.h" ] &&
    [ -f "$sdk_root/lib/cmake/HuxerUI/HuxerUIConfig.cmake" ] &&
    [ -f "$sdk_root/share/huxerui/resources/huxerui/resources.bin" ]
}

acquire_lock() {
  [ ! -L "$prefix" ] || fail "installation prefix must not be a symbolic link: $prefix"
  mkdir "$prefix.huxerui-lock" 2>/dev/null ||
    fail "another installer may be using this SDK; inspect $prefix.huxerui-lock before retrying"
  lock_directory="$prefix.huxerui-lock"
}

sdk_version() {
  sdk_output=$(HUXERUI_HOME="$1" "$1/bin/huxerui" --version) || fail "SDK CLI cannot run: $1"
  case "$sdk_output" in
  "huxerui "*) printf '%s\n' "${sdk_output#huxerui }" ;;
  *) fail "SDK CLI returned an invalid version: $1" ;;
  esac
}

is_version() {
  printf '%s\n' "$1" | LC_ALL=C awk '
    /^[0-9]+\.[0-9]+\.[0-9]+$/ { valid = 1 }
    END { exit !(NR == 1 && valid) }
  '
}

validate_version() {
  is_version "$1" || fail "expected a major.minor.patch release version: $1"
}

version_is_older() {
  awk -v first="$1" -v second="$2" 'BEGIN {
    split(first, a, "."); split(second, b, ".");
    for (i = 1; i <= 3; ++i) {
      if (a[i] + 0 < b[i] + 0) exit 0
      if (a[i] + 0 > b[i] + 0) exit 1
    }
    exit 1
  }'
}

shell_quote() {
  printf "'"
  printf '%s' "$1" | sed "s/'/'\\\\''/g"
  printf "'"
}

profile_mode() {
  if stat -f '%Lp' "$1" >/dev/null 2>&1; then
    stat -f '%Lp' "$1"
  else
    stat -c '%a' "$1"
  fi
}

rewrite_profile() {
  rewrite_mode=$1
  profile_directory=$(dirname "$profile")
  [ ! -d "$profile" ] || return 1
  mkdir -p "$profile_directory" || return 1
  profile_temporary=$(mktemp "$profile_directory/.huxerui-profile.XXXXXX") || return 1
  original_mode=""
  if [ -f "$profile" ]; then
    original_mode=$(profile_mode "$profile") || { rm -f "$profile_temporary"; return 1; }
    if ! awk -v begin="$profile_begin" -v end="$profile_end" '
      $0 == begin { skipping = 1; next }
      skipping && $0 == end { skipping = 0; next }
      !skipping { print }
      END { if (skipping) exit 2 }
    ' "$profile" >"$profile_temporary"; then
      rm -f "$profile_temporary"
      return 1
    fi
  fi

  if [ "$rewrite_mode" = add ]; then
    quoted_prefix=$(shell_quote "$prefix")
    if ! printf '%s\n' "$profile_begin" "export HUXERUI_HOME=$quoted_prefix" \
      'export PATH="$HUXERUI_HOME/bin:$PATH"' "export _HUXERUI_SDK_PROFILE=$(shell_quote "$profile")" \
      "$profile_end" >>"$profile_temporary"; then
      rm -f "$profile_temporary"
      return 1
    fi
  fi

  if [ -n "$original_mode" ]; then
    chmod "$original_mode" "$profile_temporary" || { rm -f "$profile_temporary"; return 1; }
  fi
  mv -f "$profile_temporary" "$profile" || { rm -f "$profile_temporary"; return 1; }
}

remove_owned_profile_environment() {
  if [ ! -f "$profile" ]; then
    return
  fi
  quoted_prefix=$(shell_quote "$prefix")
  expected_home="export HUXERUI_HOME=$quoted_prefix"
  if grep -Fqx "$profile_begin" "$profile" && grep -Fqx "$expected_home" "$profile"; then
    rewrite_profile remove || fail "profile contains an incomplete HuxerUI environment block: $profile"
  elif grep -Fqx "$profile_begin" "$profile"; then
    printf 'Leaving HuxerUI environment for another SDK in %s\n' "$profile"
  fi
}

normalize_prefix() {
  prefix_parent=$(dirname "$prefix")
  prefix_name=$(basename "$prefix")
  mkdir -p "$prefix_parent"
  prefix_parent=$(cd "$prefix_parent" && pwd -P)
  prefix="$prefix_parent/$prefix_name"
  case "$prefix" in
  "/" | "$HOME" | "$HOME/") fail "unsafe installation prefix: $prefix" ;;
  esac
}

profile_sdk_selection() {
  [ -f "$1" ] || return 0
  awk -v begin="$profile_begin" -v end="$profile_end" '
    $0 == begin { managed = 1; next }
    $0 == end { managed = 0 }
    managed && /^export HUXERUI_HOME=/ { selection = $0 }
    END { if (selection != "") print selection }
  ' "$1"
}

list_sdk() {
  marker=' '
  if [ "$profile_selection" = "export HUXERUI_HOME=$(shell_quote "$1")" ]; then
    marker='*'
  fi
  printf '%s %-10s %s\n' "$marker" "$2" "$1"
}

find_sdk_profile() {
  original_selection="export HUXERUI_HOME=$(shell_quote "$original_prefix")"
  version_selection="export HUXERUI_HOME=$(shell_quote "$versions_directory/")"
  # Match the quoted directory prefix and validate the remaining version.
  version_selection=${version_selection%?}
  for candidate in "${_HUXERUI_SDK_PROFILE:-}" "$profile" \
    "$HOME/.zprofile" "$HOME/.zshrc" "$HOME/.bash_profile" "$HOME/.bashrc" "$HOME/.profile"; do
    candidate_selection=$(profile_sdk_selection "$candidate")
    case "$candidate_selection" in
    "$original_selection") ;;
    "$version_selection"*"'")
      selected_version=${candidate_selection#"$version_selection"}
      is_version "${selected_version%?}" || continue
      ;;
    *) continue ;;
    esac
    profile=$candidate
    return
  done
}

verify_sdk_selection() {
  is_sdk "$1" && [ -d "$1/share/huxerui/tools" ] && [ -d "$1/share/huxerui/skills/huxerui-app-development" ] ||
    fail "SDK directory is incomplete: $1"
  [ "$(sdk_version "$1")" = "$version" ] || fail "SDK version does not match $version: $1"
  HUXERUI_HOME="$1" "$1/bin/huxerui" sdk --help >/dev/null 2>&1 ||
    fail "HuxerUI $version does not support SDK switching"
}

select_sdk() {
  verify_sdk_selection "$prefix"
  rewrite_profile add || fail "profile contains an incomplete HuxerUI environment block: $profile"
}

print_selection() {
  printf 'Selected HuxerUI SDK %s at %s\n' "$version" "$prefix"
  printf 'Restart the terminal to use the updated environment. Shell profile: %s\n' "$profile"
}

if [ -n "$sdk_command" ]; then
  is_sdk "$prefix" || fail "SDK management requires an installed SDK: $prefix"
  current_prefix=$prefix
  current_version=$(sdk_version "$current_prefix")
  validate_version "$current_version"
  original_prefix=$current_prefix
  current_parent=$(dirname "$current_prefix")
  if [ "$(basename "$current_prefix")" = "$current_version" ]; then
    case "$current_parent" in
    *-versions) original_prefix=${current_parent%-versions} ;;
    esac
  fi
  versions_directory="$original_prefix-versions"
  [ ! -L "$versions_directory" ] || fail "SDK versions directory must not be a symbolic link: $versions_directory"
  if [ "$explicit_profile" = false ]; then
    find_sdk_profile
  fi

  if [ "$sdk_command" = list ]; then
    profile_selection=$(profile_sdk_selection "$profile")
    printf 'Installed HuxerUI SDKs (* default):\n'
    if is_sdk "$original_prefix"; then
      list_sdk "$original_prefix" "$(sdk_version "$original_prefix")"
    fi
    for installed in "$versions_directory"/*; do
      installed_version=${installed##*/}
      if [ -d "$installed" ] && [ ! -L "$installed" ] && is_version "$installed_version" && is_sdk "$installed"; then
        list_sdk "$installed" "$installed_version"
      fi
    done
    exit 0
  fi
  if [ "$sdk_command" = use ]; then
    version=${version#v}
    validate_version "$version"
  elif [ -n "$version" ]; then
    fail "SDK update selects the latest stable release; use sdk use to select a version"
  fi
fi

if [ "$uninstall" = true ]; then
  [ -d "$prefix" ] || fail "HuxerUI SDK is not installed at $prefix"
  normalize_prefix
  is_sdk "$prefix" || fail "refusing to remove a directory that is not a HuxerUI SDK: $prefix"
  printf 'Uninstall HuxerUI SDK\n  SDK: %s\n  Profile: %s\n' "$prefix" "$profile"
  confirm
  acquire_lock
  remove_owned_profile_environment
  rm -rf "$prefix"
  printf 'HuxerUI SDK removed from %s\n' "$prefix"
  exit 0
fi

if [ -d "$prefix" ] && ! is_sdk "$prefix"; then
  fail "installation prefix exists but is not a HuxerUI SDK: $prefix"
fi
if [ "$update" = true ]; then
  is_sdk "$prefix" || fail "--update requires an installed HuxerUI SDK: $prefix"
  [ ! -f "$prefix/CMakeLists.txt" ] || fail "refusing to update a source checkout"
  current_version=$(sdk_version "$prefix")
  validate_version "$current_version"
fi

release_tag=""
if [ -z "$archive" ]; then
  if [ -z "$version" ]; then
    latest_url=$(curl -fsSL -o /dev/null -w '%{url_effective}' "$repository_url/releases/latest") ||
      fail "failed to resolve the latest HuxerUI release"
    release_tag=${latest_url##*/}
    case "$release_tag" in
    v*) version=${release_tag#v} ;;
    *) fail "latest HuxerUI release has an invalid tag: $release_tag" ;;
    esac
  else
    version=${version#v}
    release_tag="v$version"
  fi
  case "$version" in
  "" | *[!0-9A-Za-z._-]*) fail "invalid version: $version" ;;
  esac
  archive_name="huxerui-sdk-$version-$host_system-$host_architecture.tar.gz"
  archive_source="$repository_url/releases/download/$release_tag/$archive_name"
  archive_display="$archive_source"
else
  case "$archive" in
  /*) ;;
  *) archive="$(pwd -P)/$archive" ;;
  esac
  [ -f "$archive" ] || fail "archive does not exist: $archive"
  archive_name=$(basename "$archive")
  archive_display="$archive"
fi

case "$archive_name" in
huxerui-sdk-*-$host_system-$host_architecture.tar.gz) ;;
*) fail "archive does not match this host: $archive_name" ;;
esac

if [ "$update" = true ]; then
  version=${archive_name#huxerui-sdk-}
  version=${version%-$host_system-$host_architecture.tar.gz}
  validate_version "$version"
  printf 'Update HuxerUI SDK\n  SDK: %s\n  Current: %s\n  Target: %s\n  Package: %s-%s\n' \
    "$prefix" "$current_version" "$version" "$host_system" "$host_architecture"
  if [ "$version" = "$current_version" ]; then
    printf 'HuxerUI SDK is already at the requested version.\n'
    exit 0
  fi
  if [ "$explicit_version" = false ] && [ -z "$archive" ] && version_is_older "$version" "$current_version"; then
    printf 'The installed SDK is newer; use --version to explicitly downgrade.\n'
    exit 0
  fi
  if [ "$check" = true ]; then
    printf 'HuxerUI SDK update available.\n'
    exit 0
  fi
  printf 'The entire SDK will be replaced, including local modifications. Stop SDK builds and tools first.\n'
elif [ -n "$sdk_command" ]; then
  validate_version "$version"
  if [ "$sdk_command" = update ]; then
    printf 'Current HuxerUI SDK: %s\nLatest stable SDK: %s\n' "$current_version" "$version"
    if version_is_older "$version" "$current_version"; then
      printf 'The installed SDK is newer than the latest stable release.\n'
      exit 0
    fi
    if [ "$current_version" = "$version" ]; then
      printf 'HuxerUI SDK is up to date.\n'
      exit 0
    fi
    if [ "$check" = true ]; then
      printf 'HuxerUI SDK update available.\n'
      exit 0
    fi
  fi
  if version_is_older "$version" 0.4.0; then
    fail "SDK switching starts with HuxerUI 0.4.0"
  fi

  prefix="$versions_directory/$version"
  if [ "$version" = "$current_version" ]; then
    prefix=$current_prefix
  elif [ ! -e "$prefix" ] && is_sdk "$original_prefix" &&
    [ "$(sdk_version "$original_prefix")" = "$version" ]; then
    prefix=$original_prefix
  fi
  [ ! -L "$prefix" ] || fail "SDK directory must not be a symbolic link: $prefix"
  if [ -e "$prefix" ]; then
    if [ "$sdk_command" = update ]; then
      printf 'Switch to the installed HuxerUI SDK %s at %s\n' "$version" "$prefix"
      confirm
    fi
    acquire_lock
    select_sdk
    print_selection
    exit 0
  fi
  printf 'Install and select HuxerUI SDK %s at %s\n' "$version" "$prefix"
else
  printf 'Install HuxerUI SDK\n  Archive: %s\n  SDK: %s\n  Profile: %s\n' "$archive_display" "$prefix" "$profile"
fi
confirm
if [ "$update" = true ]; then
  normalize_prefix
  acquire_lock
  [ "$(sdk_version "$prefix")" = "$current_version" ] || fail "SDK changed during the update check; retry"
fi

temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/huxerui-sdk.XXXXXX")
if [ -z "$archive" ]; then
  archive="$temporary_directory/$archive_name"
  curl -fL --retry 3 -o "$archive" "$archive_source" || fail "failed to download $archive_source"
  curl -fL --retry 3 -o "$archive.sha256" "$archive_source.sha256" ||
    fail "failed to download $archive_source.sha256"
fi

checksum_path="$archive.sha256"
[ -f "$checksum_path" ] || fail "archive checksum does not exist: $checksum_path"
expected_checksum=""
checksum_name=""
checksum_extra=""
IFS=' ' read -r expected_checksum checksum_name checksum_extra <"$checksum_path" ||
  fail "archive checksum is malformed: $checksum_path"
if [ -z "$expected_checksum" ] || [ -z "$checksum_name" ] || [ -n "$checksum_extra" ]; then
  fail "archive checksum is malformed: $checksum_path"
fi
[ "$checksum_name" = "$archive_name" ] || fail "archive checksum names a different file: $checksum_name"
if command -v shasum >/dev/null 2>&1; then
  actual_checksum=$(shasum -a 256 "$archive" | awk '{ print $1 }')
else
  actual_checksum=$(sha256sum "$archive" | awk '{ print $1 }')
fi
[ "$actual_checksum" = "$expected_checksum" ] || fail "archive checksum does not match: $archive_name"

archive_root=${archive_name%.tar.gz}
archive_listing="$temporary_directory/archive.txt"
tar -tzf "$archive" >"$archive_listing" || fail "archive cannot be read: $archive_name"
if ! awk -v root="$archive_root/" '
  index($0, root) != 1 { exit 1 }
  $0 ~ /(^|\/)\.\.(\/|$)/ { exit 1 }
' "$archive_listing"; then
  fail "archive contains an invalid path"
fi

extract_directory="$temporary_directory/extract"
mkdir -p "$extract_directory"
tar -xzf "$archive" -C "$extract_directory" || fail "archive extraction failed"
extracted_sdk="$extract_directory/$archive_root"
extracted_sdk=$(cd "$extracted_sdk" && pwd -P) || fail "archive SDK directory is missing"
find "$extracted_sdk" -type l -exec sh -c '
  root=$1
  failures=$2
  shift 2
  for link do
    target=$(readlink "$link") || { printf "invalid link\n" >>"$failures"; continue; }
    case "$target" in
    /*) printf "absolute link\n" >>"$failures"; continue ;;
    esac
    if [ -d "$link" ]; then
      resolved=$(cd "$link" && pwd -P) || resolved=""
    else
      resolved=$(cd "$(dirname "$link")/$(dirname "$target")" && pwd -P) || resolved=""
    fi
    case "$resolved/" in
    "$root/"*) ;;
    *) printf "escaping link\n" >>"$failures" ;;
    esac
  done
' sh "$extracted_sdk" "$temporary_directory/invalid-links" {} +
[ ! -f "$temporary_directory/invalid-links" ] || fail "archive contains an unsafe symbolic link"
is_sdk "$extracted_sdk" || fail "archive does not contain a complete HuxerUI SDK"
if [ "$update" = true ]; then
  [ "$(sdk_version "$extracted_sdk")" = "$version" ] || fail "archive SDK version does not match $version"
elif [ -n "$sdk_command" ]; then
  verify_sdk_selection "$extracted_sdk"
fi

normalize_prefix
if [ -z "$lock_directory" ]; then
  acquire_lock
fi
if [ -n "$sdk_command" ] && [ -e "$prefix" ]; then
  select_sdk
  print_selection
  exit 0
fi
staging_directory=$(mktemp -d "$(dirname "$prefix")/.huxerui-install.XXXXXX")
rmdir "$staging_directory"
mv "$extracted_sdk" "$staging_directory"
is_sdk "$staging_directory" || fail "staged HuxerUI SDK is incomplete"

if [ -d "$prefix" ]; then
  backup_directory=$(mktemp -d "$(dirname "$prefix")/.huxerui-backup.XXXXXX")
  rmdir "$backup_directory"
  mv "$prefix" "$backup_directory"
fi

if ! mv "$staging_directory" "$prefix"; then
  if [ -n "$backup_directory" ] && [ -d "$backup_directory" ]; then
    mv "$backup_directory" "$prefix"
    backup_directory=""
  fi
  fail "failed to publish the HuxerUI SDK"
fi
staging_directory=""
published_directory="$prefix"

if [ "$update" = true ]; then
  [ "$(sdk_version "$prefix")" = "$version" ] || fail "installed SDK version does not match $version"
elif [ -n "$sdk_command" ]; then
  select_sdk
elif ! rewrite_profile add; then
  rm -rf "$prefix"
  published_directory=""
  if [ -n "$backup_directory" ] && [ -d "$backup_directory" ]; then
    mv "$backup_directory" "$prefix"
    backup_directory=""
  fi
  fail "profile contains an incomplete HuxerUI environment block: $profile"
fi

# Once publication succeeds, cleanup must not roll back to a partially removed backup.
published_directory=""
if [ -n "$backup_directory" ] && [ -d "$backup_directory" ]; then
  if ! rm -rf "$backup_directory"; then
    printf 'HuxerUI installer: could not completely remove old SDK backup; remaining files at %s\n' \
      "$backup_directory" >&2
  fi
  backup_directory=""
fi

if [ -n "$sdk_command" ]; then
  print_selection
else
  printf 'HuxerUI SDK installed at %s\n' "$prefix"
  if [ "$update" = false ]; then
    printf 'Restart the terminal or run: . %s\n' "$profile"
  fi
fi
