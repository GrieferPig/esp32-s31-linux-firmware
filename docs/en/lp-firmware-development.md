# LP firmware development

LP firmware uses the pinned ESP-IDF environment installed by `ci/setup.sh`. Linux loads its ELF
through remoteproc after the root filesystem becomes available.

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

## Keep firmware within its memory budget

The current ESP-IDF configuration reserves **8192 bytes (8 KiB)** for LP
firmware. LP SRAM spans `0x2E000000`–`0x2E008000`, but the entire 32 KiB is not
available to code, data and stacks:

| Region | Address range, end exclusive | Role |
|---|---|---|
| LP firmware allocation | `0x2E000000`–`0x2E002000` | Current 8 KiB build budget |
| OpenSBI suspend snapshot | `0x2E002000`–`0x2E007000` | 20 KiB reserved runtime snapshot |
| Sleep-control reservation | `0x2E007C00`–`0x2E008000` | Final 1 KiB; current structure occupies 112 bytes |

Inspect ELF load segments and the linker map when increasing code, data or
stack sizes. Do not expand into either shared reservation. See the
[build allocation](https://github.com/GrieferPig/esp32-s31-linux-firmware/blob/main/firmware/lp/sdkconfig.defaults),
[OpenSBI snapshot](https://github.com/GrieferPig/opensbi-esp32-s31/blob/v1.9-esp32-s31/platform/generic/espressif/esp32s31/services.c) and [memory map](https://grieferpig.github.io/esp32-s31-linux-docs/en/hw-reference/memory-map.html).

## Change the protocol coherently

`shared/s31_lp_protocol.h` includes Linux's
`include/linux/soc/espressif/esp32s31-lp-protocol.h`. LP publishes READY at
startup, validates requests, and writes response fields before the response
CRC. The current Linux, LP and OpenSBI sources agree on ABI 1 and the 28-word
layout.

When changing message codes, fields, states or CRC coverage, update all three
consumers together and run the existing checks from the parent root:

Run the archive tests with `python3 -m unittest discover -s tools/build/tests -v`.
Also compare the LP protocol against the directly pinned kernel and OpenSBI sources.

These checks compare source contracts; they do not execute a suspend cycle.
The [LP reference](https://grieferpig.github.io/esp32-s31-linux-docs/en/api-reference/lp-core/index.html) owns the wire-format and
field details, and [Power management](https://grieferpig.github.io/esp32-s31-linux-docs/en/api-guides/power-management.html) describes the
retention path.

## Add an LP peripheral

Before using a new register window, add its required LP peripheral-PMS access
in the remoteproc driver. The existing grant helper checks locked permissions
and verifies the written access bits; failure returns `-EACCES`. See
[permission setup](https://github.com/GrieferPig/linux-esp32-s31/blob/v6.18-esp32-s31/drivers/remoteproc/esp32s31_lp.c).

GPIO wake uses RTCIO ownership, level sampling and a configured LP GPIO
wakeup interrupt/ISR. The poll path covers the transition window; the ISR
records a GPIO reason and requests APPWR wake after `HP_ASLEEP`. Preserve the
inactive-at-ARM check and the cleanup that disables pulls, wakeup and input
before returning ownership. See [GPIO handling](https://github.com/GrieferPig/esp32-s31-linux-firmware/blob/main/firmware/lp/main/lp_core/main.c) and
[ARM setup](https://github.com/GrieferPig/esp32-s31-linux-firmware/blob/main/firmware/lp/main/lp_core/main.c).
