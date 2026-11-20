#!/usr/bin/env bash
# Runs a command once for each target of CTS_TARGETS, a comma-separated list, with `{target}` in its arguments
# replaced, and fails when any run failed; every target is run either way, so one target's failure does not hide
# another's result. The Web's compiler keeps its own cache, so the Web runs without the CMake compiler launcher.
set -u
status=0
IFS=, read -ra targets <<< "$CTS_TARGETS"
for target in "${targets[@]}"; do
  arguments=("${@//\{target\}/$target}")
  if [ "$target" = web ]; then
    env -u CMAKE_C_COMPILER_LAUNCHER -u CMAKE_CXX_COMPILER_LAUNCHER "${arguments[@]}" || status=1
  else
    "${arguments[@]}" || status=1
  fi
done
exit "$status"
