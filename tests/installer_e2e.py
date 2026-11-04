"""End-to-end run of the installer in dev mode with the game files named in config.local.toml.

Reads input.base, input.update and input.keys, and passes them to `python -m installer` started the way the release
launcher starts it: with this checkout on sys.path (the embeddable Python of build/py314-embed when it exists, this
interpreter otherwise). The paths are never printed. Events go to <events> (JSON lines); stdout gets each step's state
and time, warnings and errors.

    python tests/installer_e2e.py --dest build/install-test --lang en-US [--events FILE] [--no-inputs] [-- extra args]

--no-inputs leaves the game files out (resume mode on an existing folder).
"""
import argparse
import json
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EMBED = ROOT / 'build' / 'py314-embed' / 'py' / 'python.exe'
BOOT = ('import runpy, sys; sys.path.insert(0, sys.argv.pop(1)); '
        "runpy.run_module('installer', run_name='__main__', alter_sys=True)")


def game_inputs():
    config = tomllib.loads((ROOT / 'config.local.toml').read_text(encoding='utf-8'))['input']
    return ['--base', config['base'], '--update', config['update'], '--keys', config['keys']]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--dest', type=Path, required=True)
    ap.add_argument('--lang')
    ap.add_argument('--events', type=Path)
    ap.add_argument('--no-inputs', action='store_true')
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    python = EMBED if EMBED.is_file() else Path(sys.executable)
    command = [str(python), '-c', BOOT, str(ROOT), '--dest', str(a.dest.resolve())]
    if a.lang:
        command += ['--lang', a.lang]
    if not a.no_inputs:
        command += game_inputs()
    command += a.extra
    events = a.events or a.dest.resolve().parent / f'{a.dest.name}-events.jsonl'
    with open(events, 'w', encoding='utf-8') as log:
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, text=True, encoding='utf-8', cwd=ROOT)
        for line in proc.stdout:
            log.write(line)
            log.flush()
            try:
                e = json.loads(line)
            except ValueError:
                print('non-JSON line on stdout:', line.rstrip())
                continue
            if e['type'] == 'step' and e['state'] != 'started':
                extra = f'{e["seconds"]:.1f} s' if 'seconds' in e else e.get('reason', '')
                print(f'[{e["t"]:8.1f}] {e["step"]:8} {e["state"]} {extra}', flush=True)
            elif e['type'] == 'step':
                print(f'[{e["t"]:8.1f}] {e["step"]:8} started', flush=True)
            elif e['type'] in ('warning', 'error', 'plan', 'end'):
                print(f'[{e["t"]:8.1f}] {json.dumps({k: v for k, v in e.items() if k != "t"})}', flush=True)
            elif e['type'] == 'start':
                print(f'[{e["t"]:8.1f}] start mode={e["mode"]} release={e["release"]}', flush=True)
        return proc.wait()


if __name__ == '__main__':
    sys.exit(main())
