# Experimental Wi-Fi-only radio payload

The default combined radio is retained (`S31_WIFI_ONLY=0`). For a local engineering build, use `S31_WIFI_ONLY=1` on the `radio-linux-payload` target (or the radio component `linux-kbuild` target). Use the pinned toolchain and ESP-IDF checkout from `configs/build-versions.mk`. The IDF dependency build still uses the combined configuration; the final relocatable radio link excludes the four Bluetooth controller archives. Bluetooth lifecycle entry points retain the existing ABI but reject unsupported requests.

When building the kernel-paired XIP image with `tools/build/radio_image.py`, pass `--wifi-only` for the reduced payload. The packer uses a separate strict SRAM manifest. The existing default manifest is unchanged. This is a local engineering experiment, not a supported firmware-inputs release or a redistribution grant. The existing firmware-package schema has not been extended for the experimental profile; do not publish an experimental release-input archive using it.

## Validation on 2026-11-25

Pinned producer base: 15ba03651e8a26fa5972ef01d63bbc9aa814a76a. ESP-IDF: 08e0d30a74ad0bfd5a34933142b80f45619ee410. Kernel: c57745752022e03d2248d397429345a1157dc7e4. Linux 6.18.0, SMP 0-1, same kernel/module/rootfs during the hardware A/B experiment.

- Combined reference: 1,565,056 bytes, radio SRAM 34,912 bytes; SHA256 bad8768d83313ec6ed1121af7f5d5e963676038e52043e5f41ce5b335ff06d37.
- Wi-Fi-only: 740,304 bytes, radio SRAM 33,632 bytes; SHA256 e1f2f5556ff4b1f304f310f0d929e787c7df865a9e1e90a4f3e8e33e19b03d3c.
- Both booted with two cores, brought up wlan0 and completed four passive scans including one after reboot. Eight BSS per scan. Reduced profile also passed active scanning. The local test rootfs requires manual tmpfs setup and radio start.
- WPA A/B: the test AP announced TKIP as group cipher; both builds failed association. Shared driver advertises CCMP only. WPA2/CCMP-only association, DHCP, traffic and throughput remain unvalidated.
- Switching 0 -> 1 -> 0 in the same output directory succeeded. The Wi-Fi-only XIP was byte-identical to the hardware-tested image. Both new combined builds matched each other (SHA256 92c888ee18fee84ddd065b62029e783a04e7e0c284518ee8b0ef2c3afb2de87a) and had the reference size, import/export tables and IRAM size, but differed from the earlier control hash. The new combined image needs further investigation and physical regression before submission.

## Remaining binary audit

Input section estimates from the reduced linker map: libnet80211 244174 bytes; libpp 78758; libphy 23293; libcoexist 6619. These are approximate input contributions, not exact XIP or total proprietary-code sizes. The largest libnet80211 contributors include connection management, ioctl, scan, output, STA, AP, HT and HE. Investigate root dependencies before removing members; do not infer dispensability from the member name. No new Wi-Fi reductions beyond the tested Bluetooth exclusion have been claimed.

### Packaging boundary

The localized payload build records `radio/radio-profile.json`, binding the selected profile to the exact ELF SHA-256. Schema-1 firmware input archives accept only combined-radio payloads with a matching build record. Missing, experimental or stale records fail closed at archive creation; rebuild the combined payload rather than relabeling a record. Existing schema-1 archive verification remains unchanged. This local build record is provenance evidence, not a signature or an authorization to redistribute proprietary inputs.
