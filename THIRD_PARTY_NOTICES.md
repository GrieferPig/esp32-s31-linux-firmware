# Third-Party Notices

## Espressif ESP32-S31 Linux ports

This project contains code derived from Espressif Systems' ESP32-S31 ports:

- [esp-linux-bsp commit a77a06f03068d25799ddd719566df50207f28c5a](https://github.com/espressif/esp-linux-bsp/commit/a77a06f03068d25799ddd719566df50207f28c5a), licensed under the Apache License 2.0. A copy is provided in [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt).
- [OpenSBI commit a93bce66b102966b0612c0afb5c9872a4737fe68](https://github.com/espressif/opensbi/commit/a93bce66b102966b0612c0afb5c9872a4737fe68), licensed under the BSD 2-Clause License. See [`opensbi-esp32-s31/COPYING.BSD`](opensbi-esp32-s31/COPYING.BSD).
- [U-Boot integration/v2024.07-esp32s31](https://github.com/espressif/u-boot/tree/integration/v2024.07-esp32s31), generally licensed under GPL-2.0-or-later, subject to the SPDX identifier in each file. See [`u-boot-esp32-s31/Licenses/README`](u-boot-esp32-s31/Licenses/README).
- [Linux integration/v6.18-esp32s31](https://github.com/espressif/linux/tree/integration/v6.18-esp32s31), generally licensed under GPL-2.0-only, subject to the SPDX identifier in each file. See [`linux-esp32-s31/COPYING`](linux-esp32-s31/COPYING).

The Espressif-derived code has been modified for dual-hart SMP, hart 0 device-interrupt affinity, direct S-mode timer delivery, XIP OpenSBI, and the local radio architecture. File-level notices identify the portions derived from the BSP and OpenSBI ports.

ESP32 and Espressif are trademarks of Espressif Systems. This project is not an official Espressif product.
