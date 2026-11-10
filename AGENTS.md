# Astral Chain Recomp

Static recompilation of *Astral Chain* (Switch, ARM64) into a native Windows x86-64 exe with a Vulkan renderer, built
on the pipeline of the TotK Recomp project. Design: `docs/architecture.md`. Toolchain, build commands and tests: `docs/building.md`.


## Priorities

- **Performance first**: Astral Chain is a fast PlatinumGames action game. Frame-time budgets are a feature: keep the
  host paths cheap (upload, replay, present pacing), avoid per-frame allocations and full-RomFS scans, and measure
  with `AC_TRACE=perf` / the performance overlay instead of guessing.
- **Widely distributed**: the port runs on very heterogeneous PCs (integrated GPUs, the Steam Deck, old and weak CPUs, 4 GB cards), so a change must hold on the weakest of them, not only on the development machine.
- **ASTC textures are everywhere**: the game's textures are almost all ASTC (`runtime/nvn.h` formats 0x79-0x94,
  14 LDR + 14 sRGB block sizes). Sampling goes through the native ASTC path when the device supports
  textureCompressionASTC_LDR (`runtime/vk_astc.c`, `AC_ASTC=auto`), the compute decoder otherwise. Any change to the
  texture upload/sampler paths must be checked against ASTC (including sRGB) and the `--probe-astc` probe; watch for
  3D ASTC, which Vulkan cannot sample natively and the decoder handles only in 2D.

## Game content boundary

This repository is public. Game data lives outside it and every derived file stays in ignored folders.

- The user's game and key paths are in `config.local.toml` (ignored). Pass the keys file path to tools; the tools read
  it themselves. Key values stay out of your output, logs, commands and files.
- Everything extracted or generated goes under `build/` or `generated/`.
- Every file the code generator emits carries the marker defined as `GENERATED_MARKER` in `tools/check_content.py`.
- Run `python tools/check_content.py --staged` before proposing a commit. It is the same check CI runs.

## Reusing code

`refs/` holds the upstream checkouts (`tools/fetch_refs.sh` recreates it). Read-only.

- Copy only MIT, BSD, ISC, zlib or Apache-2.0 code, keep its header, and add a row to `THIRD_PARTY_NOTICES.md`.
- yuzu, suyu, OpenSmash-Ultimate and other GPL code: read for behaviour, write our own.
- `tools/vendor/` stays byte-identical to upstream; Windows changes go in `tools/extract.py`.

## Code

- Build C with MSYS2 UCRT64 Clang (`C:\msys64\ucrt64\bin`). Generated C uses GNU extensions, so `cl.exe` is out. Call
  Ninja from that directory by full path or put it first on PATH.
- Two Python environments: `.venv` (3.12) for the lifter and the build. The TotK project also used `.venv-py314` for
  scripts needing `compression.zstd`; this port's extraction runs under `.venv` with the `zstandard` package, so 3.14
  is not required.
- Generated C is never edited by hand: change the generator (`recomp/`).
- `build_game.py` never re-lifts: after `recomp/patches-v101.txt` changes (edit, pull, checkout), run `recomp/gen.py` again.
- Every lifter change passes the differential tests against Unicorn (`docs/building.md`, "Checks"). After changing
  `recomp/jumptables.py`, the jumptables check must report 0 disagreements.
- `recomp/layout.py` alone decides module bases.
- Graphics bugs: `docs/maxwell.md` lists the Maxwell behaviour the Vulkan path must match.
- Comments say what the code does and why, in words that stand on their own. Cite our code by file plus function (a
  line number may follow as a hint) and upstream code by project, file and function: `refs/` moves, so its line
  numbers rot. Commit hashes belong to the attribution of copied code.

## Writing

Code, comments, docs and commit messages are in English. Commit messages are plain sentences without type prefixes.

## Controller

The game reads the pad through `runtime/ov_input.c` (libnx `NpadState`), fed by `runtime/input.c`, which merges an SDL3
gamepad, the keyboard and the mouse. A physical controller needs no setup: hot-plug, both sticks, all buttons, analog
ZL/ZR, rumble and gyro are used when SDL exposes them. `AC_PAD_LAYOUT=auto|nintendo|xbox` picks the face-button layout,
and `AC_BIND_<ACTION>` (or the Buttons tab) rebinds an action.

Default keyboard: WASD move, arrow keys camera, E=A, Space=X, Shift=B, Q=L, R=R, Z=ZL, F=ZR, 1-4 D-pad, Tab=Plus,
M=Minus. Mouse left=Y, mouse right=ZL. `AC_INPUT=none` disables all input.

For runs without a physical controller use `AC_INPUT_SCRIPT=<file>`. Lines are `<frame> <button> [hold]` in 60 Hz game
ticks, or `@<ms> <button> [hold]` in milliseconds of the world clock (after the first present). `LSTICK` and `RSTICK`
take `<x> <y> [hold]`. Buttons: `A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK`. Example that presses
ZL+ZR on the title screen:

```text
@15000 ZL 3000
@15000 ZR 3000
```

`AC_INPUT_RECORD=<file>` writes a played session in the same format, so it can be replayed exactly. `AC_TEST_PAD=<lx>,
<ly>,<rx>,<ry>` creates an SDL virtual gamepad with fixed stick axes for testing the input path. Use a scripted
`--seconds` run plus the performance overlay to verify input changes; `--shot` saves the game texture before the ImGui
overlay, so it never shows the overlay itself.
