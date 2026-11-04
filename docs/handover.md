# Handover — Astral Chain static recompilation

State of the port for whoever picks it up next. Everything below was measured on this machine against the user's own
dump of the base game. Game data, keys and every extracted or generated file stay outside git (`build/`, `generated/`);
`AGENTS.md` holds the content rules and the priorities.

The user manages git and commits in plain English sentences. Run `python tools/check_content.py --staged` before
proposing a commit: silence means OK.

---

## 1. Priority 1 — AOT shaders

**This is the first thing to do.** The port is playable, the setup ships, and the one thing a player feels is that
shaders are translated while they play: the first minutes of every new area stutter. TotK solved this by preparing the
variants at install time; this port has the runtime half of that pipeline and is missing the tools around it.

### 1.1 What already works

- **Translator**: `build/xlate/ac_xlate.exe` (+ `ac_xlate.id`), built by `tools/build_xlate.py` (Ryujinx 1.3.2, four
  MacNativeGames patches and seven of ours in `tools/xlate/patches/`, NativeAOT). The setup copies it next to the game
  (`installer/steps.py` `Compile.run`); without it nothing is drawn.
- **On-demand translation and cache**: `runtime/gfx_shader.c` starts `ac_xlate --serve` children and caches `<hash>.spv`,
  `<hash>.res.json`, `<hash>.use` and `in/<hash>.in` under `shader-cache/`. A 15 s title-screen run leaves about 24
  shaders (96 files).
- **Pipeline cache and keys**: `pipeline-cache.bin` (the driver's disk cache) and `pipeline-keys.bin` (recorded pipeline
  keys, replayed by the boot prewarm — `runtime/vk_pipeline.c` `keys_open`, `prewarm`).
- **Recorded shaders**: `shader-answers.bin` ("ACSANS") and `shader-programs.bin` ("ACSPRG"), written next to
  `pipeline-keys.bin` by play. They hold exactly the (program, answers) pairs the game asked for, with the real input id
  — not a census guess.
- **Preparation hooks**: `--prepare-shaders` (translate a seed, write `shader-prepared.bin` "ACSPRP", then build every
  variant's stage library or compute pipeline into the driver cache), `--warm-shaders` (rebuild them from the cache
  without translating, after a driver update), and `gfx_shader_prepare_recorded()` behind `AC_TEST_PREPARE_SHADERS=1`
  (prepare the recorded pairs at boot — the development shortcut, not the final pipeline).

### 1.2 Fix the recording first

Recording does not always start, and nothing works reliably until it does.

`runtime/vk_pipeline.c` `keys_open()` calls `gfx_shader_record_at()` and returns early when `A.on` is 0; `A.on` is only
set by `vk_pipeline_async_enable()`. Depending on the init order, a run then records **nothing**: the log has no
`gfx_shader: recording answers met in play to …` line and no `shader-answers.bin` is written. Reproduced on a fresh
install: the first launch wrote nothing, the next one recorded normally.

The recording call does not need pipeline keys. Move it out of that gate (call it from `vk_pipeline_async_enable()` as
well, or before the `A.on` check) and confirm the log line and the two files appear on the **first** run of a fresh
install.

### 1.3 What to build, in order

1. **Seed converter — `tools/prepare_shaders.py` (the missing piece).** Read `shader-answers.bin` +
   `shader-programs.bin`, write `seed-answers.bin` + `seed-programs.bin` with the same record layouts, a stable priority
   in the pad byte (0 title, 1 warm-up, 2 rest) and a zero input id (the translator derives the real one). Dedupe by
   (stage, code hash, input id). The formats are already fixed by the runtime (below); this is about thirty lines.
   After it, `--prepare-shaders` works with a harvested seed.
2. **Cache packer — `tools/pack_shader_cache.py` (optional, startup speed).** The runtime already reads
   `shader-cache.pack` (48-byte entries) and falls back to loose files; folding the four files per shader into one pack
   saves the first frames of a fresh install. Port it for packaging only, never for development.
3. **Installer step — `prepare`.** Re-add the step removed for 0.1.0 (`installer/steps.py`): run the two tools into the
   install folder, then `astralchain.exe --prepare-shaders` and `--warm-shaders`, and record a key over the extraction
   and the runtime (`runtime_key`, as the old TotK step did). It must be skippable when the runtime did not change: a
   rebuild for the mods' patches keeps the prepared shaders.
4. **Wider source, only if wanted.** MacNativeGames' `shader_census.py` / `prepare_shader_seed.py` (MIT, under `refs/`)
   extract every Maxwell shader from the game's archives. More work, but it covers content nobody played yet.

### 1.4 Record layouts (`runtime/gfx_shader.c`)

```text
shader-answers.bin   "ACSANS", u32 version, u32 rec_size; records:
    u32 stage, u8 code_hash[32], u8 input_id[32], u8 packed_answers[147], u8 priority_pad
shader-programs.bin  "ACSPRG", u32 version, u32 0; records:
    u32 stage, u32 length, u32 c1_length, u8 code_hash[32],
    u8 control[0x880], u8 code[length], u8 c1[c1_length]
shader-prepared.bin  "ACSPRP", u32 version, u32 rec_size(36); records:
    u8 stage, u8 priority, u8 pad[2], u8 input_id[32]
```

A seed record carries a zero input id and the priority in the pad byte; `gfx_shader_prepare_seed` translates it, derives
the real input id and stores the prepared list.

### 1.5 Harvesting is playtime, not a script

- TotK ships ~29 600 prepared variants (its `--warm-shaders` replays 29 587 in 1.8 s), harvested by its community over
  whole playthroughs.
- This port records ~32 variants in 15 s at the title screen. A few hours of play covers the menus, the common effects
  and the areas played, and removes most of the stutter. Whatever the seed lacks still translates on demand and stays in
  the cache, so a partial seed is a real gain, not an all-or-nothing switch.
- Record on a fresh install, then grow the same files: `rec_append` only adds new records, and a torn tail is cut at
  the next start.

### 1.6 Acceptance

On a **fresh** install folder: `--prepare-shaders`, then `--warm-shaders`, then launch with `AC_TRACE=perf` and the
overlay. The first five minutes must translate nothing (`shader translation` stays at 0.0 in the frame lines), the
preparation must not crash the driver, and the log must show the prepared variant count. `runtime/vk_pipeline.c`
journals the variants a crashing driver refused (`shader-prepare-skip.txt`) and the next preparation skips them: keep
that resumability.

Never commit `shader-cache/`, `pipeline-cache.bin`, `pipeline-keys.bin`, `shader-*.bin`, `seed-*.bin` or
`build/xlate/`: they are game-derived or ignored.

---

## 2. Snapshot

- `HEAD` = `13188f5` ("Port the setup to this port: it builds the base game 1.0.0 alone"), 18 commits, working tree
  clean, `docs/handover.md` tracked.
- Remote `origin` = `https://github.com/co-esquie/astral-chain-pc-recomp.git` (GitHub redirects to
  `github.com/captainj-sparrow/astral-chain-pc-recomp`); the local branch is `master`, pushed as `main`. `origin/main`
  is at `87ceb2e`, so `13188f5` is **not pushed yet**.
- Author and committer on every commit: `CaptainJSparrow <223753297+co-esquie@users.noreply.github.com>`.
- `Eglantine/` is gone (it was a submodule; the folder and the gitlink were removed, and its section left `AGENTS.md`).
  Read GPL emulators for behaviour only, write our own code.
- A **release zip exists and was tested end to end**: `build\release\Astral Chain-Setup-0.1.0-windows.zip`
  (132.5 MiB, 483 MiB unpacked). It installs in ~65 s and the installed game boots (`boot: passed`). It is not
  published; nothing was pushed to GitHub Releases.
- What ships: the **1.0.1 update**, merged from the player's base game 1.0.0, Windows x64, Vulkan, the game's own 30 fps tick.

---

## 3. What works

- Boot through `nn::init::Start`, module constructors, the entry point, the first NVN calls.
- RomFS mount, directory enumeration, `root.pkz`, `st*.pkz`, `ui`, `um`, `wet`, `wp`, `core`, the sound archives.
- Shader lookup in `shader2.dat`, VSO/PSO validation, translation through `ac_xlate`.
- Title screen, `ZL + ZR` prompt, main menu, LOAD DATA, New Game, the save system, scripted input.
- **1.0.1**: built from the update NSP, merges its BKTR RomFS with the base game, boots to the opening scene
  (`boot: passed`, 1197 frames, no unimplemented NVN calls, 1600x900 shot).
- Vulkan rendering: FSR 1 upscale from 1600x900 to the window, ~30 fps present loop, no validation errors, no shader
  failures, no skipped draws in the last runs.
- The opening cutscene and the first gameplay scene (police HQ), in the language the ini selects.
- **Audio**: `audout:u` with a full `IAudioOut` session over an SDL3 stream (music, effects, voices); the `hwopus`
  service and its decoder over libopus are implemented but not exercised yet (section 4).
- **Save data** through the host layer: mount, atomic commit, recovery of an interrupted commit, `CommitSaveData`.
- Anisotropic filtering up to 16x, AMD FSR 1 upscaling and emulator-style 3D render scaling (`AC_SSAA`).
- In-game menu (F1), performance overlay (F10 / L3+R3), four UI languages.
- **The setup**: check, extract, lift, compile, cleanup on the player's machine, from the player's own base game.

Measured on the installed 0.1.0 folder: 1346 frames in 46.2 s (mean 29.2 fps, 1% low 18.0), GPU around 25% in the
overlay, memory census 2586.8 MiB of imported pools at frame 600.

## 4. Not verified, not working

- **Play coverage stops at the opening.** The longest verified run is a 45 s scripted New Game that reaches the police
  HQ cutscene. Nothing past it was played; expect missing services or render issues there.
- **1.0.1 is the target.** The installer takes the base game (1.0.0) and the update NSP (1.0.1), merges the update's
  BKTR RomFS section and builds from the update's ExeFS. The base title id is `01007300020FA000`, the update's
  `01007300020FA800`.
- **60 fps is on by default** (`AC_FPS`, menu 30/60): `recomp/patches-v101.txt` set `fps60` halves the game's frame
  time, and `runtime/frame_rate.c` `cutscene_rate` keeps cutscenes at 30. Measured 59.5 fps mean in play, 29.9 with the
  switch at 30.
- **AOT shaders are not done** (section 1): shaders translate during play.
- **`hwopus` decoding is unexercised.** The service and the decoder exist, but the game has only asked for work-buffer
  sizes so far; no decoder was ever opened, so the Opus path never ran.
- **Shared system fonts are absent.** `nn::pl::GetSharedFont` skips missing types; the game's own fonts cover the UI,
  Chinese/Korean system-font text would not render.
- **`fsp-srv` serves only the access-log commands.** Saves work because `runtime/fs_host.c` overrides the SDK entry
  points; there is no `IFileSystem` object.
- **`nifm:u`, `prepo:u`, `nfp:user`, parts of `account`/`friends`** are local compatibility stubs: no network, no
  telemetry, no NFC.

---

## 5. After AOT, in order

1. **Play further.** The next missing service or black scene is the work; keep a representative scripted slice as a
   regression run so a fix cannot silently break it.
2. **Full `fsp-srv` save data** (section 10): `IFileSystem` with `Commit`, in case a later path stops going through the
   host override.
3. **Support services**, in this order: `nfp:user`/amiibo, the `account`/`friends` commands a longer run reaches,
   `prepo:u`/`nifm:u` kept local, shared fonts.
4. Optional, later: frame rates above 60, a Linux release.

The user asked for real implementations, not stubs that report success: a service that returns success without doing
the work leaves the game waiting for data it never gets.

Definition of done for this handover:

- A fresh install does not translate shaders during the first minutes of play.
- Title, menu and New Game still reached with the same controller flow.
- `--seconds` runs still finish without a fatal, the probes still pass, and `check_content.py --staged` is silent.

---

## 6. Environment and build

| What | How |
|---|---|
| Repo | `C:\Users\Sparrow\Desktop\astral-chain-pc-recomp` |
| C toolchain | MSYS2 UCRT64 clang/lld 23 at `C:\msys64\ucrt64\bin`; **never** `cl.exe` (generated C uses GNU extensions) |
| Python | `.venv` (3.12) for extraction, lifting, building, tests |
| Python for releases | `.venv-py314` (3.14): only `tools/make_release.py` needs it |
| .NET | 10.0 SDK, needed by `tools/build_xlate.py` |
| Config | `config.local.toml` (ignored): `input.base`, `input.update`, `input.keys`, `work.dir = 'build'` |
| Extract | `.venv\Scripts\python tools\extract.py` → `build/extracted/v0/{exefs,romfs}` |
| Translator | `.venv\Scripts\python tools\build_xlate.py` → `build/xlate/ac_xlate.exe` (~15 s once NuGet is warm) |
| Generate C | `.venv\Scripts\python recomp\gen.py --exefs build\extracted\v101\exefs --out generated/v101 --jobs 16 --patches recomp\patches-v101.txt --version v101` |
| Build | `.venv\Scripts\python tools\build_game.py --version v101 --jobs 16` |
| Run | `build\v101\astralchain.exe --seconds 30 [--shot PNG]` |
| Log | `build/v101/logs/astralchain.log` (rotated to `astralchain.1.log` … on each start) |
| Traces | `AC_TRACE=host,gfx,kernel,svc,ipc,fs,perf,sched,present,audio` or `all`; a bare `1` is rejected |
| Content check | `.venv\Scripts\python tools\check_content.py --staged` |

Facts that cost time when forgotten:

- `tools/build_game.py` does **not** regenerate the lifted C. After changing `recomp/`, a patch table or the generated
  C, run `recomp/gen.py` first (about 11 s for the whole game).
- `--patches recomp/patches-v101.txt` is mandatory. It holds the 32 spin waits found by
  `tools/spin_waits.py --exefs build\extracted\v101\exefs --modules main`. Without them clang hoists a guest spin loop's
  load and turns it into a tight `jmp $`; the game then stops making progress.
- The translator is auto-detected as the nearest `build/xlate/ac_xlate.exe` above the exe. `AC_XLATE` overrides it,
  `AC_XLATE_ID` overrides its cache identity; keep `ac_xlate.id` next to it.
- `--seconds S` ends at the first **presented** frame after the world clock passes `S`; it does not time out if the
  game never presents. `--shot` saves the game texture, never the ImGui overlay.
- A run needs the exe to not be running (ninja cannot replace a locked exe). A second instance should redirect its log:
  `… 2> <file>` writes to `<that folder>/logs` instead.
- `build/` is ~10 GB after extraction and generation. Never scan the whole RomFS per frame; the runtime uses a prebuilt
  index and a fault-paged image.

### Running the setup from the repository or a release folder

```text
python -m installer --base FILE --keys FILE --lang CODE --dest DIR [--root DIR] [--temp DIR] [--verify]
```

From the repository it uses the dev toolchain; from a release folder it uses the bundled one. The release folder builds
with:

```text
.venv-py314\Scripts\python.exe tools\make_release.py --out "build\release\Astral Chain-Setup" --zip "build\release\Astral Chain-Setup-0.1.0-windows.zip"
```

It needs `toolchain/hactool/hactool.exe` (`tools/build_hactool.py`) and `build/xlate/ac_xlate.exe` to exist.

---

## 7. Game and platform facts

- Title id `01007300020FA000`, update `01007300020FA800`. Base 1.0.0 only, Windows x64.
- Module builds: `main` = `ed2026b20685971f8b332095a7ff80ff`, `sdk` = `e29be8ad6778527aa248310d0c71a8a5`
  (the SDK is the same in 1.0.0 and 1.0.1 according to nxnative). SDK 6.4.0.
- `main`: image `0xcccc000`, bss `0xbf217e0`, 337 imports, needs `nnSdk.nss`, 16 exports.
  `sdk`: image `0xc75000`, 24 737 exports.
- NVN ABI the game insists on: major **53**, minor **313** (it accepts >= 310). `runtime/nvn_api_core.c` reports
  53/313; the shader GPU code minor version is **15** (Astral Chain's shaders are 1.15).
- No C++ exceptions, no `_Unwind_*` or `longjmp` imports; only `__cxa_guard_*` and `__cxa_pure_virtual`, so
  `recomp/regs.py`'s callee-saved assumptions hold.
- Memory layout (`generated/v101/layout.h`, decided only by `recomp/layout.py`):

  | Region | Base | Size |
  |---|---|---|
  | `main` | `0x7000000000` | `0xcce6000` |
  | RomFS image | `0x7000d35000` | `0x26689fefc` |
  | `sdk` | `0x7267600000` | `0xc75000` |
  | kernel / helpers | `0x7268400000` | — |
  | host functions | `0x7300000000` | — |

  `RT_RET_HOST` is `0x73000ff000`.
- RomFS (measured): 10 309 560 892 bytes, 8701 files. Metadata `0x6e25c` bytes at `0x2667887e0`; directory hash at
  `+0x184`, directory meta at `+0xbc8`, file hash at `+0x880c`, file meta at `+0x64d04`; file data at `0x200`.
- The RomFS image is mapped read-faulted, not copied. The mount cache is a small metadata cache (`0x6f000` bytes the
  game requests) copied straight into the guest allocation, never page-protected.
- Root listings must accept `Contents:/` and nested paths with or without a trailing slash: `Contents:/st8/` once
  produced `st8/`, which never matched `st8/st8.pkz`. Do not regress `romfs_dir_list`.

---

## 8. Code map

### Boot and CPU

- `recomp/` — NSO loader, lifter, layout, jump tables, patch tables, generator. Generated C is never edited by hand.
- `runtime/main.c` — process start, config, probes, `--seconds`, `--shot`.
- `runtime/boot.c` — `ldr_load`, constructors, `nn::init::Start`, first NVN checkpoint (`--boot` stops there).
- `runtime/loader.c` — module loading, relocations, imports, exports.
- `runtime/kernel.c` — virtual cores, threads, fibers, waits, events, shared memory, SVC 0x36 preemption.
- `runtime/memory.c` — guest address space, heap, read/write faults.
- `runtime/dispatch.c` — guest address → lifted function table; host functions are hashed separately.
- `runtime/svc.c` — SVC dispatch. `runtime/ipc.c` — HIPC/CMIF/TIPC, the named-service table, CMIF domains.

### Files and saves

- `runtime/fs_host.c` — the port's own `nn::fs`: mounts (`Contents:`, `save:`, `cache:`, `temp:`), handles, the atomic
  save transaction, and the host overrides `Commit`, `CommitSaveData`, `EnsureSaveData`.
- `runtime/romfs.c` — RomFS image and tables, mount cache, faults, the mod overlay.
- `runtime/ov_fsp.c` — the small `fsp-srv` service (access log only).

### Graphics

- `runtime/nvn_api_core.c`, `nvn_api_obj.c`, `nvn_api_cmd.c`, `nvn_api.h`, `nvn_obj.h` — the NVN front end.
- `runtime/vk_exec*.c` — replay, draw preparation (`bind_resources`), pipelines, descriptors.
- `runtime/vk_pipeline.c` — pipeline cache, `pipeline-keys.bin`, boot prewarm, AOT preparation, library builds.
- `runtime/gfx_shader.c` — shader service: translator children, cache keys, reflection, the recorded answers/programs,
  preparation from a seed or from the records.
- `runtime/vk_texture.c`, `vk_astc.c` — uploads, samplers (anisotropy, mipmap bias), native ASTC and the compute
  decoder. `runtime/gfx_present.c` — swapchain and pacing.

### Audio

- `runtime/ov_audout.c` — `audout:u` manager and the `IAudioOut` session; one SDL3 stream, a float ring, released
  buffers signalled through the session's kernel event.
- `runtime/ov_hwopus.c` — `hwopus` and `IHardwareOpusDecoder` over libopus.
- `runtime/audio_mixer.c`, `audren.h` — `IAudioDevice` volume that scales the stream.

### Input and UI

- `runtime/input.c` — SDL3 gamepad, keyboard, mouse, scripted input, recording, virtual pad.
- `runtime/input_map.c` — bindings and `AC_BIND_*`. `runtime/ov_input.c` — libnx `NpadState`, HID, vibration, six-axis.
- `runtime/ui/menu.cpp` — ImGui menu, performance overlay. `runtime/perfmon.c` — its numbers.

### Services

- In `runtime/ipc.c`: `audren:u`, `audout:u`, `fsp-srv`, `hid`, `hwopus`, `nifm:u`, `prepo:u`, `nfp:user`, `set`.
- Host overrides: `ov_system.c` (oe, applet/am, prepo, nfp, account, friends, time, `pl` fonts), `ov_input.c`,
  `fs_host.c`, `ov_gfx.c`, `ov_media.c`, `ov_settings.c` (including `LanguageCode::Make`), `ov_nifm.c`, `ov_prepo.c`,
  `ov_fsp.c`, `ov_audout.c`, `ov_hwopus.c`. `runtime/ov_probe.c` holds their probe cases.

### Installer and release

- `installer/` — `steps.py` (the five steps), `layout.py` (release vs dev paths, `version_tag`), `runner.py` (keys,
  resume, update), `cli.py`, `gui.py` + `guilogic.py` (Tk window), `text/<lang>.json` (every sentence is an id).
- `tools/make_release.py` — assembles and zips the release. `tools/build_hactool.py` builds the bundled hactool.

---

## 9. Fixes that must not regress

Each of these was a crash, a hang or a visibly broken feature; all are in the boot path now.

1. **RomFS mount cache.** Build it and copy it into the guest allocation with `mem_thaw` + `memcpy`. Never
   `plat_mem_protect` an allocation that is not page-exclusive: the next allocation can share the last page.
2. **RomFS root and nested listings.** Accept the mount root and trim trailing `/` in `romfs_dir_list`.
3. **Spin waits.** `recomp/patches-v101.txt` has 32 `spin main` lines; build with `--patches` or the port hangs.
4. **Shader GPU code minor version.** `runtime/nvn_api_core.c` reports `[0x30] = 15`; 14 rejects every shader.
5. **Per-stage NVN programs.** The game binds a vertex and a fragment program separately with stage masks.
   `nvnCommandBufferBindProgram` fills `ex->prog_stage[6]`, and `shader_for`, the memo key and the descriptor signature
   use the per-stage program. A single `ex->prog` skips every draw with "no vertex stage".
6. **`nvnCommandBufferFenceSync`.** The backend replays synchronously; the sync records `nvn_fence_last()`.
7. **Fiber `GetCurrentFiber` / `SwitchToFiber(0)`.** On the thread context `GetCurrentFiber` returns the `RtThread*`
   token and `SwitchToFiber` treats it as "back to the thread context"; never return 0.
8. **HID singular calls.** The game imports singular `GetNpadState`; `runtime/ov_input.c` overrides both forms.
9. **Start-up service chain.** `nifm::Initialize` must succeed; `account::OpenUser`/`OpenPreselectedUser` return a
   **Result** while `TryOpenPreselectedUser` returns a bool; `friends::UserPresence::Initialize` and
   `fs::EnsureSaveData` are overridden; `fsp-srv` 1005 must answer success with mode 0 or `IsEnabledAccessLog` aborts.
10. **`pl:u` shared fonts.** `GetSharedFont` is filled from `.bfttf` files and skips missing ones with count 0;
    `GetSharedFontAddress`/`Size` fail loudly on a damaged file.
11. **Descriptor push size (`runtime/vk_exec_draw.c` `bind_resources`).** For a vertex-only pipeline, set 1 is empty, so
    `d0[1]` stayed 0 and set 0's push size computed as `d0[1] - d0[0]` = 0 bytes while its template expected 72. The
    recorder copied nothing and the Intel driver dereferenced NULL when a save was loaded. The fix sets `d0[1] = nd`
    when the second set has no bindings.
12. **`nn::fs::CommitSaveData`.** The game saves through it, and the SDK body opens `fsp-srv` for an `IFileSystem` the
    port does not serve. `runtime/fs_host.c` overrides it (`S_COMMIT_SAVE`, `ov_commit`), next to `Commit`.
13. **`nn::settings::LanguageCode::Make`.** A `LanguageCode` is an 8-byte code string, not an enum index. `set:` cmd 5
    answers the string and `runtime/ov_settings.c` overrides `Make`; without it the game compared empty codes and stayed
    on English whatever the ini said.
14. **The install bakes its own ExeFS hashes.** `runtime/boot.c` `ldr_read_verified` refuses any ExeFS file whose
    SHA-256 differs from `rt_exefs_sha256`, which `tools/build_game.py` writes from `--expected`. The installer
    therefore writes `<temp>/expected-install.json` from the files it extracted and passes that: a player's
    `main.npdm` legitimately differs from ours, and `--accept-unknown-version` lets their code differ too. Never pass
    the reference dump there; it would refuse the player's own files at boot. The version check is the reference
    comparison (`tools/extract.py` `VERSION_FILES` = `main`, `sdk`, `rtld`), and `main.npdm` only warns.

---

## 10. Save data and other services

1. **`fsp-srv` / `IFileSystem`.** Saves work through `runtime/fs_host.c` (mount, atomic commit, recovery) because the
   SDK entry points are overridden. If a later path calls `OpenSaveDataFileSystem` and `IFileSystem::Commit` directly,
   implement cmd 51/52/53 and `IFileSystem` cmd 10 on a session object, publishing the host save. The layouts are in the
   emulator references; read them for behaviour and write our own code.
2. **`nfp:user` / amiibo.** A compatibility stub today. Either implement the NFC service or return the documented "no
   reader" results consistently.
3. **`account` / `friends`.** `ov_system.c` overrides `OpenUser`, `OpenPreselectedUser`, `CloseUser` and
   `friends::UserPresence::Initialize`; watch for new commands in a longer run.
4. **`prepo:u` / `nifm:u`.** Keep them local and logged. Do not answer a success the game then waits on.
5. **Shared fonts.** Drop `.bfttf` files where `AC_SHARED_FONTS` points (or the game's fallback path) if system-font
   text is needed.

A new service must be added to the table in `runtime/ipc.c`, declared in `runtime/ov.h`, and must respect CMIF domain
requests. An unknown command should log a backtrace and fail loudly.

---

## 11. Installer and release

Five steps (`installer/steps.py`): **check, extract, lift, compile, cleanup**. A sixth, **prepare**, comes back with the
AOT work (section 1.3).

- **check**: release files present, destination empty, language, host probe (`installer/hostcheck.c` built with the
  bundled clang: x86-64-v2 minimum, v3 when AVX2/FMA, a Vulkan 1.3 device), disk space (RomFS size + 4 GiB + the
  generated C), the game file opens with the keys and the base ExeFS matches `tools/data/expected-v1.0.0.json` and the update `expected-v1.0.1.json`.
- **extract**: `tools/extract.py` `extract_exefs` + `extract_romfs`, every IVFC level verified, then the ExeFS and
  RomFS hashes compared with the expected file.
- **lift**: `recomp/gen.py` with `recomp/patches-v101.txt`, after `tools/exefs_patches.py` turns the mods' ExeFS patch
  files into a patch set (a file the tool refuses costs those patches, not the install).
- **compile**: `tools/build_game.py --runtime-lib` with the prebuilt `libacrt.a`/`libacrt-v2.a`, then the exe,
  `romfs-index.bin`, `ac_xlate.exe` (+ `.id`) and the DLLs copied into the install folder. The `ac_xlate` copy is
  essential: the removed shader-preparation step used to do it.
- **cleanup**: write `astralchain.ini`, keep `gen/` and `build/` in the temp folder for the next rebuild, check that
  `romfs/` still holds the verified image, write `build-cache/rebuild.txt`.

Measured on this machine: 64.6 s for a full install (extract 19 s, lift 13 s, compile 32 s); the check computed
26.2 GiB needed on the install disk, with the temp folder on the same drive.

`tools/make_release.py` assembles the folder: bundled Python 3.14.9 (tkinter, numpy, lz4, cryptography, cffi,
pycparser), llvm-mingw, `runtime/` (libacrt, headers, deps), `bin/` (ac_xlate, hactool, ninja, SDL3, opus), `recomp/`,
`tools/` (with `tools/data/expected-v1.0.0.json` and `expected-v1.0.1.json`), `licenses/`, the notices, the ini example and `VERSION`. It then
runs `check_content.py`, starts the folder's own installer to see it reach `check.inputs_needed`, builds every window
screen in both languages, and drops `__pycache__` and the setup's `temp/` before zipping.

Release facts:

- The zip holds one top folder, `Astral Chain-Setup/`; the exe is unsigned, so SmartScreen warns once.
- The release string is `VERSION` + the commit (`0.1.0+<sha>`); rebuilding changes the zip, so recompute its SHA-256
  before publishing.
- `docs/installer.md` describes the layout, the steps and the CLI; `README.md` carries the player-facing text.

---

## 12. Controller and input

Full instructions are in `AGENTS.md` ("Controller"). Summary:

- Physical SDL3 gamepad: hot-plug, both sticks, all buttons, analog ZL/ZR, rumble, gyro. No setup.
- `AC_PAD_LAYOUT=auto|nintendo|xbox`, `AC_BIND_<ACTION>` or the Buttons tab.
- Default keyboard: WASD move, arrows camera, E=A, Space=X, Shift=B, Q=L, R=R, Z=ZL, F=ZR, 1-4 D-pad, Tab=Plus,
  M=Minus; mouse left=Y, mouse right=ZL; `AC_INPUT=none` disables input.
- `AC_INPUT_SCRIPT=<file>`: `<frame> <button> [hold]` in 60 Hz ticks or `@<ms> <button> [hold]` in world-clock ms;
  `LSTICK`/`RSTICK <x> <y> [hold]`; buttons `A B X Y L ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK`.
  `AC_INPUT_RECORD=<file>` writes the same format; `AC_TEST_PAD=<lx>,<ly>,<rx>,<ry>` makes a virtual pad.
- A stick line sets the stick absolutely only inside its `at..at+hold` window, so a multi-segment recording with a
  dropped frame leaves a centre gap; record gestures as one segment when replaying them.
- The game reads the pad through `runtime/ov_input.c` (libnx `NpadState`) fed by `runtime/input.c`. Handheld mode
  (`AC_OPERATION_MODE=handheld`) uses id 0x20 / style 2; otherwise id 0 / full-key style 1.

---

## 13. Graphics and performance

- Budgets are a feature: keep upload, replay and present cheap, no per-frame allocations, and measure with
  `AC_TRACE=perf` and the overlay instead of guessing.
- The render-context list is at `objB = [0x7000da8000+0x640]` (0x700cc9c298): count `+0x20`, sentinel `+0x28`, node
  payload `node+0x18`. `f_70009aba20` iterates it, `f_70009aa680` checks `ctx+0x169` bit 6 and calls the vtable at
  `ctx+0x300+0x38/0x40/0x48/0x50`, `f_70009abae0` creates a context. Useful when a scene renders black again; an
  empty list is not automatically a bug (the title has none until the scene is created).
- ASTC is everywhere. Native sampling is used when `textureCompressionASTC_LDR` exists, the compute decoder otherwise;
  3D ASTC cannot be sampled natively and the decoder handles only 2D. Re-check `AC_ASTC=auto` and `--probe-astc` after
  any texture or sampler change.
- Graphics pipeline libraries, a replay recorder thread, fast links and library builds on workers are what keep the
  30 fps tick; the overlay shows translation, pipeline build, replay, upload and present per frame.

---

## 14. Testing checklist

After each significant change:

1. `.venv\Scripts\python tools\check_content.py --staged` before a commit.
2. Regenerate the C with `--patches`, then `tools/build_game.py`.
3. `build\v101\astralchain.exe --seconds 10 --shot build\v101\title.png`: the Astral Chain logo and `ZL + ZR`, not black.
4. A scripted New Game (`AC_INPUT_SCRIPT=build\play_seq.txt`, `--seconds 45`) reaches the police HQ scene, ~30 fps, no
   fatal, no shader failure, no skipped draw. `build\play_seq.txt` is the user's own sequence: ZL+ZR, stick up, A,
   A, stick left, A, stick left, A.
5. AOT (section 1.6): fresh folder, `--prepare-shaders` + `--warm-shaders`, 5 minutes of play with no translation.
6. `--probe-ipc`, `--probe-services`, `--probe-astc`, `--probe-nvn`, `--probe-gfx-abi`, `--probe-shader`,
   `--probe-window`, `--probe-dpi` where relevant. `--probe-services` runs ~2900 checks with ~60 known failures
   (shared fonts, locale cases the ini overrides).
7. Installer: `tests/installer_test.py` and `tests/installer_gui_test.py` (unittest), plus one real install from a
   release folder and a 20 s boot of the installed exe.
8. Lifter changes: the differential tests against Unicorn (`docs/building.md`), and 0 disagreements in the jumptables
   check after touching `recomp/jumptables.py`.

### Debug tips

- Log: `build/v0/logs/astralchain.log`; `AC_TRACE=ipc` for service/command/result lines (the first stop for a missing
  command), `AC_TRACE=gfx` for per-frame submit/draw/clear counts, `AC_TRACE=perf` for frame times.
- `AC_CORE_DIAG=1` explains core waits, `AC_TRACE=sched` gives per-thread accounting; neither belongs in a performance
  measurement.
- Scratch scripts under `build/` (not in the repo): `unwind.py` (minidump unwinding), `disasm_rva.py`, `dumpinfo.py`,
  `find_map.py` (host RVA → symbol via `build/v0/totk.map`).
- A host PC at `exe+0xd38240` in a log is `f_70003dfc50+0x70`, the hoisted spin loop: the spin patch table was not
  applied.

---

## 15. Pitfalls

- Generated C is never hand-edited: change `recomp/` and regenerate, marker included.
- `recomp/layout.py` alone decides module bases; never hardcode an address where `layout.h` has a query.
- `cl.exe` cannot compile the generated C (GNU extensions). Use the MSYS2 UCRT64 clang, or the bundled llvm-mingw in a
  release.
- `plat_mem_protect` works per page: protecting part of a page affects neighbouring allocations.
- Host functions live outside the modules; `rt_dispatch` must resolve them through the host hash table.
- SDK-internal functions are not imports. When the SDK calls an internal function that opens a service, implement the
  service side, not just an import override.
- CMIF domains are required by some services; keep and extend the minimal implementation in `runtime/ipc.c`.
- Async paths (audio playback, shader translation) must not block IPC handlers. The audio session signals released
  buffers from the SDL thread after releasing the session lock, for the same reason.
- Text files are LF, UTF-8, no BOM; `Set-Content` in PowerShell adds CRLF and once a stray byte, so prefer the editor
  tools.
- No game data in git: `build/`, `generated/`, caches, seeds, screenshots and `build/xlate/` stay out.

---

## 16. Reference

- `docs/architecture.md` — design of the port.
- `docs/building.md` — toolchain, build, probes, lifter checks. Some shader-preparation names in it come from TotK and
  do not exist here (section 1.3).
- `docs/maxwell.md` — the Maxwell behaviour the Vulkan path must match.
- `docs/astral-chain.md` — Astral Chain port notes, RomFS and address facts.
- `docs/installer.md` — the setup: steps, layout, release, CLI.
- `AGENTS.md` — priorities, content rules, controller instructions.
- `refs/` — MIT/BSD upstreams (TotK/MacNativeGames, LAS Recomp, nxnative) and the pinned Ryujinx translator.
