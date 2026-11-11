"""Intel's XeSS SDK, pinned: libxess.dll and its licence texts, for runtime/vk_xess.c (xess = 1).

    .venv\\Scripts\\python tools\\xess_sdk.py

Downloads the SDK release zip once into toolchain/release-dl/ (the cache tools/make_release.py uses), checks it against
PIN and extracts bin/libxess.dll, LICENSE.txt and third-party-programs.txt into toolchain/xess/. tools/build_game.py
then copies libxess.dll next to astralchain.exe; tools/make_release.py ships it in the release's bin/ with the licence
texts in licenses/. Nothing of the SDK goes into the repository: its headers and binaries are under Intel's licence
(the Intel Simplified Software License lets the unmodified DLL be redistributed with its notice), and vk_xess.c declares
the few types it uses itself.
"""
import hashlib
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN_ROOT = (ROOT / 'build').resolve().parent   # toolchain/ is a junction in a worktree
VERSION = '3.0.2'
PIN = (f'https://github.com/intel/xess/releases/download/v{VERSION}/XeSS_SDK_{VERSION}.zip',
       '88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990')
DEST = MAIN_ROOT / 'toolchain' / 'xess'
# member of the zip: name in DEST
FILES = {'bin/libxess.dll': 'libxess.dll', 'LICENSE.txt': 'LICENSE.txt',
         'third-party-programs.txt': 'third-party-programs.txt'}


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def extract(zip_path, dest=DEST):
    """The FILES of the pinned zip into dest; returns dest."""
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path) as z:
        for member, name in FILES.items():
            with z.open(member) as src, open(dest / name, 'wb') as dst:
                shutil.copyfileobj(src, dst)
    return dest


def fetch():
    """The pinned zip in toolchain/release-dl/, downloaded when missing or not the pinned file."""
    url, want = PIN
    dl = MAIN_ROOT / 'toolchain' / 'release-dl'
    dl.mkdir(parents=True, exist_ok=True)
    path = dl / url.rsplit('/', 1)[1]
    if path.is_file() and sha256_file(path) == want:
        return path
    print(f'download {url}')
    part = path.with_name(path.name + '.part')
    with urllib.request.urlopen(url, timeout=120) as r, open(part, 'wb') as f:
        shutil.copyfileobj(r, f)
    got = sha256_file(part)
    if got != want:
        part.unlink()
        sys.exit(f'{url}: sha256 {got}, pinned {want}')
    part.replace(path)
    return path


def main():
    dest = extract(fetch())
    print(f'{dest / "libxess.dll"}: XeSS SDK {VERSION}; tools/build_game.py copies it next to astralchain.exe')


if __name__ == '__main__':
    main()
