#!/usr/bin/env python3
"""Run actual RT backends against reviewed Metal PT references; never invent backend references."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--backend', choices=('metal', 'vulkan', 'd3d12'), required=True)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
scenes = ('pathtracer_cornell', 'pathtracer_gallery_subset')
a.out.mkdir(parents=True, exist_ok=True)
manifest = {'backend': a.backend, 'reference_backend': 'metal', 'scenes': []}
with tempfile.TemporaryDirectory(prefix='de-pt-metal-reference-') as tmp:
    refs = Path(tmp)
    destination = refs / a.backend / 'rt'
    destination.mkdir(parents=True)
    for scene in scenes:
        source = root / 'tests/golden/refs/metal/rt' / (scene + '.png')
        if not source.is_file():
            raise SystemExit('Missing reviewed actual Metal reference: ' + str(source))
        shutil.copyfile(source, destination / source.name)
        command = [str(a.exe.resolve()), '--backend', a.backend, '--scene', scene,
                   '--refs', str(refs), '--out', str(a.out.resolve())]
        result = subprocess.run(command, check=False)
        manifest['scenes'].append({'scene': scene, 'reference_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                                   'exit': result.returncode, 'command': command})
        (a.out / 'cross-backend-manifest.json').write_text(json.dumps(manifest, indent=2))
        if result.returncode:
            raise SystemExit(result.returncode)  # 77 remains unsupported, never a pass.
