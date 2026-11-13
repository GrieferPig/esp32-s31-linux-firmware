# Source migration

Firmware, shared contracts and producer helpers were imported from
GrieferPig/esp32-s31-linux at commit
18095da86df33c734c1684f3a42b7442209a0c79. Original copyright and license
notices are retained. This is an attribution record, not a build dependency.

This repository now owns the radio and LP sources, SDK configuration, OpenSBI
and U-Boot build procedures and input-archive writer. It directly pins the
separate Linux fork for radio interface headers, plus OpenSBI and U-Boot.
It never clones or invokes the esp32-s31-linux consumer repository.
The Linux tree is used for interface bytes and provenance; no Linux kernel,
module or root filesystem is built here.

The schema-1 archive retains historical source member names for compatibility
with already published consumers. tools/build/radio_image.py is a schema-1
linker reference carried as metadata; the consumer owns final kernel pairing.
