# Developing the radio firmware

The radio firmware combines Espressif's Wi-Fi and Bluetooth libraries with the
port's OS adapters. Most code executes directly from the prelinked `radio.bin`
flash image, managed by the common Linux radio module in the rootfs.

Use this guide for changes inside `firmware/radio/`. Changes to Linux network
or Bluetooth interfaces usually belong in the kernel frontend instead; see
[Radio architecture](https://grieferpig.github.io/esp32-s31-linux-docs/en/api-reference/radio/architecture.html).

## Build the producer package

From this firmware repository, initialize the direct source pins and run the
standalone build:

```sh
bash ci/checkout.sh
bash ci/setup.sh
bash ci/build.sh
```

The verified input archive is `dist/s31-firmware-inputs.tar`. The build invokes
this repository's radio and LP sources and its pinned bootloader sources.
It does not build a Linux kernel, module, root filesystem or final radio.bin.
Publish a versioned package, then select it in the consumer's Buildroot config.
When firmware interfaces change, save the consumer kernel configuration and
follow its linux-dirclean procedure before rebuilding the matched images.

## Change an operation

Follow the existing Wi-Fi or HCI path through the frontend, Linux radio core,
and firmware entry point. Keep application-facing calls in the frontend and
hardware-library calls inside the firmware.

When adding a firmware entry point, update its export and the loader's import
or export handling as needed, then regenerate the stubs with the normal build
target. That build also regenerates `out/generated/radio-kernel-symbols.txt`
for the kernel's `CONFIG_TRIM_UNUSED_KSYMS` export retention. Do not hand-edit
the generated stubs or whitelist. Changes to structures or calling conventions
also need an ABI update on the components that exchange them.

Radio code can run from a worker or an interrupt callback. Keep interrupt work
short, and copy data that must outlive a callback into storage owned by its
consumer.

## Find the implementation

| Source | Role |
|---|---|
| `firmware/radio/radio_stack.c` | ESP-IDF operations and raw Wi-Fi frame transport |
| `firmware/radio/s31_rtos/` | Task and synchronization adapters |
| `firmware/radio/Makefile` | Link inputs and generated import stubs |
| `drivers/platform/esp32s31-radio-smode.c` in Linux | Command queue, workers, and SRAM allocation |
| `drivers/platform/esp32s31-radio-loader.c` in Linux | XIP image validation and entry-point wrappers |
| `drivers/net/wireless/espressif/esp32s31_softmac.c` in Linux | mac80211 station frontend |
| `drivers/bluetooth/hci_esp32s31.c` in Linux | Direct HCI and Linux HCI frontends |

## Check memory use

The XIP payload's writable data/BSS lives in a 40 KiB built-in kernel RAM
arena in PSRAM. Radio heap allocations, task stacks, buffers, and selected
Wi-Fi hot code consume internal SRAM. Keep both budgets within their separate
limits. Use `radio_health` for heap usage and worker state; inspect `dmesg` for
allocation failures, which do not have a dedicated health counter. The memory regions are described in
[Memory map](https://grieferpig.github.io/esp32-s31-linux-docs/en/hw-reference/memory-map.html).

## Test the update

Start with the changed mode, then check Wi-Fi and Bluetooth together. Exercise
startup, traffic, queue pressure, shutdown, and reloading the radio module.
The [HIL guide](https://grieferpig.github.io/esp32-s31-linux-docs/en/contribute/testing-hil.html) includes radio peer tests.

Publish the verified input archive from `dist/` as a versioned firmware release. See
[Releases and licensing](https://grieferpig.github.io/esp32-s31-linux-docs/en/contribute/release-and-legal.html) before distributing
firmware that includes third-party libraries.
