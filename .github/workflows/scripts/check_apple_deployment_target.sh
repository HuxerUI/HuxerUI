#!/bin/sh

set -eu

fail() {
  printf 'HuxerUI Apple deployment target: %s\n' "$1" >&2
  exit 1
}

if [ "$#" -lt 2 ]; then
  fail "usage: check_apple_deployment_target.sh <expected-version> <binary>..."
fi

expected_version=$1
shift

command -v otool >/dev/null 2>&1 || fail "'otool' is required on PATH"

for binary_path in "$@"; do
  [ -f "$binary_path" ] || fail "binary is missing: $binary_path"
  deployment_versions=$(otool -l "$binary_path" | awk '$1 == "minos" { print $2 }' | LC_ALL=C sort -u)
  [ -n "$deployment_versions" ] || fail "binary has no minimum deployment version: $binary_path"
  [ "$deployment_versions" = "$expected_version" ] || \
    fail "expected $expected_version for $binary_path, found $(printf '%s' "$deployment_versions" | tr '\n' ' ')"
done
