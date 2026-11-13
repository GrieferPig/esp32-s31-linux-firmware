"""Fail if a consumer source checkout is reintroduced into the producer."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
assert not (root / 'source').exists(), 'producer must not contain a consumer checkout'
modules = subprocess.check_output(['git', '-C', str(root), 'config', '--file', '.gitmodules', '--get-regexp', r'submodule\..*\.url'], text=True)
urls = {line.split(' ', 1)[1] for line in modules.splitlines()}
assert urls == {
    'https://github.com/GrieferPig/linux-esp32-s31.git',
    'https://github.com/GrieferPig/opensbi-esp32-s31',
    'https://github.com/GrieferPig/u-boot-esp32-s31.git',
}, urls
for name in ('setup.sh', 'build.sh', 'checkout.sh'):
    assert 'esp32-s31-linux.git' not in (root / 'ci' / name).read_text()
assert (root / 'firmware/radio/Makefile').is_file()
assert (root / 'firmware/lp/Makefile').is_file()
print('Standalone producer sources and direct submodule boundary verified')
