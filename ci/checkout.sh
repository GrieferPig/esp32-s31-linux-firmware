#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# These are direct upstream pins, never a checkout of esp32-s31-linux.
git submodule update --init --depth 1 \
  linux-esp32-s31 opensbi-esp32-s31 u-boot-esp32-s31
