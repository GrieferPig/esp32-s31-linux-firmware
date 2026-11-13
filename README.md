# ESP32-S31 Linux firmware

Builds and hosts versioned producer inputs for the ESP32-S31 Linux Buildroot
workflow. The source submodule pins the main project's firmware, bootloader,
interfaces and dependency versions.

The archive includes SPL/U-Boot (with OpenSBI), LP firmware, the relocatable
radio payload, generated imports, SDK configurations and provenance metadata.
Buildroot builds its own kernel, module, device trees and root filesystem and
links radio.bin against that kernel. A final radio.bin or old kernel module is
never reused as a producer input.

## Build

On Ubuntu 24.04 x86-64, with sudo available for host packages:

```sh
git clone --recurse-submodules https://github.com/GrieferPig/esp32-s31-linux-firmware.git
cd esp32-s31-linux-firmware
bash ci/checkout.sh
bash ci/setup.sh
bash ci/build.sh
```

Allow substantial disk space and first-build download time. SDK, toolchains and
outputs live under work/. Deliverables are under dist/. No board is accessed.
The host distribution and IDF-managed Python environment are not a fully locked
container; provenance records source and compiler identities without claiming
bit-for-bit reproduction across arbitrary hosts.

## CI and releases

Main pushes run script checks and package regression tests. Pull requests and
manual workflow runs also build and verify firmware. A fw-v* tag builds and
publishes the checked package to a private GitHub Release:

```sh
git tag -a fw-v0.1.0 -m 'Initial firmware inputs'
git push origin fw-v0.1.0
```

Use a new version for every release; never overwrite assets. Only the release
job receives contents: write. Actions are pinned by commit. Both artifact
creation and release publishing require private repository visibility.

To update firmware, update source to a reviewed main-project commit, commit the
submodule pin here, validate a manual build, then tag the new producer commit.
Release metadata records both producer and main-project commits and input ID.

## Consume a private release

```sh
gh release download fw-v0.1.0 \
  --repo GrieferPig/esp32-s31-linux-firmware \
  --pattern s31-firmware-inputs.tar --pattern SHA256SUMS \
  --dir firmware-inputs
sha256sum firmware-inputs/s31-firmware-inputs.tar
```

Compare the digest with the release notes or reviewed release.json. In the
consumer's Buildroot menuconfig, set BR2_PACKAGE_S31_FIRMWARE_ARCHIVE to the
absolute file:// URL and BR2_PACKAGE_S31_FIRMWARE_SHA256 to that digest. No
GitHub credential belongs in a defconfig. A future authorized public release
can use a versioned GitHub release download URL directly.

Use the source commit recorded in the release as the compatibility baseline.
When replacing firmware inputs, save the custom Linux .config, select it as
BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE, run make linux-dirclean, then make using
the same output directory and BR2_EXTERNAL settings. The consumer checks the
actual kernel interfaces and refuses stale inputs.

## Distribution status

Engineering-only. The upstream packager and radio licensing notice do not
authorize public redistribution. Review the vendor grants and provide exact
corresponding sources before changing this policy. Private hosting does not
itself grant distribution rights. See the notices included in each archive.
Host build and archive checks do not establish hardware boot, SSH, Wi-Fi or
Bluetooth operation.
