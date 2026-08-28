#!/bin/sh

set -eu

maximum_glibc=${HUXERUI_MAX_GLIBC:-2.35}
maximum_glibcxx=${HUXERUI_MAX_GLIBCXX:-3.4.32}
maximum_cxxabi=${HUXERUI_MAX_CXXABI:-1.3.15}
require_static_cxx=${HUXERUI_REQUIRE_STATIC_CXX:-false}

if [ "$#" -eq 0 ]; then
  printf 'Usage: check_linux_binary_compatibility.sh <ELF file>...\n' >&2
  exit 1
fi

highest_symbol_version() {
  symbol_prefix=$1
  binary_path=$2
  readelf --version-info "$binary_path" 2>/dev/null |
    grep -o "${symbol_prefix}_[0-9.]*" |
    sed "s/^${symbol_prefix}_//" |
    sort -Vu |
    tail -n 1
}

version_exceeds() {
  actual=$1
  maximum=$2
  [ "$(printf '%s\n%s\n' "$actual" "$maximum" | sort -V | tail -n 1)" != "$maximum" ]
}

failed=false
for binary_path in "$@"; do
  if [ ! -f "$binary_path" ] || ! readelf --file-header "$binary_path" >/dev/null 2>&1; then
    printf 'HuxerUI Linux compatibility: not an ELF file: %s\n' "$binary_path" >&2
    failed=true
    continue
  fi

  glibc_version=$(highest_symbol_version GLIBC "$binary_path")
  glibcxx_version=$(highest_symbol_version GLIBCXX "$binary_path")
  cxxabi_version=$(highest_symbol_version CXXABI "$binary_path")
  [ -n "$glibc_version" ] || glibc_version=none
  [ -n "$glibcxx_version" ] || glibcxx_version=static
  [ -n "$cxxabi_version" ] || cxxabi_version=static
  printf '%s: GLIBC %s, GLIBCXX %s, CXXABI %s\n' \
    "$binary_path" "$glibc_version" "$glibcxx_version" "$cxxabi_version"

  if [ "$glibc_version" != none ] && version_exceeds "$glibc_version" "$maximum_glibc"; then
    printf 'HuxerUI Linux compatibility: %s exceeds GLIBC %s\n' "$binary_path" "$maximum_glibc" >&2
    failed=true
  fi
  if [ "$glibcxx_version" != static ] && version_exceeds "$glibcxx_version" "$maximum_glibcxx"; then
    printf 'HuxerUI Linux compatibility: %s exceeds GLIBCXX %s\n' "$binary_path" "$maximum_glibcxx" >&2
    failed=true
  fi
  if [ "$cxxabi_version" != static ] && version_exceeds "$cxxabi_version" "$maximum_cxxabi"; then
    printf 'HuxerUI Linux compatibility: %s exceeds CXXABI %s\n' "$binary_path" "$maximum_cxxabi" >&2
    failed=true
  fi
  if [ "$require_static_cxx" = true ] &&
    { [ "$glibcxx_version" != static ] || [ "$cxxabi_version" != static ]; }; then
    printf 'HuxerUI Linux compatibility: %s dynamically links the GNU C++ runtime\n' "$binary_path" >&2
    failed=true
  fi
done

[ "$failed" = false ]
