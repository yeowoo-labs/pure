#!/usr/bin/env python3
"""Research 048 (030's steps, built): the 140 spec apps' machine instructions on one build of Pure, to hold a core
change to its price on every kind of work (not only the one it was made for). Each of research 140's time tools
(research/experiments/044-specs/*_time.c, time.c) is built against ROOT's core headers and build/libpure.a, given aup's
example module for its spec and the same data every time, and run whole under 041's icount (user-mode instructions and
cycles of the process: its C references and Pure; the C's are the same on every build, so a difference is Pure's). Each
tool checks Pure's answers before it times them and stops otherwise: a run that ends wrong is reported as such.

  specs.py ROOT OUT [SPEC ...]     appends a JSON line a spec to OUT (PURE_JIT=0: the executor, as the slow specs run)"""
import json
import os
import subprocess
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
PURE = HERE.parents[2]
AUP = Path(os.environ.get('AUP', PURE.parent / 'aup'))
VEGA = PURE.parent / 'vega'
TOOLS = PURE / 'research' / 'experiments' / '044-specs'
SPECS_DATA = PURE / 'tests' / 'specs'


def http_data(work):
    """The whole streams http_check.py writes for http_time.c (its seed 140): made once."""
    streams = work / 'http-streams.bin'
    if not streams.exists():
        subprocess.run([sys.executable, str(TOOLS / 'http_check.py'), str(streams)], check=True, capture_output=True,
                       env=dict(os.environ, AUP=str(AUP)))
    return [str(streams)]


def inflate_data(work):
    """A deterministic raw deflate stream and its original: this repository's design 030, nine times, zlib level 6."""
    original = (PURE / 'docs' / 'design' / '030-one-path.md').read_bytes() * 9
    packer = zlib.compressobj(6, zlib.DEFLATED, -15)
    stream = packer.compress(original) + packer.flush()
    (work / 'inflate.raw').write_bytes(stream)
    (work / 'inflate.original').write_bytes(original)
    return [str(work / 'inflate.raw'), str(work / 'inflate.original')]


# spec: (tool, example module, extra libraries and sources, its arguments after the module (work -> list))
SPECS = {
    'sha256': ('time.c', 'sha256', [], lambda w: ['65536', '20']),
    'json': ('json_time.c', 'json', [], lambda w: ['20']),
    'json_tree': ('json_tree_time.c', 'json_tree', ['-l:libcjson.so.1'], lambda w: ['20']),
    'json_rec': ('json_rec_time.c', 'json_tree', ['-l:libcjson.so.1'], lambda w: ['20']),
    'inflate': ('inflate_time.c', 'inflate', ['-lz'], lambda w: inflate_data(w) + ['10']),
    'dns': ('dns_time.c', 'dns', ['-lresolv'], lambda w: [str(SPECS_DATA / 'dns' / 'messages.bin'), '10']),
    'http': ('http_time.c', 'http', [], lambda w: http_data(w) + ['10']),
    'layout': ('layout_time.c', 'layout', [], lambda w: [str(SPECS_DATA / 'layout' / 'trees.bin'), '10']),
    'linebreak': ('linebreak_time.c', 'linebreak', [f'-I{VEGA / "text"}', str(VEGA / 'text' / 'linebreak.c'),
                                                    str(VEGA / 'text' / 'unicode-data.c')],
                  lambda w: [str(SPECS_DATA / 'linebreak' / 'ranges.bin'), str(SPECS_DATA / 'linebreak' / 'lines.bin'), '3']),
}


def main():
    root, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    chosen = sys.argv[3:] or list(SPECS)
    work = Path(os.environ.get('SPECS_WORK', '/dev/shm/pure-048-specs')) / root.name
    work.mkdir(parents=True, exist_ok=True)
    icount = work / 'icount'
    subprocess.run(['cc', '-O2', '-o', str(icount), str(PURE / 'research' / 'experiments' / '041-batches' / 'icount.c')], check=True)
    for spec in chosen:
        tool, example, extra, arguments = SPECS[spec]
        binary, module = work / f'{spec}-tool', work / f'{example}.pmod'
        subprocess.run([str(AUP / 'build' / 'aup'), 'build', str(AUP / 'examples' / f'{example}.aup'), '-o', str(module)], check=True)
        built = subprocess.run(['cc', '-O2', '-std=c17', f'-I{root / "core"}', str(TOOLS / tool), *[e for e in extra if not e.startswith('-l')],
                                str(root / 'build' / 'libpure.a'), *[e for e in extra if e.startswith('-l')], '-lm', '-pthread',
                                '-o', str(binary)], capture_output=True, text=True)
        line = {'spec': spec, 'root': str(root)}
        if built.returncode:
            line['built'] = False
            line['error'] = built.stderr[-400:]
        else:
            run = subprocess.run([str(icount), str(binary), str(module), *arguments(work)], capture_output=True, text=True,
                                 env=dict(os.environ, PURE_JIT='0'), timeout=1800)
            counted = [json.loads(l) for l in run.stderr.splitlines() if l.startswith('{"instructions"')]
            said = [l for l in run.stdout.splitlines() if l.startswith('{')]
            # icount answers 0 whatever its command did: a tool that measured printed its JSON line (it stops otherwise)
            line.update({'measured': bool(said), 'instructions': counted[-1]['instructions'] if counted else None,
                         'cycles': counted[-1]['cycles'] if counted else None,
                         'tool': json.loads(said[-1]) if said else None,
                         'stderr': '' if said else run.stderr[-300:]})
        with open(out, 'a') as f:
            f.write(json.dumps(line) + '\n')
        print(json.dumps({k: line.get(k) for k in ('spec', 'measured', 'instructions', 'cycles', 'stderr')}), flush=True)


if __name__ == '__main__':
    main()
