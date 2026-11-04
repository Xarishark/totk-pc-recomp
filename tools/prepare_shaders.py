"""Prepare the variants the game met and a default variant of every shader for a build, once, before play.

The recorded answers (a shader-answers.bin the runtime writes in play, collected over several sessions and shipped as
metadata in data/shader-answers.bin) name the shaders and bind states players actually met: a code hash (the runtime's
sha256 over stage, the NVN control section, the code and c1) and the 152-byte packed answers it was translated for.
This tool reads the shaders from the extracted RomFS (tools/shader_census.py inventory), keeps the recorded variants
whose shader is in this game version, adds each shader's default variant (every texture slot a 2D texture, as the
translator's default bind state), and writes them next to astralchain.exe in the record layouts of runtime/gfx_shader.c:

    seed-programs.bin  "ACSPRG": stage, length, c1 length, code hash, control section, code, c1
    seed-answers.bin   "ACSANS": stage, code hash, input id (zero: the runtime computes it), answers, priority

Then it runs `astralchain.exe --prepare-shaders`, which translates every record into the build's shader cache, writes
shader-prepared.bin and builds every variant's stage library or compute pipeline once, so the GPU driver's own disk cache
holds them (vk_pipeline.c "stage library prebuild"). The seed files can be deleted afterwards.

A driver whose shader compiler crashes on a variant crashes on it in every run. The translations and shader-prepared.bin
are written by then, and the journal the run leaves names the variants the driver was building (vk_pipeline.c skip_init),
so only the driver step runs again (`astralchain.exe --warm-shaders`), up to RETRIES times, and skips them.

Game-derived output stays in the build directory (ignored). Needs Python 3.14 (compression.zstd), so run it in
.venv-py314 or the embeddable 3.14:

    .venv-py314\\Scripts\\python tools\\prepare_shaders.py build\\v101 [--version v101] [--answers <bin>] [--no-run]
                                                     [--romfs DIR]     # instead of build\\extracted\\<version>

--recordings turns the runtime's own recording into the shippable metadata (a zero input id and the priority byte the
prepare reads; the recording is written by play, kept in generated/ and bundled by tools/make_release.py):

    .venv-py314\\Scripts\\python tools\\prepare_shaders.py --recordings C:\\...\\aa\\shader-answers.bin --out generated
"""
import argparse
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
while not (ROOT / 'tools' / 'shader_census.py').is_file() and ROOT != ROOT.parent:
    ROOT = ROOT.parent
sys.path.insert(0, str(ROOT / 'tools'))
from shader_census import code_hash, inventory  # noqa: E402

STAGES = {'vs': 0, 'ps': 4, 'cs': 5}                       # runtime/gfx.h GFX_STAGE_*
CONTROL = 0x880                                            # runtime/gfx_shader.c CONTROL_SIZE
ANS_PACKED = 152                                           # pack_answers' size
ANS_REC = 4 + 32 + 32 + ANS_PACKED + 1                     # 221: stage, code hash, input id, answers, priority
PRG_REC = 12 + 32 + CONTROL                                # program header before code + c1
JOURNAL = 'shader-prepare-journal.bin'                     # vk_pipeline.c skip_init
RETRIES = 3
DEFAULT_PRIORITY = 2                                       # no warm-row list for this port: every variant is "rest"

BUNDLED = ROOT / 'data' / 'shader-answers.bin'             # release folder layout (tools/make_release.py)
if not BUNDLED.is_file():
    BUNDLED = ROOT / 'generated' / 'shader-answers.bin'    # dev: generated from a recording


def read_answers(path):
    """(code hash, stage, answers, priority) records of a shader-answers.bin (ACSANS)."""
    data = Path(path).read_bytes()
    if data[:8] != b'ACSANS\x00\x00':
        sys.exit(f'{path}: not an ACSANS shader-answers.bin')
    version, rec = struct.unpack_from('<II', data, 8)
    if version != 2 or rec != ANS_REC:
        sys.exit(f'{path}: ACSANS version {version}, record {rec}; expected 2, {ANS_REC}')
    return [(data[o + 4:o + 36], data[o], data[o + 68:o + 68 + ANS_PACKED], data[o + ANS_REC - 1])
            for o in range(16, len(data) - ANS_REC + 1, ANS_REC)]


def default_answer():
    """The default bind state (152 packed bytes): every texture slot a 2D texture, everything else zero. A compute
    shader's workgroup size then comes from its control section (runtime/gfx_shader.c build_request), so it needs no
    local size here."""
    a = bytearray(ANS_PACKED)
    a[:32] = b'\x04' * 32   # tex_type = 2d
    return bytes(a)


def program_record(s):
    """The ACSPRG record of a shader: (code hash, body)."""
    control, code = s['control'][:CONTROL], s['code']
    c1_off, length = struct.unpack_from('<II', control, 0x700)
    c1 = code[c1_off:length] if c1_off <= length else b''
    stage = STAGES[s['stage']]
    h = code_hash(stage, control, length, code, c1)
    return h, struct.pack('<III', stage, length, len(c1)) + h + control + code[:length] + c1


def answer_record(stage, code_hash, answers, priority):
    rec = struct.pack('<I', stage) + code_hash + bytes(32) + answers + bytes([priority])
    assert len(rec) == ANS_REC
    return rec


def driver_crashed(out):
    """astralchain.exe died while the graphics driver built variants: the journal it left names at least one."""
    p = out / JOURNAL
    data = p.read_bytes() if p.is_file() else b''
    return any(any(data[i:i + 32]) for i in range(0, len(data), 64))


def recordings(path, out):
    """Turn the runtime's shader-answers.bin (written in play) into the shippable metadata: zero input id, priority."""
    records = read_answers(path)
    body = b''.join(answer_record(stage, h, answers, DEFAULT_PRIORITY) for h, stage, answers, _ in records)
    Path(out).mkdir(parents=True, exist_ok=True)
    target = Path(out) / 'shader-answers.bin'
    target.write_bytes(b'ACSANS\x00\x00' + struct.pack('<II', 2, ANS_REC) + body)
    print(f'{target}: {len(records)} recorded variants')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('build', type=Path, nargs='?', help='build directory holding astralchain.exe (build\\v101)')
    ap.add_argument('--version', default='v101', help='build/extracted/<version>')
    ap.add_argument('--romfs', type=Path, help='RomFS directory (default build/extracted/<version>/romfs)')
    ap.add_argument('--answers', help='a shader-answers.bin of recorded variants (default data/ or generated/)')
    ap.add_argument('--no-run', action='store_true', help='write the seed files only')
    ap.add_argument('--recordings', type=Path, help='turn a play recording into shader-answers.bin in --out and exit')
    ap.add_argument('--out', type=Path, help='with --recordings: output directory (default generated/)')
    a = ap.parse_args()
    if a.recordings:
        recordings(a.recordings, a.out or ROOT / 'generated')
        return 0
    if not a.build:
        sys.exit('a build directory is required (unless --recordings)')
    t0 = time.monotonic()
    answers_path = Path(a.answers) if a.answers else BUNDLED
    recorded = read_answers(answers_path)
    extracted = ROOT / 'build' / 'extracted' / a.version
    shaders = inventory(a.romfs or extracted / 'romfs')
    if not shaders:
        sys.exit(f'no shaders found in {a.romfs or extracted / "romfs"}: extract the game files first')
    t1 = time.monotonic()
    progs, recs, covered, missing = {}, [], set(), 0
    for code_hash, stage, answers, _ in recorded:
        s = shaders.get(code_hash)
        if not s:
            missing += 1
            continue
        if code_hash not in progs:
            progs[code_hash] = program_record(s)
        recs.append((DEFAULT_PRIORITY, answer_record(stage, code_hash, answers, DEFAULT_PRIORITY)))
        covered.add(code_hash)
    for code_hash, s in shaders.items():
        if code_hash in covered:
            continue
        if code_hash not in progs:
            progs[code_hash] = program_record(s)
        recs.append((DEFAULT_PRIORITY, answer_record(STAGES[s['stage']], code_hash, default_answer(),
                                                     DEFAULT_PRIORITY)))
    recs.sort(key=lambda r: r[0])   # stable: recorded first, then the defaults, within a priority
    out = a.build.resolve()
    with open(out / 'seed-programs.bin', 'wb') as f:
        f.write(b'ACSPRG\x00\x00' + struct.pack('<II', 1, 0))
        for _, body in progs.values():
            f.write(body)
    with open(out / 'seed-answers.bin', 'wb') as f:
        f.write(b'ACSANS\x00\x00' + struct.pack('<II', 2, ANS_REC))
        for _, rec in recs:
            f.write(rec)
    n_default = len(shaders) - len(covered)
    print(f'{len(recorded)} recorded variants and {n_default} default ones: {len(recs)} variants of {len(progs)} shaders '
          f'in {a.version} ({missing} recorded shaders not in it); inventory {t1 - t0:.1f} s, seed written in '
          f'{time.monotonic() - t0:.1f} s', flush=True)
    if a.no_run:
        return 0
    exe = out / ('astralchain.exe' if os.name == 'nt' else 'astralchain')
    r = subprocess.run([str(exe), '--prepare-shaders'], cwd=str(out))
    for _ in range(RETRIES):
        if not r.returncode or not driver_crashed(out):
            break
        print(f'prepare_shaders: the graphics driver crashed while building shaders (rc={r.returncode}); building them '
              'again without the variants it was building (astralchain.exe --warm-shaders)', flush=True)
        r = subprocess.run([str(exe), '--warm-shaders'], cwd=str(out))
    print(f'astralchain.exe --prepare-shaders: rc={r.returncode}; total {time.monotonic() - t0:.1f} s')
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
