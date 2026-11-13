#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Synthetic package boundary tests; not a hardware or firmware validation."""
import io
import json
from pathlib import Path
import struct
import sys
import tarfile
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.build import firmware_package as fw


class PackageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root, self.out = self.base / 'src', self.base / 'out'
        for path in fw.INTERFACES + fw.NOTICES:
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(path.encode())
        for path in fw.ARTIFACTS:
            target = self.out / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(path.encode())
        elf = bytearray(52)
        elf[:7] = b'\x7fELF\x01\x01\x01'
        struct.pack_into('<HHI', elf, 16, 1, 243, 1)
        struct.pack_into('<I', elf, 36, 2)
        (self.out / 'radio/linux_radio.localized.o').write_bytes(elf)
        (self.out / 'radio/linux-radio-linked-imports.txt').write_bytes(
            (self.root / 'firmware/radio/linux-radio-linked-imports.txt').read_bytes())
        self.provenance = dict(sources={k: dict(commit='a'*40, tree='b'*40,
            dirty=False, diff_sha256='c'*64, untracked_sha256='d'*64)
            for k in ('project', 'esp-idf', 'linux', 'u-boot', 'opensbi')},
            toolchains={k: 'e' * 64 for k in ('radio', 'boot', 'lp')},
            license_mode='engineering-only')
        self.archive = self.base / 'inputs.tar'
        self.manifest = fw.create(self.root, self.out, self.archive, self.provenance)

    def verify(self):
        return fw.verify(self.archive, self.root, fw.sha(self.archive.read_bytes()))

    def rewrite(self, transform):
        with tarfile.open(self.archive, 'r:') as tar:
            entries = [(entry, tar.extractfile(entry).read()) for entry in tar]
        with tarfile.open(self.archive, 'w', format=tarfile.USTAR_FORMAT) as tar:
            for entry, data in transform(entries):
                entry.size = len(data)
                tar.addfile(entry, io.BytesIO(data))

    def test_deterministic_and_roundtrip(self):
        before = self.archive.read_bytes()
        fw.create(self.root, self.out, self.archive, self.provenance)
        self.assertEqual(before, self.archive.read_bytes())
        manifest, files = self.verify()
        self.assertEqual(manifest, self.manifest)
        destination = self.base / 'consumer'
        fw.unpack(self.archive, self.root, fw.sha(before), destination)
        self.assertEqual((destination / 'radio/linux_radio.localized.o').read_bytes(), files['radio/linux_radio.localized.o'])
        with self.assertRaises(FileExistsError):
            fw.unpack(self.archive, self.root, fw.sha(before), destination)

    def test_actual_kernel_source_override(self):
        kernel = self.base / 'linux-buildroot'
        header = 'include/linux/esp32s31-radio-control.h'
        for interface in fw.INTERFACES:
            if interface.startswith('linux-esp32-s31/'):
                relative = interface.removeprefix('linux-esp32-s31/')
                target = kernel / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((self.root / interface).read_bytes())
        archive_hash = fw.sha(self.archive.read_bytes())
        fw.verify(self.archive, self.root, archive_hash, kernel)
        (kernel / header).write_text('different interface')
        with self.assertRaisesRegex(ValueError, 'interface mismatch'):
            fw.verify(self.archive, self.root, archive_hash, kernel)

    def test_trailing_archive_rejected(self):
        with self.archive.open('ab') as stream:
            stream.write(b'unverified trailing data')
        with self.assertRaisesRegex(ValueError, 'trailing archive data'):
            self.verify()

    def test_missing_tar_terminator(self):
        with tarfile.open(self.archive, 'r:') as tar:
            entries = tar.getmembers()
        end = entries[-1].offset_data + ((entries[-1].size + 511) // 512) * 512
        self.archive.write_bytes(self.archive.read_bytes()[:end])
        with self.assertRaisesRegex(ValueError, 'terminator'):
            self.verify()

    def test_deferred_kernel_contract_is_staging_only(self):
        for interface in fw.INTERFACES:
            if interface.startswith('linux-esp32-s31/'):
                (self.root / interface).unlink()
        archive_hash = fw.sha(self.archive.read_bytes())
        staged = fw.unpack(self.archive, self.root, archive_hash,
                           self.base / 'staged', defer_kernel_contract=True)
        self.assertEqual(staged['verification']['kernel_contract'], 'deferred')
        with self.assertRaises(FileNotFoundError):
            fw.verify(self.archive, self.root, archive_hash)
        (self.root / 'shared/s31_lp_protocol.h').write_text('changed')
        with self.assertRaisesRegex(ValueError, 'mismatch'):
            fw.unpack(self.archive, self.root, archive_hash,
                      self.base / 'staged2', defer_kernel_contract=True)

    def test_unknown_member(self):
        def change(entries):
            entry = tarfile.TarInfo('images/radio.bin')
            return entries + [(entry, b'already paired stale radio')]
        self.rewrite(change)
        with self.assertRaisesRegex(ValueError, 'unexpected'):
            self.verify()

    def test_input_identity_mismatch(self):
        def change(entries):
            result = []
            for entry, data in entries:
                if entry.name == fw.MANIFEST:
                    manifest = json.loads(data)
                    manifest['input_id'] = '0' * 64
                    data = fw.canonical(manifest)
                result.append((entry, data))
            return result
        self.rewrite(change)
        with self.assertRaisesRegex(ValueError, 'input identity mismatch'):
            self.verify()

    def test_wrong_board(self):
        def change(entries):
            result = []
            for entry, data in entries:
                if entry.name == fw.MANIFEST:
                    manifest = json.loads(data)
                    manifest['contract']['board'] = 'another-board'
                    data = fw.canonical(manifest)
                result.append((entry, data))
            return result
        self.rewrite(change)
        with self.assertRaisesRegex(ValueError, 'board/layout/ISA/ABI/interface mismatch'):
            self.verify()

    def test_slot_overflow(self):
        (self.out / 'images/spl_app.bin').write_bytes(b'x' * (0xC000 + 1))
        with self.assertRaisesRegex(ValueError, 'flash slot'):
            fw.create(self.root, self.out, self.archive, self.provenance)

    def test_trusted_hash_required(self):
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            fw.verify(self.archive, self.root, '0' * 64)

    def test_interface_changes_rejected(self):
        (self.root / fw.INTERFACES[0]).write_text('changed layout')
        with self.assertRaisesRegex(ValueError, 'mismatch'):
            self.verify()

    def test_corrupt_file(self):
        self.rewrite(lambda entries: [(e, b'corrupt' if e.name == fw.ARTIFACTS[0] else d) for e,d in entries])
        with self.assertRaises(ValueError): self.verify()

    def test_duplicate(self):
        self.rewrite(lambda entries: entries + [entries[0]])
        with self.assertRaisesRegex(ValueError, 'duplicate'): self.verify()

    def test_missing(self):
        self.rewrite(lambda entries: entries[1:])
        with self.assertRaises(ValueError): self.verify()

    def test_traversal(self):
        def change(entries):
            entries[0][0].name = '../escaped'
            return entries
        self.rewrite(change)
        with self.assertRaises(ValueError): self.verify()
        self.assertFalse((self.base / 'escaped').exists())

    def test_symlink(self):
        def change(entries):
            entries[0][0].type = tarfile.SYMTYPE
            entries[0][0].linkname = '/tmp/target'
            entries[0] = (entries[0][0], b'')
            return entries
        self.rewrite(change)
        with self.assertRaises(ValueError): self.verify()

    def test_json_duplicate_key(self):
        self.rewrite(lambda entries: [(e, d.replace(b'"schema":1', b'"schema":1,"schema":1') if e.name == fw.MANIFEST else d) for e,d in entries])
        with self.assertRaisesRegex(ValueError, 'duplicate JSON'): self.verify()

    def test_executable_payload_rejected(self):
        path = self.out / 'radio/linux_radio.localized.o'
        data = bytearray(path.read_bytes())
        struct.pack_into('<H', data, 16, 2)
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'relocatable'):
            fw.create(self.root, self.out, self.archive, self.provenance)

    def test_public_release_rejected(self):
        self.provenance['license_mode'] = 'public'
        with self.assertRaisesRegex(ValueError, 'redistribution'):
            fw.create(self.root, self.out, self.archive, self.provenance)

    def test_final_kernel_pairing_not_part_of_schema(self):
        self.assertNotIn('radio.bin', '\n'.join(fw.MEMBERS))
        self.assertNotIn('kernel', self.manifest['contract'])
        self.assertEqual(self.manifest['input_id'], self.verify()[0]['input_id'])


if __name__ == '__main__': unittest.main()
