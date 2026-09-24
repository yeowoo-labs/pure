#!/usr/bin/env python3
"""Run the local browser comparison with the archived implementation injected."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--cli', required=True, help='Path to Playwright CLI 0.1.21 JS entry point')
parser.add_argument('--session', default='pure-presentation')
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
if args.output.exists():
    raise SystemExit('Choose a new output file; evidence is never overwritten.')
archive = ROOT / 'results/2026-09-23-presentation/before'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
manifest = json.loads((archive / 'manifest.json').read_text())
for name, digest in manifest.items():
    if sha(archive / name) != digest:
        raise SystemExit(f'Archived source mismatch: {name}')
baseline = {name: (archive / f'web/{file}').read_text()
            for name, file in [('app', 'app.js'), ('runtime', 'runtime.mjs')]}
script = (ROOT / 'qa-presentation.js').read_text().replace('/* BASELINE_SOURCES */ null', json.dumps(baseline))
command = ['node', args.cli, f'--session={args.session}']
run = subprocess.run([*command, 'run-code', script], text=True, capture_output=True, check=True)
# Navigation can return early while the run-code function is accepting a
# beforeunload dialog. Collect the published result instead of rerunning it.
for attempt in range(30):
    result = subprocess.run([*command, 'eval', 'window.__purePresentationEvidence'],
                            text=True, capture_output=True, check=True)
    if '### Result\n' not in result.stdout:
        raise SystemExit(result.stdout + result.stderr)
    payload = result.stdout.split('### Result\n', 1)[1].split('\n### ', 1)[0].strip()
    if payload != 'undefined':
        value = json.loads(payload)
        if value.get('failed'):
            raise SystemExit(json.dumps(value, ensure_ascii=False, indent=2))
        value['sourceSha256'] = {name: sha(ROOT / name) for name in
                                ['web/runtime.mjs', 'web/app.js', 'web/output.mjs', 'web/core.wasm',
                                 'web/compiler.mjs', 'qa-presentation.js', 'qa-presentation.py']}
        value['baselineManifestSha256'] = sha(archive / 'manifest.json')
        with args.output.open('x') as output:
            json.dump(value, output, ensure_ascii=False, indent=2)
            output.write('\n')
        print(json.dumps({'checks':len(value['checks']), 'samples':[
            {k:s[k] for k in ['variant', 'elapsedMs', 'createdElements', 'snapshotCalls']}
            for s in value['samples']]}, indent=2))
        break
    time.sleep(0.2)
else:
    raise SystemExit('No finished evidence; inspect the browser session before retrying.')
