#!/usr/bin/env python3
"""Content identity of a compiler installation, including native subtools/sysroot."""
import argparse
import hashlib
import json
import os
from pathlib import Path


def identify(compiler):
    compiler = Path(compiler).absolute()
    if not compiler.is_file():
        raise ValueError('missing toolchain compiler: ' + str(compiler))
    if not os.access(compiler, os.X_OK):
        raise ValueError('toolchain compiler is not executable: ' + str(compiler))
    wrapper = compiler.resolve()
    if wrapper.name == 'toolchain-wrapper':
        # Buildroot's bin directory mixes its compiler wrapper with unrelated
        # host programs and Python bytecode. The external-toolchain package
        # exports its actual selected compiler; never hash the mixed prefix or
        # silently fall back to it if that declaration is missing.
        declared = os.environ.get('S31_TOOLCHAIN_CC')
        if not declared:
            raise ValueError('Buildroot compiler wrapper requires S31_TOOLCHAIN_CC')
        underlying = Path(declared).absolute()
        if (underlying.resolve().name == 'toolchain-wrapper' or
                (underlying.exists() and os.path.samefile(underlying, wrapper))):
            raise ValueError('S31_TOOLCHAIN_CC must name the underlying compiler')
        identity = identify(underlying)
        selected = {'installation': identity['sha256'],
                    'underlying_argv0': underlying.name,
                    'wrapper_argv0': compiler.name,
                    'wrapper_sha256': hashlib.sha256(wrapper.read_bytes()).hexdigest()}
        return {'sha256': hashlib.sha256(json.dumps(selected, sort_keys=True).encode()).hexdigest(),
                'file_count': identity['file_count'] + 1}
    root = compiler.parent.parent
    # A fixture/single-file compiler still has an honest, bounded identity.
    paths = [compiler]
    if compiler.parent.name == 'bin':
        paths = []
        for child in sorted(root.iterdir()):
            if child.name in {'share', '.git'}:
                continue
            if child.is_dir() and not child.is_symlink():
                paths.extend(sorted(p for p in child.rglob('*') if p.is_file()))
            elif child.is_file():
                paths.append(child)
    combined = hashlib.sha256()
    count = 0
    for path in sorted(set(paths)):
        with path.open('rb') as stream:
            content = hashlib.file_digest(stream, 'sha256').hexdigest()
        combined.update(str(path.relative_to(root)).encode() + b'\0' + content.encode() + b'\n')
        count += 1
    return {'sha256': combined.hexdigest(), 'file_count': count}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', required=True)
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    text = json.dumps(identify(a.compiler), sort_keys=True) + '\n'
    if a.output:
        a.output.parent.mkdir(parents=True, exist_ok=True)
        if not a.output.exists() or a.output.read_text() != text:
            temp = a.output.with_suffix('.tmp'); temp.write_text(text); temp.replace(a.output)
    else:
        print(text.strip())
