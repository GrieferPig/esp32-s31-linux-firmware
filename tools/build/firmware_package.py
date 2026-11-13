#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Engineering-only IDF producer inputs; never a kernel-paired radio.bin.

The archive hash MUST come from a trusted channel. Embedded hashes detect
corruption, not publisher authenticity. Consumers compare interface bytes with
current sources, then link the relocatable payload against their own kernel.
Only regular files in the exact schema are accepted; tar extraction is not used.
"""
from __future__ import annotations
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import subprocess
import tarfile
import tempfile
import os

SCHEMA = 1
BOARD = 'esp32s31_generic'
# The vendor closure adds extensions beyond the local radio objects' base ISA.
ISA = 'rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p2'
ABI = 'ilp32f'
LIMIT = 64 * 1024 * 1024
MANIFEST = 'firmware-inputs.json'
# Archive paths intentionally match existing out/ consumers.
ARTIFACTS = (
    'images/spl_app.bin', 'images/u-boot.itb',
    'radio/linux_radio.localized.o', 'radio/linux-radio-linked-imports.txt',
    'generated/esp32s31-radio-imports.S', 'generated/radio-kernel-symbols.txt',
    'staging/overlay/lib/firmware/esp32s31/s31-lp-core.bin',
    'staging/overlay/lib/firmware/esp32s31/s31-lp-core.elf',
    'idf-radio/sdkconfig', 'lp/sdkconfig',
)
INTERFACES = (
    'configs/esp32s31-layout.cfg', 'shared/s31_lp_protocol.h',
    'shared/s31_memory_layout.h',
    'linux-esp32-s31/include/linux/esp32s31-radio-control.h',
    'linux-esp32-s31/include/linux/esp32s31-radio.h',
    'linux-esp32-s31/drivers/platform/esp32s31-radio-internal.h',
    'linux-esp32-s31/drivers/platform/esp32s31-radio-xip.h',
    'firmware/radio/s31_rtos/s31_rtos.h',
    'firmware/radio/linux-radio-linked-imports.txt',
    'firmware/radio/wifi_sram_symbols.txt',
    'tools/build/radio_imports.py', 'tools/build/radio_image.py',
)
NOTICES = ('THIRD_PARTY_NOTICES.md', 'firmware/radio/RADIO_BUNDLE_LICENSES.md',
           'configs/build-versions.mk', 'LICENSES/Apache-2.0.txt')
MEMBERS = frozenset(ARTIFACTS) | {'source/' + p for p in INTERFACES + NOTICES}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def canonical(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':')) + '\n').encode()


def strict_json(data):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('duplicate JSON key: ' + key)
            result[key] = value
        return result
    return json.loads(data, object_pairs_hook=pairs)


def contract(root, kernel_source=None, deferred_kernel_hashes=None):
    def source(path):
        if kernel_source is not None and path.startswith('linux-esp32-s31/'):
            return Path(kernel_source) / path.removeprefix('linux-esp32-s31/')
        return Path(root) / path
    return dict(board=BOARD, isa=ISA, abi=ABI,
                interface_sha256={p: (deferred_kernel_hashes[p]
                    if deferred_kernel_hashes is not None and p.startswith('linux-esp32-s31/')
                    else sha(source(p).read_bytes())) for p in INTERFACES})


def validate_elf(data):
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('radio payload must be ELF32 little-endian')
    kind, machine, version = struct.unpack_from('<HHI', data, 16)
    flags = struct.unpack_from('<I', data, 36)[0]
    if (kind, machine, version) != (1, 243, 1) or flags & 6 != 2:
        raise ValueError('radio payload must be relocatable RISC-V ILP32F')


def validate_provenance(value):
    # Explicit producer evidence, not a claim inferred from a checkout tag.
    if not isinstance(value, dict) or set(value) != {'sources', 'toolchains', 'license_mode'}:
        raise ValueError('provenance requires sources, toolchains and license_mode')
    if value['license_mode'] != 'engineering-only':
        raise ValueError('public redistribution is not authorized by this package')
    for group in ('sources', 'toolchains'):
        entries = value[group]
        if not isinstance(entries, dict) or not entries:
            raise ValueError('missing provenance ' + group)
        if any(not isinstance(k, str) or not isinstance(v, str) or not v.strip()
               for k, v in entries.items()) and group == 'toolchains':
            raise ValueError('provenance entries must be nonempty strings')
    if not {'project', 'esp-idf', 'linux', 'u-boot', 'opensbi'} <= value['sources'].keys():
        raise ValueError('missing producer source revisions')
    for name, state in value['sources'].items():
        if not isinstance(state, dict) or set(state) != {'commit', 'tree', 'dirty', 'diff_sha256', 'untracked_sha256'}:
            raise ValueError('source identity requires commit/tree/dirty/diff/untracked: ' + name)
        if type(state['dirty']) is not bool:
            raise ValueError('source dirty flag must be boolean')
        for field in ('commit', 'tree', 'diff_sha256', 'untracked_sha256'):
            length = 40 if field in ('commit', 'tree') else 64
            if not isinstance(state[field], str) or not re.fullmatch('[0-9a-f]{%d}' % length, state[field]):
                raise ValueError('invalid source identity ' + name + ':' + field)
    if not {'radio', 'boot', 'lp'} <= value['toolchains'].keys():
        raise ValueError('missing producer toolchain identities')
    if any(not re.fullmatch(r'[0-9a-f]{64}', v) for v in value['toolchains'].values()):
        raise ValueError('toolchain identities must be installation SHA256 hashes')


def source_identity(path):
    """Capture actual producer source state, including untracked source bytes.

    Call on source checkouts only; outputs should always be out of tree.
    """
    path = Path(path)
    def git(*args):
        return subprocess.check_output(['git', '-C', str(path), *args])
    patch = git('diff', 'HEAD', '--binary', '--no-ext-diff')
    untracked = {}
    for name in git('ls-files', '--others', '--exclude-standard', '-z').split(b'\0'):
        if name:
            name = name.decode()
            entry = path / name
            untracked[name] = sha(str(entry.readlink()).encode() if entry.is_symlink() else entry.read_bytes())
    return dict(commit=git('rev-parse', 'HEAD').decode().strip(),
                tree=git('rev-parse', 'HEAD^{tree}').decode().strip(),
                dirty=bool(patch or untracked), diff_sha256=sha(patch),
                untracked_sha256=sha(canonical(untracked)))


def check_contents(files, manifest, expected):
    if set(manifest) != {'schema', 'contract', 'provenance', 'files', 'input_id'}:
        raise ValueError('unexpected manifest fields')
    if type(manifest['schema']) is not int or manifest['schema'] != SCHEMA:
        raise ValueError('unsupported schema')
    if manifest['contract'] != expected:
        raise ValueError('board/layout/ISA/ABI/interface mismatch')
    validate_provenance(manifest['provenance'])
    actual = {p: {'sha256': sha(data), 'size': len(data)} for p, data in sorted(files.items())}
    if set(files) != MEMBERS or manifest['files'] != actual:
        raise ValueError('missing, extra, corrupt or mismatched package files')
    identity = {k: v for k, v in manifest.items() if k != 'input_id'}
    if manifest['input_id'] != sha(canonical(identity)):
        raise ValueError('input identity mismatch')
    for path, digest in expected['interface_sha256'].items():
        if sha(files['source/' + path]) != digest:
            raise ValueError('interface content mismatch: ' + path)
    if files['radio/linux-radio-linked-imports.txt'] != files['source/firmware/radio/linux-radio-linked-imports.txt']:
        raise ValueError('linked import allowlist mismatch')
    validate_elf(files['radio/linux_radio.localized.o'])
    if len(files['images/spl_app.bin']) > 0xC000 or len(files['images/u-boot.itb']) > 0x50000:
        raise ValueError('boot image exceeds unchanged flash slot')
    if any(not data for data in files.values()):
        raise ValueError('empty package input')


def create(root, output, archive, provenance):
    root, output, archive = Path(root), Path(output), Path(archive)
    files = {p: (output / p).read_bytes() for p in ARTIFACTS}
    files.update({'source/' + p: (root / p).read_bytes() for p in INTERFACES + NOTICES})
    manifest = dict(schema=SCHEMA, contract=contract(root), provenance=provenance,
                    files={p: {'sha256': sha(data), 'size': len(data)} for p, data in sorted(files.items())})
    manifest['input_id'] = sha(canonical(manifest))
    check_contents(files, manifest, contract(root))
    files[MANIFEST] = canonical(manifest)
    if sum(map(len, files.values())) > LIMIT:
        raise ValueError('package exceeds size limit')
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=archive.parent, delete=False) as temp:
        temporary = Path(temp.name)
    try:
        with tarfile.open(temporary, 'w', format=tarfile.USTAR_FORMAT) as tar:
            for path, data in sorted(files.items()):
                entry = tarfile.TarInfo(path)
                entry.size, entry.mode, entry.mtime = len(data), 0o644, 0
                tar.addfile(entry, io.BytesIO(data))
        os.replace(temporary, archive)
    finally:
        temporary.unlink(missing_ok=True)
    return manifest


def _verify(archive, root, expected_sha256, kernel_source=None, defer_kernel_contract=False):
    archive = Path(archive)
    if not re.fullmatch(r'[0-9a-f]{64}', expected_sha256):
        raise ValueError('a trusted archive SHA256 is required')
    if archive.stat().st_size > LIMIT + 1024 * 1024:
        raise ValueError('archive exceeds size limit')
    raw = archive.read_bytes()
    if sha(raw) != expected_sha256:
        raise ValueError('archive SHA256 mismatch')
    files, total, next_offset = {}, 0, 0
    # Deliberately uncompressed: predictable bounds and no decompression bombs.
    with tarfile.open(fileobj=io.BytesIO(raw), mode='r:') as tar:
        for entry in tar:
            if entry.offset != next_offset or raw[entry.offset + 257:entry.offset + 263] != b'ustar\0':
                raise ValueError('only plain USTAR headers are permitted')
            next_offset = entry.offset_data + ((entry.size + 511) // 512) * 512
            if entry.name not in MEMBERS | {MANIFEST} or entry.name in files:
                raise ValueError('unexpected or duplicate archive path: ' + entry.name)
            if not entry.isfile() or entry.linkname or entry.pax_headers or entry.size < 0:
                raise ValueError('only plain regular archive members are permitted')
            total += entry.size
            if total > LIMIT:
                raise ValueError('expanded archive exceeds size limit')
            files[entry.name] = tar.extractfile(entry).read()
    if len(raw) - next_offset < 1024 or any(raw[next_offset:]):
        raise ValueError('missing tar terminator or trailing archive data')
    if MANIFEST not in files:
        raise ValueError('missing manifest')
    manifest = strict_json(files.pop(MANIFEST))
    if defer_kernel_contract and kernel_source is not None:
        raise ValueError('cannot defer and validate kernel contract together')
    deferred = manifest['contract']['interface_sha256'] if defer_kernel_contract else None
    check_contents(files, manifest, contract(root, kernel_source, deferred))
    return manifest, files


def verify(archive, root, expected_sha256, kernel_source=None):
    """Final verification: always compare every interface to current sources."""
    return _verify(archive, root, expected_sha256, kernel_source)


def unpack(archive, root, expected_sha256, destination, kernel_source=None,
           defer_kernel_contract=False):
    """Stage inputs before Linux extraction; deferral is not build validation.

    Deferred mode verifies hashes of packaged Linux headers, while all other
    interfaces still match local source. The consumer MUST subsequently call
    verify with its actual kernel_source before configuring/building Linux.
    """
    manifest, files = _verify(archive, root, expected_sha256, kernel_source,
                              defer_kernel_contract)
    destination = Path(destination)
    # A fresh directory prevents symlink races through existing descendants.
    destination.mkdir(parents=True, exist_ok=False)
    for path, data in sorted(files.items()):
        target = destination / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    (destination / MANIFEST).write_bytes(canonical(manifest))
    return dict(manifest, verification={'kernel_contract':
        'deferred' if defer_kernel_contract else 'verified'})


def capture_provenance(root, idf, radio_compiler, boot_compiler, lp_compiler):
    # The producer invokes this after builds, with outputs kept out of tree.
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.build.toolchain_identity import identify
    root = Path(root)
    return dict(sources={name: source_identity(path) for name, path in {
        'project': root, 'esp-idf': Path(idf), 'linux': root / 'linux-esp32-s31',
        'u-boot': root / 'u-boot-esp32-s31', 'opensbi': root / 'opensbi-esp32-s31'
    }.items()}, toolchains={name: identify(path)['sha256'] for name, path in {
        'radio': radio_compiler, 'boot': boot_compiler, 'lp': lp_compiler
    }.items()}, license_mode='engineering-only')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('provenance')
    p.add_argument('--root', type=Path, required=True)
    p.add_argument('--idf', type=Path, required=True)
    for name in ('radio', 'boot', 'lp'):
        p.add_argument('--' + name + '-compiler', type=Path, required=True)
    for command in ('create', 'verify', 'unpack'):
        p = sub.add_parser(command)
        p.add_argument('--root', type=Path, required=True)
        p.add_argument('--archive', type=Path, required=True)
        if command == 'create':
            p.add_argument('--output', type=Path, required=True, help='producer out/ directory')
            p.add_argument('--provenance', type=Path, required=True)
        else:
            p.add_argument('--sha256', required=True)
            p.add_argument('--kernel-source', type=Path, help='actual consumer Linux source directory')
        if command == 'unpack':
            p.add_argument('--destination', type=Path, required=True, help='new directory only')
            p.add_argument('--defer-kernel-contract', action='store_true',
                           help='staging only; requires subsequent verify --kernel-source before Linux build')
    args = parser.parse_args()
    try:
        if args.command == 'provenance':
            result = capture_provenance(args.root, args.idf, args.radio_compiler, args.boot_compiler, args.lp_compiler)
        elif args.command == 'create':
            result = create(args.root, args.output, args.archive, strict_json(args.provenance.read_bytes()))
        elif args.command == 'verify':
            result, _ = verify(args.archive, args.root, args.sha256, args.kernel_source)
        else:
            result = unpack(args.archive, args.root, args.sha256, args.destination, args.kernel_source, args.defer_kernel_contract)
        print(json.dumps(result, sort_keys=True, indent=2))
    except (OSError, ValueError, tarfile.TarError, KeyError, TypeError) as error:
        parser.exit(1, f'firmware-package: {error}\n')


if __name__ == '__main__':
    main()
