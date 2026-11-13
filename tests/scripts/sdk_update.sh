#!/bin/sh
set -eu

installer=$1
cli=$2
test_root=$(mktemp -d "${TMPDIR:-/tmp}/huxerui-update-test.XXXXXX")
trap 'rm -rf "$test_root"' EXIT
test_root=$(cd "$test_root" && pwd -P)
export TEST_PREFIX="$test_root/SDK with spaces and 'quotes'"
export TEST_PACKAGES="$test_root/packages"
export TEST_CURL_LOG="$test_root/curl.log"
export TEST_LATEST=0.2.0
mkdir -p "$TEST_PACKAGES" "$test_root/bin"
case "$(uname -s)" in
Darwin) host=macos ;;
Linux) host=linux ;;
*) exit 0 ;;
esac
case "$(uname -m)" in
arm64 | aarch64)
  if [ "$host" = macos ]; then arch=arm64; else arch=aarch64; fi ;;
*) arch=x86_64 ;;
esac

cat >"$test_root/bin/curl" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$*" >>"$TEST_CURL_LOG"
[ "${TEST_NETWORK_FAILURE:-false}" = false ] || exit 22
output=""
latest=false
while [ "$#" -gt 0 ]; do
  case "$1" in
  -o) output=$2; shift 2 ;;
  -w) latest=true; shift 2 ;;
  --retry) shift 2 ;;
  *) url=$1; shift ;;
  esac
done
if [ "$latest" = true ]; then
  printf 'https://github.com/HuxerUI/HuxerUI/releases/tag/v%s' "$TEST_LATEST"
else
  cp "$TEST_PACKAGES/${url##*/}" "$output"
fi
EOF
chmod +x "$test_root/bin/curl"
export TEST_REAL_RM=$(command -v rm)
export TEST_BACKUP_PATH_FILE="$test_root/retained-backup"
cat >"$test_root/bin/rm" <<'EOF'
#!/bin/sh
set -eu
if [ "${TEST_BACKUP_CLEANUP:-}" ] && [ "$#" -eq 2 ] && [ "$1" = -rf ]; then
  case "$2" in
  "$(dirname "$TEST_PREFIX")"/.huxerui-backup.*)
    printf '%s\n' "$2" >"$TEST_BACKUP_PATH_FILE"
    if [ "$TEST_BACKUP_CLEANUP" = partial ]; then
      "$TEST_REAL_RM" -f "$2/version"
    fi
    exit 1
    ;;
  esac
fi
exec "$TEST_REAL_RM" "$@"
EOF
chmod +x "$test_root/bin/rm"
export PATH="$test_root/bin:$PATH"

make_sdk() {
  root=$1
  sdk_version=$2
  mkdir -p "$root/bin" "$root/include/huxerui" "$root/lib/cmake/HuxerUI" \
    "$root/share/huxerui/resources/huxerui" "$root/share/huxerui/tools" \
    "$root/share/huxerui/skills/huxerui-app-development"
  touch "$root/include/huxerui/huxerui.h" "$root/lib/cmake/HuxerUI/HuxerUIConfig.cmake" \
    "$root/share/huxerui/resources/huxerui/resources.bin"
  printf '%s\n' "$sdk_version" >"$root/version"
  cat >"$root/bin/huxerui" <<'EOF'
#!/bin/sh
set -eu
if [ -f "$HUXERUI_HOME/fail-publication" ] && [ "$HUXERUI_HOME" = "$TEST_PREFIX" ]; then
  exit 1
fi
case "$*" in
  --version) printf 'huxerui %s\n' "$(cat "$HUXERUI_HOME/version")" ;;
  'sdk --help') [ ! -f "$HUXERUI_HOME/legacy-cli" ] && printf '  huxerui sdk list\n' ;;
  *) exit 2 ;;
esac
EOF
  chmod +x "$root/bin/huxerui"
}

for sdk_version in 0.1.0 0.2.0 0.3.0 0.4.0; do
  name="huxerui-sdk-$sdk_version-$host-$arch"
  make_sdk "$test_root/$name" "$sdk_version"
  if [ "$sdk_version" = 0.2.0 ]; then
    ln -s version "$test_root/$name/version-link"
  elif [ "$sdk_version" = 0.3.0 ]; then
    touch "$test_root/$name/fail-publication"
  elif [ "$sdk_version" = 0.4.0 ]; then
    ln -s /etc/passwd "$test_root/$name/unsafe-link"
  fi
  tar -czf "$TEST_PACKAGES/$name.tar.gz" -C "$test_root" "$name"
  (cd "$TEST_PACKAGES" && shasum -a 256 "$name.tar.gz" >"$name.tar.gz.sha256")
done
make_sdk "$TEST_PREFIX" 0.1.0
printf 'preserve profile\n' >"$test_root/profile"
touch "$TEST_PREFIX/local-modification"

run_update() {
  expected=$1
  message=$2
  shift 2
  result=0
  sh "$installer" --update --prefix "$TEST_PREFIX" --profile "$test_root/profile" "$@" \
    >"$test_root/output" 2>&1 || result=$?
  if [ "$result" -ne "$expected" ] || ! grep -q "$message" "$test_root/output"; then
    cat "$test_root/output"
    printf 'Expected exit %s with %s, got %s\n' "$expected" "$message" "$result" >&2
    exit 1
  fi
  [ "$(cat "$test_root/profile")" = 'preserve profile' ]
}

run_update 0 'already at' --version 0.1.0 --yes
[ ! -f "$TEST_CURL_LOG" ]
run_update 0 'update available' --check
[ "$(wc -l <"$TEST_CURL_LOG" | tr -d ' ')" = 1 ]
[ -f "$TEST_PREFIX/local-modification" ]
[ ! -e "$TEST_PREFIX.huxerui-lock" ]
export TEST_NETWORK_FAILURE=true
run_update 1 'failed to resolve' --check
unset TEST_NETWORK_FAILURE
mkdir "$TEST_PREFIX.huxerui-lock"
run_update 1 'another installer' --yes
rmdir "$TEST_PREFIX.huxerui-lock"
run_update 0 'installed at' --yes
[ "$(cat "$TEST_PREFIX/version")" = 0.2.0 ]
[ ! -f "$TEST_PREFIX/local-modification" ]
[ -L "$TEST_PREFIX/version-link" ]
export TEST_LATEST=0.1.0
run_update 0 'installed SDK is newer' --yes
[ "$(cat "$TEST_PREFIX/version")" = 0.2.0 ]
run_update 0 'installed at' --version 0.1.0 --yes
touch "$TEST_PREFIX/local-modification"
run_update 1 'SDK CLI cannot run' --version 0.3.0 --yes
[ "$(cat "$TEST_PREFIX/version")" = 0.1.0 ]
[ -f "$TEST_PREFIX/local-modification" ]
[ ! -e "$TEST_PREFIX.huxerui-lock" ]
run_update 1 'unsafe symbolic link' --version 0.4.0 --yes
[ -f "$TEST_PREFIX/local-modification" ]
for cleanup_failure in fail partial; do
  export TEST_BACKUP_CLEANUP="$cleanup_failure"
  run_update 0 'could not completely remove old SDK backup' --version 0.2.0 --yes
  [ "$(HUXERUI_HOME="$TEST_PREFIX" "$TEST_PREFIX/bin/huxerui" --version)" = 'huxerui 0.2.0' ]
  [ ! -e "$TEST_PREFIX.huxerui-lock" ]
  retained_backup=$(cat "$TEST_BACKUP_PATH_FILE")
  [ -d "$retained_backup" ]
  grep -Fq "$retained_backup" "$test_root/output"
  if [ "$cleanup_failure" = partial ]; then
    [ ! -f "$retained_backup/version" ]
  else
    [ "$(cat "$retained_backup/version")" = 0.1.0 ]
  fi
  unset TEST_BACKUP_CLEANUP
  "$TEST_REAL_RM" -rf "$retained_backup"
  run_update 0 'installed at' --version 0.1.0 --yes
done
touch "$TEST_PREFIX/local-modification"
printf 'corrupt\n' >>"$TEST_PACKAGES/huxerui-sdk-0.2.0-$host-$arch.tar.gz"
run_update 1 'checksum does not match' --version 0.2.0 --yes
[ -f "$TEST_PREFIX/local-modification" ]

cp "$cli" "$TEST_PREFIX/bin/huxerui"
export HUXERUI_HOME="$TEST_PREFIX"
current_version=$("$TEST_PREFIX/bin/huxerui" --version)
current_version=${current_version#huxerui }
export HOME="$test_root/home"
export SHELL=/bin/bash
unset _HUXERUI_SDK_PROFILE
mkdir "$HOME"
profile="$HOME/.bashrc"
printf 'export KEEP_TEST=preserved\n' >"$profile"
sh "$installer" --sdk use --prefix "$TEST_PREFIX" --version "$current_version" --profile "$profile" >"$test_root/output"
cp "$profile" "$test_root/original-profile"
export TEST_LATEST=9.0.0
"$TEST_PREFIX/bin/huxerui" sdk update --check >"$test_root/output"
grep -q 'update available' "$test_root/output"
cmp -s "$profile" "$test_root/original-profile"
if "$cli" sdk update --check >"$test_root/output" 2>&1; then
  printf 'CLI accepted a different installation\n' >&2
  exit 1
fi
grep -q 'different installations' "$test_root/output"

name="huxerui-sdk-$TEST_LATEST-$host-$arch"
make_sdk "$test_root/$name" "$TEST_LATEST"
tar -czf "$TEST_PACKAGES/$name.tar.gz" -C "$test_root" "$name"
(cd "$TEST_PACKAGES" && shasum -a 256 "$name.tar.gz" >"$name.tar.gz.sha256")
"$TEST_PREFIX/bin/huxerui" sdk update --yes >"$test_root/output"
selected="$TEST_PREFIX-versions/$TEST_LATEST"
( . "$profile"; test "$HUXERUI_HOME" = "$selected"; test "$(huxerui --version)" = "huxerui $TEST_LATEST" )
test -f "$TEST_PREFIX/local-modification"
grep -Fqx 'export KEEP_TEST=preserved' "$profile"
"$TEST_PREFIX/bin/huxerui" sdk list >"$test_root/output"
test "$(awk '$1 == "*" { print $2 }' "$test_root/output")" = "$TEST_LATEST"
grep -Fq "$current_version" "$test_root/output"

export TEST_NETWORK_FAILURE=true
"$TEST_PREFIX/bin/huxerui" sdk use "$current_version" >"$test_root/output"
( . "$profile"; test "$HUXERUI_HOME" = "$TEST_PREFIX"; test "$(huxerui --version)" = "huxerui $current_version" )
test ! -e "$HOME/.bash_profile"
"$TEST_PREFIX/bin/huxerui" sdk list >"$test_root/output"
test "$(awk '$1 == "*" { print $2 }' "$test_root/output")" = "$current_version"

custom_profile="$HOME/custom 'settings'"
sh "$installer" --sdk use --prefix "$TEST_PREFIX" --version "$current_version" \
  --profile "$custom_profile" >"$test_root/output"
(
  . "$custom_profile"
  "$TEST_PREFIX/bin/huxerui" sdk use "$TEST_LATEST" >"$test_root/output"
  "$TEST_PREFIX/bin/huxerui" sdk list >"$test_root/output"
  test "$(awk '$1 == "*" { print $2 }' "$test_root/output")" = "$TEST_LATEST"
  ( . "$custom_profile"; test "$HUXERUI_HOME" = "$selected" )
  "$TEST_PREFIX/bin/huxerui" sdk use "$current_version" >"$test_root/output"
  ( . "$custom_profile"; test "$HUXERUI_HOME" = "$TEST_PREFIX" )
)
cmp -s "$profile" "$test_root/original-profile"

if "$TEST_PREFIX/bin/huxerui" sdk use 0.3.0 --yes >"$test_root/output" 2>&1; then
  printf 'CLI selected a release without SDK management\n' >&2
  exit 1
fi
grep -q 'SDK switching starts with' "$test_root/output"
touch "$selected/legacy-cli"
if "$TEST_PREFIX/bin/huxerui" sdk use "$TEST_LATEST" >"$test_root/output" 2>&1; then
  printf 'CLI selected an SDK without the sdk command\n' >&2
  exit 1
fi
grep -q 'does not support SDK switching' "$test_root/output"
( . "$profile"; test "$HUXERUI_HOME" = "$TEST_PREFIX" )
printf 'SDK update tests passed.\n'
