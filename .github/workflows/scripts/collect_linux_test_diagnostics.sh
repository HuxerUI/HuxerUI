#!/usr/bin/env bash

set +e

build_directory=${1:-build}
diagnostic_directory=${2:?diagnostic directory is required}
workspace_directory=${3:?workspace directory is required}

mkdir -p "${diagnostic_directory}"
cp "${build_directory}/linux-test.log" "${diagnostic_directory}/ctest.log" 2>/dev/null
cp "${build_directory}/bin/huxerui_tests" "${diagnostic_directory}/huxerui_tests" 2>/dev/null
{
  uname -a
  printf 'core limit: '
  ulimit -c
  printf 'core pattern: '
  cat /proc/sys/kernel/core_pattern
  printf 'compiler: '
  "${CXX}" --version | head -n 1
} >"${diagnostic_directory}/environment.txt" 2>&1

core_index=0
while IFS= read -r -d '' core_file; do
  core_index=$((core_index + 1))
  copied_core="${diagnostic_directory}/core-${core_index}"
  cp "${core_file}" "${copied_core}"
  if command -v gdb >/dev/null 2>&1 && [[ -x "${build_directory}/bin/huxerui_tests" ]]; then
    gdb --batch \
      -ex "set pagination off" \
      -ex "thread apply all backtrace full" \
      "${build_directory}/bin/huxerui_tests" "${copied_core}" \
      >"${diagnostic_directory}/core-${core_index}-backtrace.txt" 2>&1
  fi
done < <(
  find "${workspace_directory}" -maxdepth 5 -type f \
    \( -name core -o -name 'core.[0-9]*' -o -name 'core.huxerui_tests*' \) -print0
)

seed="$(sed -n 's/^.*Randomness seeded to: //p' "${build_directory}/linux-test.log" 2>/dev/null | tail -n 1)"
if [[ -n "${seed}" ]] && command -v gdb >/dev/null 2>&1 && \
    [[ -x "${build_directory}/bin/huxerui_tests" ]]; then
  timeout --signal=INT --kill-after=30s 300s gdb --batch \
    -ex "set pagination off" \
    -ex "set confirm off" \
    -ex "run" \
    -ex "thread apply all backtrace full" \
    -ex "generate-core-file ${diagnostic_directory}/huxerui_tests-reproduced.core" \
    --args "${build_directory}/bin/huxerui_tests" --rng-seed "${seed}" \
    >"${diagnostic_directory}/gdb-rerun.txt" 2>&1
fi

exit 0
