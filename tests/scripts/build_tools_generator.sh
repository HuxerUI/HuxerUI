#!/bin/sh

set -eu

source_directory=$1
test_directory=$(mktemp -d "${TMPDIR:-/tmp}/huxerui-build-tools-generator.XXXXXX")
trap 'rm -rf "$test_directory"' EXIT HUP INT TERM

dirname_command=$(command -v dirname)
mkdir_command=$(command -v mkdir)
uname_command=$(command -v uname)

create_command() {
  command_path=$1
  command_body=$2
  printf '#!/bin/sh\n%s\n' "$command_body" >"$command_path"
  chmod +x "$command_path"
}

create_path() {
  path_directory=$1
  include_ninja=$2
  include_make=$3
  mkdir -p "$path_directory"
  ln -s "$dirname_command" "$path_directory/dirname"
  ln -s "$mkdir_command" "$path_directory/mkdir"
  ln -s "$uname_command" "$path_directory/uname"
  create_command "$path_directory/cmake" 'printf "%s\n" "$*" >>"$HUXERUI_TEST_CMAKE_LOG"'
  if [ "$include_ninja" = true ]; then
    create_command "$path_directory/ninja" 'exit 0'
  fi
  if [ "$include_make" = true ]; then
    create_command "$path_directory/make" 'exit 0'
  fi
}

run_success_case() {
  case_name=$1
  expected_generator=$2
  include_ninja=$3
  include_make=$4
  explicit_generator=$5
  case_directory="$test_directory/$case_name"
  path_directory="$case_directory/bin"
  log_file="$case_directory/cmake.log"
  create_path "$path_directory" "$include_ninja" "$include_make"

  if [ -n "$explicit_generator" ]; then
    CMAKE_GENERATOR=$explicit_generator PATH=$path_directory HUXERUI_TEST_CMAKE_LOG=$log_file \
      /bin/sh "$source_directory/scripts/build_tools.sh" \
      --build-dir "$case_directory/build" --output-dir "$case_directory/output" >/dev/null
  else
    (
      unset CMAKE_GENERATOR
      PATH=$path_directory HUXERUI_TEST_CMAKE_LOG=$log_file \
        /bin/sh "$source_directory/scripts/build_tools.sh" \
        --build-dir "$case_directory/build" --output-dir "$case_directory/output" >/dev/null
    )
  fi

  grep -F -- "-G $expected_generator" "$log_file" >/dev/null
}

run_success_case explicit "Unix Makefiles" true true "Unix Makefiles"
run_success_case ninja Ninja true true ""
run_success_case make "Unix Makefiles" false true ""

failure_directory="$test_directory/unavailable"
create_path "$failure_directory/bin" false false
if (
  unset CMAKE_GENERATOR
  PATH=$failure_directory/bin HUXERUI_TEST_CMAKE_LOG=$failure_directory/cmake.log \
    /bin/sh "$source_directory/scripts/build_tools.sh" \
    --build-dir "$failure_directory/build" --output-dir "$failure_directory/output"
) >"$failure_directory/output.log" 2>&1; then
  printf 'build_tools.sh unexpectedly accepted a missing generator\n' >&2
  exit 1
fi
grep -F -- "set CMAKE_GENERATOR or install ninja or make" "$failure_directory/output.log" >/dev/null
