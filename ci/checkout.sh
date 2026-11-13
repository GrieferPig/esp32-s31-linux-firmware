#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
git submodule update --init --depth 1 source
git -C source submodule update --init --depth 1 \
  linux-esp32-s31 opensbi-esp32-s31 u-boot-esp32-s31
