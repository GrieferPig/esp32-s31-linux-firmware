# ESP32-S31 Linux firmware

Standalone producer for boot firmware, LP firmware and the relocatable radio
payload used by ESP32-S31 Linux. All producer sources and build scripts live
here. There is no dependency on the esp32-s31-linux consumer repository.

## Build

On Ubuntu 24.04 x86-64, with sudo for host dependencies:

```sh
git clone https://github.com/GrieferPig/esp32-s31-linux-firmware.git
cd esp32-s31-linux-firmware
bash ci/checkout.sh
bash ci/setup.sh
bash ci/build.sh
```

The direct submodules pin Linux radio interface headers, OpenSBI and U-Boot.
The Linux kernel is not built. ci/setup.sh installs the SDK and compilers
pinned by configs/build-versions.mk. Outputs and SDK tools live under work/;
the verified firmware archive, metadata, checksums and notices live under dist/.
Allow substantial disk space and first-build download time. No board is used.

For component builds after setup, use make radio-linux-payload, make
lp-firmware or make uboot with the paths exported in work/versions.sh and
ESP-IDF's export.sh. ci/build.sh supplies all required paths automatically.

## CI and releases

Main pushes run checks. Pull requests and manual runs also build and verify
firmware. Pushing a fw-v* tag builds and publishes an immutable public release:

```sh
git tag -a fw-v0.2.0 -m 'Standalone firmware producer'
git push origin fw-v0.2.0
```

Update producer sources or direct submodule pins here, validate, then tag a new
release. Only publishing receives contents: write. Actions are pinned by commit.
Provenance records this repository's source identity, each direct source pin,
ESP-IDF and compiler identities. It does not claim reproduction across arbitrary
host environments. See MIGRATION.md for the original source attribution.

## Consumer boundary

The consumer downloads the latest published release, checks its SHA256 and
compares packaged kernel interfaces, memory layout and LP protocol against its
own sources. It builds Linux, modules, device trees and rootfs, then links the
final radio.bin against its actual kernel. A prelinked radio.bin or kernel
module is never supplied as a reusable producer input.

Use the release notes for the exact public archive URL and SHA256. Archive
schema 1 retains historical paths as metadata for compatibility. The consumer
does not need this source checkout, ESP-IDF or the bare-metal compiler.

## Distribution status

Public hosting was requested by the owner. Packages retain engineering-only
provenance and the vendor licensing notices. Hosting does not change license
terms. CI validates build and package boundaries; hardware boot, SSH, Wi-Fi
and Bluetooth require separate target testing.
