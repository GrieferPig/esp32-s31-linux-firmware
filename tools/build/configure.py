#!/usr/bin/env python3
"""Small native configuration boundaries, not a build scheduler."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from toolchain_identity import identify as toolchain_identity

def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def run(argv, **kw):
    print('+', ' '.join(map(str, argv)), flush=True)
    subprocess.run(list(map(str, argv)), check=True, **kw)

def save_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        temp = path.with_suffix(path.suffix + '.tmp')
        temp.write_text(text)
        temp.replace(path)

def save(path, value):
    save_text(path, json.dumps(value, sort_keys=True, indent=2) + '\n')

def radio_exports(a):
    # Preserve the external XIP payload's kernel imports while modpost trims
    # exports. Avoid touching an unchanged list and rebuilding the kernel.
    output = subprocess.check_output([a.nm, '--undefined-only', '--format=posix',
                                      str(a.payload)], text=True)
    # Native NAPI CSD admission consumes a completion token without sleeping;
    # retain that module API before the module itself has been built.
    # Final SRAM TX copies complete partial checksums in the radio module.
    # Retain the standard helper before that module is built/modposted.
    symbols = sorted({line.split()[0] for line in output.splitlines() if line.strip()} |
                     {'try_wait_for_completion',
                      'skb_copy_and_csum_bits', '__s31_csum_copy'})
    save_text(a.output, ''.join(symbol + '\n' for symbol in symbols))

def identity(files, values):
    return {'files': {str(Path(p).resolve()): digest(p) for p in files}, 'values': values}

def same(path, obj):
    return path.exists() and json.loads(path.read_text()) == obj

def parse_config(path):
    result = {}
    for line in path.read_text().splitlines():
        if line.startswith('CONFIG_') or line.startswith('BR2_'):
            key, value = line.split('=', 1)
            result[key] = value
        elif line.startswith(('# CONFIG_', '# BR2_')) and line.endswith(' is not set'):
            result[line[2:-11]] = 'n'
    return result

def native(a):
    a.output.mkdir(parents=True, exist_ok=True)
    config_dir = 'configs'
    files = [a.source / config_dir / a.defconfig, *a.fragment, Path(a.compiler)]
    if a.ksyms_whitelist:
        if a.kind != 'linux':
            raise SystemExit('--ksyms-whitelist is only supported for Linux')
        files.append(a.ksyms_whitelist)
    obj = identity(files, [a.kind, a.cmdline, a.cross])
    obj['toolchain'] = toolchain_identity(a.compiler)
    stamp = a.output / '.s31-config.json'
    make = ['make', '-C', a.source, f'O={a.output}', 'ARCH=riscv', f'CROSS_COMPILE={a.cross}']
    if not same(stamp, obj) or not (a.output / '.config').exists():
        if stamp.exists():
            previous = json.loads(stamp.read_text())
            compiler_key = str(Path(a.compiler).resolve())
            if previous.get('toolchain') != obj['toolchain'] or previous.get('values', [])[-1:] != obj['values'][-1:]:
                run(make + ['clean'])
        initialize = not (a.output / '.config').exists()
        if initialize:
            run(make + [a.defconfig])
        if initialize and a.fragment:
            merged = parse_config(a.output / '.config')
            for fragment in a.fragment:
                merged.update(parse_config(fragment))
            if a.cmdline:
                merged['CONFIG_CMDLINE'] = json.dumps(a.cmdline)
            if a.ksyms_whitelist:
                merged['CONFIG_UNUSED_KSYMS_WHITELIST'] = json.dumps(str(a.ksyms_whitelist.resolve()))
            (a.output / '.config').write_text(''.join(f'# {k} is not set\n' if v == 'n' else f'{k}={v}\n' for k, v in merged.items()))
            run(make + ['olddefconfig'])
        elif not initialize:
            # Defconfigs/fragments are defaults, not an authority over menuconfig.
            run(make + ['olddefconfig'])
        save(stamp, obj)
    save(a.output.parent / 'reports' / f'{a.kind}-inputs.json', obj)

def idf(a):
    defaults = [a.source / 'sdkconfig.defaults', a.source / 'sdkconfig.radio.defaults']
    idf = Path(os.environ['IDF_PATH'])
    files = defaults + [a.source / 'CMakeLists.txt', a.link_list, idf / 'tools/cmake/version.cmake']
    compiler = shutil.which('riscv32-esp-elf-gcc')
    if not compiler:
        raise SystemExit('ESP-IDF compiler unavailable after export.sh')
    files.append(Path(compiler))
    obj = identity(files, [str(idf), subprocess.check_output(['git', '-C', idf, 'rev-parse', 'HEAD'], text=True).strip()])
    obj['toolchain'] = toolchain_identity(compiler)
    a.output.mkdir(parents=True, exist_ok=True)
    stamp = a.output / '.s31-config.json'
    if not same(stamp, obj) or not (a.output / 'build.ninja').exists():
        if stamp.exists() and (a.output / 'build.ninja').exists():
            old = json.loads(stamp.read_text())
            key = str(Path(compiler).resolve())
            if old.get('toolchain') != obj['toolchain']:
                run(['ninja', '-C', a.output, 'clean'])
        # Reset only when defaults/toolchain really change; no stale choices survive.
        (a.output / 'sdkconfig').unlink(missing_ok=True)
        run(['idf.py', '-C', a.source, '-B', a.output, '-D', f'SDKCONFIG={a.output}/sdkconfig', '-D', 'SDKCONFIG_DEFAULTS=' + ';'.join(map(str, defaults)), 'reconfigure'])
        save(stamp, obj)
    available = subprocess.check_output(['ninja', '-C', a.output, '-t', 'targets', 'all'], text=True)
    targets = {line.split(':', 1)[0] for line in available.splitlines()}
    requested = {line.strip() for line in a.link_list.read_text().splitlines() if line.startswith('esp-idf/') and line.endswith('.a')}
    closure = sorted(targets & requested)
    if not closure:
        raise SystemExit('No radio IDF archive targets resolved')
    run(['ninja', '-C', a.output, '-j', a.jobs, *closure])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    n = sub.add_parser('native')
    n.add_argument('--kind', choices=['uboot'], required=True)
    n.add_argument('--source', type=Path, required=True)
    n.add_argument('--output', type=Path, required=True)
    n.add_argument('--defconfig', required=True)
    n.add_argument('--compiler', required=True)
    n.add_argument('--cross', required=True)
    n.add_argument('--fragment', type=Path, action='append', default=[])
    n.set_defaults(ksyms_whitelist=None, cmdline='')
    i = sub.add_parser('idf')
    i.add_argument('--source', type=Path, required=True)
    i.add_argument('--output', type=Path, required=True)
    i.add_argument('--jobs', default='4')
    i.add_argument('--link-list', type=Path, required=True)
    r = sub.add_parser('radio-exports')
    r.add_argument('--nm', required=True)
    r.add_argument('--payload', type=Path, required=True)
    r.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    {'native': native, 'idf': idf, 'radio-exports': radio_exports}[args.command](args)

if __name__ == '__main__':
    main()
