#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify ESP32-P4 rev1.3 boot components before packaging."""

import argparse
import hashlib
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("spl", type=Path)
    parser.add_argument("uboot", type=Path)
    args = parser.parse_args()
    spl = args.spl.read_bytes()
    if len(spl) < 65 or spl[:2] != bytes((0xE9, 1)):
        parser.error("expected a single-segment ESP ROM image")
    entry, = struct.unpack_from("<I", spl, 4)
    chip, _, min_rev, max_rev = struct.unpack_from("<HBHH", spl, 12)
    address, size = struct.unpack_from("<II", spl, 24)
    if (chip, min_rev, max_rev) != (0x12, 103, 103):
        parser.error("SPL image must target ESP32-P4 rev1.3")
    if entry != 0x4FF2B000 or address != entry or not 0 < size < 0xB000:
        parser.error("SPL entry or segment falls outside its reserved SRAM")
    if 32 + size >= len(spl) - 32 or spl[23] != 1:
        parser.error("SPL image has an invalid segment length or missing digest")
    if hashlib.sha256(spl[:-32]).digest() != spl[-32:]:
        parser.error("SPL image SHA-256 digest mismatch")
    uboot_size = args.uboot.stat().st_size
    if not 0 < uboot_size <= 0x2B000:
        parser.error("U-Boot proper exceeds its ESP32-P4 Linux FIT slot or is empty")
    print(f"ESP32-P4 rev1.3: ROM SPL {len(spl)} bytes, U-Boot proper {uboot_size} bytes")


if __name__ == "__main__":
    main()
