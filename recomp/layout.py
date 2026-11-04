"""Guest address layout of the loaded modules, computed from their sizes.

Guest addresses equal host addresses, and the lifter writes absolute addresses into the generated C, so the
generator and the runtime must agree on every module base. This is the single place that decides them: the generator
calls layout() and writes the result into generated/<version>/layout.h and layout.json, which the runtime and the
build read.

Modules are placed in Horizon's load order (rtld is dropped; the host links the modules):
main, subsdk0..9, sdk. Each base is aligned to 2 MiB. Everything must stay below 2^39, the limit of the addresses
IPC buffers can encode.
"""
import json
from pathlib import Path

from nso import Nso

FIRST_BASE = 0x7000000000
ALIGN = 0x200000
ADDRESS_LIMIT = 1 << 39
ORDER = ['main'] + [f'subsdk{i}' for i in range(10)] + ['sdk']

# Astral Chain parses the RomFS image at a fixed address baked into the game binary (content manager init, 1.0.0
# f_70005693e0 and 1.0.1 f_700056e800, which pass it to the reader setup). The image must be mapped there before the
# game boots; the modules that follow (sdk) go above the end of the image. The address moved with 1.0.1 (its data
# segment starts there), so it is chosen per game version, never guessed.
ROMFS_BASES = {
    'v0': 0x7000d1b000,      # 1.0.0
    'v101': 0x7000d35000,    # 1.0.1
}
ROMFS_BASE = ROMFS_BASES['v0']


def base_of(version='v0'):
    """The fixed RomFS address of a game version tag ('v0', 'v101')."""
    try:
        return ROMFS_BASES[version]
    except KeyError:
        raise ValueError(f'no fixed RomFS address known for {version}; add it to recomp/layout.py') from None


def romfs_size(exefs):
    """Size of the extracted RomFS image, or 0 when it is not there (generator-only runs).

    The image lives at <exefs>/../romfs/romfs.bin next to the extracted ExeFS."""
    romfs = Path(exefs) / '..' / 'romfs' / 'romfs.bin'
    return romfs.stat().st_size if romfs.exists() else 0


def align_up(v, a=ALIGN):
    return (v + a - 1) & ~(a - 1)


def layout(exefs, version='v0'):
    """[(name, base, size)] for the modules present in the ExeFS directory, in load order.

    The RomFS region (base_of(version)..+size) is reserved between main and the modules that follow it."""
    exefs = Path(exefs)
    romfs_base = base_of(version)
    out, base = [], FIRST_BASE
    for name in ORDER:
        path = exefs / name
        if not path.exists():
            continue
        size = Nso(str(path)).size
        out.append((name, base, size))
        base = align_up(base + size)
        if name == 'main':
            base = max(base, align_up(romfs_base + romfs_size(exefs)))
    if base > ADDRESS_LIMIT:
        raise ValueError(f'modules end at {base:#x}, above 2^39')
    return out


def write(mods, outdir, romfs_size=0, romfs_base=ROMFS_BASE):
    """layout.json for Python tools and layout.h for the runtime."""
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / 'layout.json').write_text(json.dumps(
        [{'name': n, 'base': f'{b:#x}', 'size': f'{s:#x}'} for n, b, s in mods], indent=1) + '\n')
    lines = ['// Module layout chosen by recomp/layout.py. Generated; do not edit.', '#pragma once', '']
    lines.append(f'#define ROMFS_BASE {romfs_base:#x}ull')
    lines.append(f'#define ROMFS_SIZE {romfs_size:#x}ull')
    lines.append('')
    for n, b, s in mods:
        lines.append(f'#define MOD_{n.upper()}_BASE {b:#x}ull')
        lines.append(f'#define MOD_{n.upper()}_SIZE {s:#x}ull')
    lines += ['', f'#define MOD_COUNT {len(mods)}',
              '#define MOD_LIST(X) ' + ' '.join(f'X({n}, {n.upper()})' for n, _, _ in mods), '']
    (outdir / 'layout.h').write_text('\n'.join(lines))


def read(outdir):
    """[(name, base, size)] from a layout.json written by write()."""
    data = json.loads((Path(outdir) / 'layout.json').read_text())
    return [(m['name'], int(m['base'], 16), int(m['size'], 16)) for m in data]
