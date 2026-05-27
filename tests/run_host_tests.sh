#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p build/host-tests
cc -std=c11 -Wall -Wextra -Werror \
  -Icomponents/board_config/include \
  -Icomponents/irrigation_core/include \
  components/irrigation_core/irrigation_core.c \
  tests/host_core_test.c \
  -o build/host-tests/host_core_test

./build/host-tests/host_core_test
