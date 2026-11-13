#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
mkdir -p work

sudo apt-get update
sudo apt-get install -y \
  bc bison build-essential ca-certificates ccache cmake cpio curl \
  device-tree-compiler file flex git gperf libffi-dev libssl-dev \
  ninja-build pkg-config python3-dev python3-libfdt python3-packaging \
  python3-pkg-resources python3-pyelftools python3-setuptools \
  python3-venv python3-yaml python3-jsonschema python3-pycryptodome \
  rsync swig unzip wget xz-utils

# The source pin owns all SDK and compiler version selections.
python3 - <<'PY' > work/versions.sh
import shlex, sys
sys.path.insert(0, 'source')
from tools.checks.versions import versions
for key, value in versions().items():
    print(f'export {key}={shlex.quote(value)}')
PY
source work/versions.sh
export IDF_PATH="$root/work/esp-idf"
export IDF_TOOLS_PATH="$root/work/idf-tools"
if [[ ! -d "$IDF_PATH/.git" ]]; then
  git init "$IDF_PATH"
  git -C "$IDF_PATH" remote add origin https://github.com/espressif/esp-idf.git
fi
git -C "$IDF_PATH" fetch --depth 1 origin "$ESP_IDF_REF"
git -C "$IDF_PATH" checkout --detach FETCH_HEAD
git -C "$IDF_PATH" submodule update --init --recursive --depth 1
"$IDF_PATH/install.sh" esp32s31

make -C source OUT_ROOT="$root/work/producer" \
  CACHE_DIR="$root/work/cache" toolchain-fetch

python3 - <<'PY' >> work/versions.sh
from pathlib import Path
import shlex
root = Path.cwd()
paths = {
    'IDF_PATH': root / 'work/esp-idf',
    'IDF_TOOLS_PATH': root / 'work/idf-tools',
    'PRODUCER_OUT': root / 'work/producer',
    'LINUX_TOOLCHAIN': root / 'work/cache/toolchains/riscv32-esp-linux-musl',
}
for key, value in paths.items():
    print(f'export {key}={shlex.quote(str(value))}')
PY
