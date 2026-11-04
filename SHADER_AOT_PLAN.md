# Astral Chain AOT shader compilation (TotK model)

Goal: compile the game's Maxwell shaders ahead of play, at install time, from the player's own dump,
exactly as the TotK Recomp installer does. The collected recordings in `C:\Users\Sparrow\Desktop\aa`
(the developer's multi-hour play runs) are the source of the variant list we ship; the shader *programs*
are re-read from the player's dump at install time so no game code is distributed.

## Status: implemented

All deliverables are written and validated: `tools/shader_census.py` (1348 unique shaders, 540 VS / 738 PS / 70 CS),
`tools/prepare_shaders.py` (461 recorded + 896 default = 1357 seed variants across all 1348 shaders, byte-correct seeds),
`tools/pack_shader_cache.py` (restored, `TKSPACK1`), the `Prepare` installer step, `layout.shader_answers`,
`make_release.py` bundling, `generated/shader-answers.bin` (from the `aa` recording), and the docs. `check_content.py`
passes and `tests/installer_test.py` (34) + `tests/pack_shader_cache_test.py` (5) are green.

One follow-up before `tests/installer_gui_test.py` goes green end to end: `build/install-test` (the E2E fixture) was
built by the pre-`prepare` installer and lacks `shader-prepared.bin` and `shader-cache.pack`, so its verify run now
tries to run `prepare`. Done: installed the installer deps into `.venv-py314` (it only had `compression.zstd`), rekeyed
the fixture, and ran the resume install so only `prepare` ran. Note `Prepare.current()` checks the pack at
`shader-cache/shader-cache.pack` (inside the cache folder), not beside the exe. The full GUI suite (38 tests) is green.

## Verified state

### The runtime already implements AOT (inherited from TotK, intact)

- `astralchain.exe --prepare-shaders` -> `vk_pipeline_prepare_shaders()` -> `gfx_shader_prepare_seed()`
  -> `prepare_files()` (translates through `ac_xlate.exe` on N cores) -> `lb_warm_prepared()` (builds
  stage libraries / compute pipelines into the driver's disk cache).
- `astralchain.exe --warm-shaders` re-runs only the driver step after a crash (journal
  `shader-prepare-journal.bin`, skip list `shader-prepare-skip.txt`).
- Disk cache: `shader-cache/`, read as `shader-cache.pack` (magic `TKSPACK1`) then loose files
  (`.spv`, `.res.json`, `.use`, `in/*.in`).
- At boot the warm list reads `shader-answers.bin` (`ACSANS`) + `shader-prepared.bin` (`ACSPRP`).

### Exact record formats (`runtime/gfx_shader.c`)

| file | magic | version | record |
|---|---|---|---|
| `seed-answers.bin` | `ACSANS` | 2 | 221 B = stage(u32) + code_hash(32) + input_id(32, ignored) + answers(152) + priority(1) |
| `seed-programs.bin` | `ACSPRG` | 1 | stage(u32) + length(u32) + c1_len(u32) + code_hash(32) + control(0x880) + code[length] + c1 |
| `shader-prepared.bin` | `ACSPRP` | 1 | 36 B = stage + priority + 2 zero + input_id(32) |

- Runtime `code_hash` = `sha256( <I stage || control[:0x880] || <Q length || code[:length] || <I len(c1) || c1 )`.
- Census key (jobs.json, self-test) = `sha256(control[:0x880] || code[:length])` (inherited TotK).
- Control section: magic `0x98761234` (LE `34 12 76 98`), `length@0x704`, `c1_off@0x700`,
  `stage@0x714` (0 vs / 1 ps / 5 cs), `local@0x748` (cs). Code: header magic `0x12345678`
  (LE `78 56 34 12`), 0x30 header for vs/ps.
- `prepare_files()` reads the seed files with the *same* magics/versions as the recordings, so the
  collected files convert by copy.

### The collection (`aa/`)

Install of 1.0.1 at release `0.3.0+3d548a9` (current HEAD); `ac_xlate.id` == build
(`b7516cd5...`). 461 variants (`shader-answers.bin` 101 897 B), programs 2.7 MB, cache 3230 files /
28 MB. Golden program record: `stage 0, length 768, c1len 0, code_hash 64599b03..., control magic
34127698, code magic 78563412`.

### What commit 13188f5 ("Port the setup") removed

`installer` step `Prepare`, `tools/prepare_shaders.py`, `tools/pack_shader_cache.py`,
`tools/shader_census.py`, `tools/shader_list.py`. Survived: the runtime, `tests/pack_shader_cache_test.py`,
all `prepare.*` strings in `installer/text/en.json`, the `lz4: ... tools/shader_census.py` comment in
`tools/requirements-installer.txt`, `docs/building.md` (line 219 cites `shader_census.py`), and the
`--probe-shader` / `--probe-vkexec` probes that look for `build/<ver>/shaders/jobs.json`.

### Content boundary

`seed-programs.bin` is game shader code and must never be shipped. We ship only a metadata list
(stage + code_hash + bind-state answers + priority), and the census re-reads the programs from the
player's dump at install time.

## The one real unknown: the offline census (PKZ -> DAT/DTT -> VSO/PSO/CSO)

No AC archive parser exists in `refs/`: `nxnative` has only the TotK profile (`0100F2C0115B6000`),
`d2r-mobile` only TotK's `shader_map.py`. The AC shaders are not loose RomFS files (no `shader`/`.dat`/`.dtt`
path in `index.json`); they are inside the `.pkz` archives.

Confirmed PKZ facts (read from `romfs.bin`):
- magic `"pkzl"`, version `0x10000`.
- header 0x20: `magic(4) version(4) total_size(4) 0(4) count(4) table_off=0x20(4) data_off=0x98(4) 0(4)`.
- entry table at 0x20, `count x 0x20` bytes; each entry ~ `{ name_off(u32), 0(u32), offset(u64), size(u64), size(u64) }`.
- name table at 0x98: `"ZStandard"`, `"autoshader_eff.dat"`, `"autoshader_effInfo.bxm"`, ... => zstd,
  shaders in `.dat` containers.

## Deliverables

1. `tools/shader_census.py` (AC) - same CLI/output contract as the deleted TotK one (keeps the runtime
   probes and docs true), AC inventory (PKZ -> DAT/DTT -> VSO/PSO/CSO -> control+code):
   - reads `build/extracted/<version>/romfs/{romfs.bin,index.json}` (+ `exefs/` NSO check);
   - writes `build/<version>/shaders/{code/<key>.code, jobs.json}` and with `--translate` `spv/` + `failures.txt`.
   - zstd via `compression.zstd` (Python 3.14 stdlib, present in the release 3.14.9).
2. `tools/prepare_shaders.py` (AC) - reads the variant list (metadata `data/shader_answers.bin`), calls
   `shader_census.inventory()` on the player's romfs, joins by `code_hash`, writes the seeds, runs
   `astralchain.exe --prepare-shaders` with the driver-crash retry loop, deletes the seeds.
   **Exhaustive coverage**: the seed set is the recorded variants *plus* one default variant per shader
   (every texture slot a 2D texture, float vertex inputs / colour outputs), so every shader in the game is
   precompiled at least in its default bind state - the exact TotK default-variant rule.
3. `tools/pack_shader_cache.py` - restored verbatim from the baseline (rename totk->astralchain only).
4. `installer/steps.py` - restore the `Prepare` step (shaders only), register in `all_steps()`.
5. `installer/layout.py` + `tools/make_release.py` - `shader_answers` path; ship the metadata list and the
   three tools; never ship program binaries.
6. `generated/` (ignored) - the collected artifact: `shader_answers.bin` (metadata, shipped), and
   `shader-programs.bin` + `shader-cache/` as the golden reference only.
7. `docs/installer.md`, `docs/building.md`, tests.

## Execution order

1. Spike: decompress a pkz entry, find VSO/PSO/CSO objects, extract control+code, match one golden
   `code_hash` from `aa/shader-answers.bin`.
2. Complete `tools/shader_census.py`; all 461 listed code_hashes found in the dump.
3. `tools/pack_shader_cache.py` (test green).
4. `tools/prepare_shaders.py` (manual `--prepare-shaders` on a test install).
5. Installer `Prepare` step + layout + make_release + docs + tests, end to end.
6. `python tools/check_content.py --staged` before committing.

## Risks

- Exact PKZ entry semantics (offset absolute/relative, comp/decomp order) and DAT/DTT -> VSO object
  layout: covered by the golden test (nothing guessed without validation).
- Shaders possibly also in ExeFS (`main`/`sdk`): check in the spike; reuse `nso_image()` if so.
- Coverage: exhaustive - recorded variants + one default variant per shader. The census must inventory
  *every* shader program (not just the 461 the list names), and prepare adds the default variant per shader.
