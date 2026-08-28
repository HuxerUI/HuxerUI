#!/bin/sh

set -eu

repository_url="https://github.com/HuxerUI/HuxerUI"
profile_begin="# huxerui-sdk environment begin"
profile_end="# huxerui-sdk environment end"

version=""
prefix=""
profile=""
archive=""
assume_yes=false
uninstall=false
temporary_directory=""
staging_directory=""
backup_directory=""
published_directory=""

usage() {
  cat <<'EOF'
Usage:
  install.sh [--version <version>] [--prefix <path>] [--profile <path>] [--archive <path>] [--yes]
  install.sh --uninstall [--prefix <path>] [--profile <path>] [--yes]
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
  --help | -h)
    usage
    exit 0
    ;;
  *)
    fail "unknown argument: $1"
    ;;
  esac
done

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
  mkdir -p "$profile_directory"
  profile_temporary=$(mktemp "$profile_directory/.huxerui-profile.XXXXXX")
  original_mode=""
  if [ -f "$profile" ]; then
    original_mode=$(profile_mode "$profile")
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
    {
      printf '%s\n' "$profile_begin"
      printf 'export HUXERUI_HOME=%s\n' "$quoted_prefix"
      printf '%s\n' 'export PATH="$HUXERUI_HOME/bin:$PATH"'
      printf '%s\n' "$profile_end"
    } >>"$profile_temporary"
  fi

  if [ -n "$original_mode" ]; then
    chmod "$original_mode" "$profile_temporary"
  fi
  mv "$profile_temporary" "$profile"
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

if [ "$uninstall" = true ]; then
  [ -d "$prefix" ] || fail "HuxerUI SDK is not installed at $prefix"
  normalize_prefix
  is_sdk "$prefix" || fail "refusing to remove a directory that is not a HuxerUI SDK: $prefix"
  printf 'Uninstall HuxerUI SDK\n  SDK: %s\n  Profile: %s\n' "$prefix" "$profile"
  confirm
  remove_owned_profile_environment
  rm -rf "$prefix"
  printf 'HuxerUI SDK removed from %s\n' "$prefix"
  exit 0
fi

if [ -d "$prefix" ] && ! is_sdk "$prefix"; then
  fail "installation prefix exists but is not a HuxerUI SDK: $prefix"
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

printf 'Install HuxerUI SDK\n  Archive: %s\n  SDK: %s\n  Profile: %s\n' "$archive_display" "$prefix" "$profile"
confirm

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
is_sdk "$extracted_sdk" || fail "archive does not contain a complete HuxerUI SDK"

normalize_prefix
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

if ! rewrite_profile add; then
  rm -rf "$prefix"
  published_directory=""
  if [ -n "$backup_directory" ] && [ -d "$backup_directory" ]; then
    mv "$backup_directory" "$prefix"
    backup_directory=""
  fi
  fail "profile contains an incomplete HuxerUI environment block: $profile"
fi

if [ -n "$backup_directory" ] && [ -d "$backup_directory" ]; then
  rm -rf "$backup_directory"
  backup_directory=""
fi
published_directory=""

printf 'HuxerUI SDK installed at %s\n' "$prefix"
printf 'Restart the terminal or run: . %s\n' "$profile"
