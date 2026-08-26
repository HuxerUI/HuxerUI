#!/usr/bin/env bash

set -euo pipefail

build_directory=${1:-build}

ulimit -c unlimited
{
  echo "Core dump limit: $(ulimit -c)"
  echo "Core dump pattern: $(cat /proc/sys/kernel/core_pattern)"
  ctest --test-dir "${build_directory}" --output-on-failure --timeout 300
} 2>&1 | tee "${build_directory}/linux-test.log"
