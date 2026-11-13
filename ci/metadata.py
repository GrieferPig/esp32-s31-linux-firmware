"""Record the producer pin and provide copyable Buildroot configuration."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
dist = root / 'dist'
git = lambda *args: subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()
source_sha = git('-C', 'source', 'rev-parse', 'HEAD')
manifest = json.loads((dist / 'firmware-inputs.json').read_text())
archive_sha = hashlib.sha256((dist / 's31-firmware-inputs.tar').read_bytes()).hexdigest()
tag = os.environ.get('GITHUB_REF_NAME', 'local')
repo = os.environ.get('GITHUB_REPOSITORY', 'GrieferPig/esp32-s31-linux-firmware')
record = dict(producer_commit=git('rev-parse', 'HEAD'), source_commit=source_sha,
              input_id=manifest['input_id'], archive_sha256=archive_sha,
              workflow_run=os.environ.get('GITHUB_RUN_ID'),
              license_mode=manifest['provenance']['license_mode'])
(dist / 'release.json').write_text(json.dumps(record, indent=2) + '\n')
notes = f'''Producer firmware inputs for ESP32-S31 Linux.

Source commit: `{source_sha}`
Input identity: `{manifest['input_id']}`
Archive SHA256: `{archive_sha}`

This is an engineering-only package. See the attached licensing notices.
It contains boot firmware, LP firmware and a relocatable radio payload.
Buildroot builds the kernel/module/rootfs and links the final radio.bin.
CI package verification does not establish board boot or wireless operation.

For authenticated access to this private release:

```sh
gh release download {tag} --repo {repo} \\
  --pattern s31-firmware-inputs.tar --pattern SHA256SUMS --dir firmware-inputs
```

Set the archive URL to an absolute local file:// path and the trusted hash to:

```text
BR2_PACKAGE_S31_FIRMWARE_SHA256="{archive_sha}"
```

Use source revision `{source_sha}` as the consumer compatibility baseline.
When changing firmware inputs, follow the firmware README's linux-dirclean
procedure, preserving the custom Linux config first.
'''
(dist / 'release-notes.md').write_text(notes)
