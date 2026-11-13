#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
source work/versions.sh
export ESP_TOOLS="$IDF_TOOLS_PATH/tools/riscv32-esp-elf/$ESP_ELF_RELEASE/riscv32-esp-elf"
# ESP-IDF's export script is not written for nounset.
set +u
source "$IDF_PATH/export.sh"
set -u
test -x "$ESP_TOOLS/bin/riscv32-esp-elf-gcc"
test "$(cat "$LINUX_TOOLCHAIN/.release")" = "$TOOLCHAIN_RELEASE_TAG"
"$LINUX_TOOLCHAIN/bin/riscv32-esp-linux-musl-gcc" --version
python3 tools/checks/versions.py --idf "$IDF_PATH"

args=(OUT_ROOT="$PRODUCER_OUT" CACHE_DIR="$root/work/cache"
      JOBS="${JOBS:-4}" IDF_ROOT="$IDF_TOOLS_PATH"
      IDF_EXPORT="$IDF_PATH/export.sh" ESP_TOOLS="$ESP_TOOLS"
      TOOLCHAIN_PREFIX="$LINUX_TOOLCHAIN")
make -C "$root" "${args[@]}" radio-linux-payload lp-firmware
# Binman must see the distribution's libfdt/elftools, independently of IDF's venv.
make -C "$root" "${args[@]}" HOST_PYTHON=/usr/bin/python3 \
  UBOOT_PYTHONPATH=/usr/lib/python3/dist-packages uboot

mkdir -p dist
python3 tools/build/firmware_package.py provenance \
  --root "$root" --idf "$IDF_PATH" \
  --radio-compiler "$ESP_TOOLS/bin/riscv32-esp-elf-gcc" \
  --lp-compiler "$ESP_TOOLS/bin/riscv32-esp-elf-gcc" \
  --boot-compiler "$LINUX_TOOLCHAIN/bin/riscv32-esp-linux-musl-gcc" \
  > dist/provenance.json
python3 tools/build/firmware_package.py create \
  --root "$root" --output "$PRODUCER_OUT" \
  --provenance dist/provenance.json --archive dist/s31-firmware-inputs.tar \
  > dist/firmware-inputs.json
archive_sha="$(sha256sum dist/s31-firmware-inputs.tar | cut -d ' ' -f 1)"
python3 tools/build/firmware_package.py verify \
  --root "$root" --archive dist/s31-firmware-inputs.tar --sha256 "$archive_sha" \
  > work/verification.json

cp THIRD_PARTY_NOTICES.md dist/
cp firmware/radio/RADIO_BUNDLE_LICENSES.md dist/
python3 ci/metadata.py
(cd dist && sha256sum s31-firmware-inputs.tar firmware-inputs.json \
  provenance.json release.json THIRD_PARTY_NOTICES.md RADIO_BUNDLE_LICENSES.md \
  release-notes.md > SHA256SUMS)
