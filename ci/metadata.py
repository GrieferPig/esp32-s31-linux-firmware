"""Record the producer pin and provide copyable Buildroot configuration."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
dist = root / 'dist'
git = lambda *args: subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()
source_sha = git('rev-parse', 'HEAD')
kernel_sha = git('-C', 'linux-esp32-s31', 'rev-parse', 'HEAD')
manifest = json.loads((dist / 'firmware-inputs.json').read_text())
archive_sha = hashlib.sha256((dist / 's31-firmware-inputs.tar').read_bytes()).hexdigest()
tag = os.environ.get('GITHUB_REF_NAME', 'local')
repo = os.environ.get('GITHUB_REPOSITORY', 'GrieferPig/esp32-s31-linux-firmware')
record = dict(producer_commit=git('rev-parse', 'HEAD'), firmware_source_commit=source_sha, kernel_interface_commit=kernel_sha,
              input_id=manifest['input_id'], archive_sha256=archive_sha,
              workflow_run=os.environ.get('GITHUB_RUN_ID'),
              license_mode=manifest['provenance']['license_mode'])
(dist / 'release.json').write_text(json.dumps(record, indent=2) + '\n')
notes = f'''Producer firmware inputs for ESP32-S31 Linux.

Firmware source commit: `{source_sha}`
Kernel interface pin: `{kernel_sha}`
Input identity: `{manifest['input_id']}`
Archive SHA256: `{archive_sha}`

This is an engineering-only package. See the attached licensing notices.
It contains boot firmware, LP firmware and a relocatable radio payload.
Buildroot builds the kernel/module/rootfs and links the final radio.bin.
CI package verification does not establish board boot or wireless operation.

Download this public release:

```sh
gh release download {tag} --repo {repo} \\
  --pattern s31-firmware-inputs.tar --pattern SHA256SUMS --dir firmware-inputs
```

Buildroot configuration for this exact release:

```text
BR2_PACKAGE_S31_FIRMWARE_ARCHIVE="https://github.com/{repo}/releases/download/{tag}/s31-firmware-inputs.tar"
BR2_PACKAGE_S31_FIRMWARE_SHA256="{archive_sha}"
```

The consumer verifies the packaged kernel headers, memory layout and LP protocol
against its own sources. This producer does not check out the consumer repository.
When changing firmware inputs, follow the firmware README's linux-dirclean
procedure, preserving the custom Linux config first.
'''
(dist / 'release-notes.md').write_text(notes)
