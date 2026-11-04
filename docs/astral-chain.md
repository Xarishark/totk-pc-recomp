# Astral Chain port notes

Working notes for porting the TotK Recomp pipeline to Astral Chain 1.0.1 (01007300020FA000, update
01007300020FA800). Facts marked "nxnative" come from the MIT nxnative project's game profile and are
re-verified against our own 1.0.1 dump where possible.

## Iteration 1 scope

Base 1.0.0 + update 1.0.1, 60 fps by default with a 30 fps switch. Windows only.

## Verified from the user's dumps

- Base 1.0.0 ExeFS modules: `main` (7 644 617 B), `sdk` (5 231 344 B), `rtld`, `main.npdm`. No `subsdk0`.
- Update 1.0.1 ExeFS: `main` (7 700 590 B, 4b159f0f7a360669738390ee8657b4f0), `sdk` and `rtld` byte-identical
  to 1.0.0, `main.npdm` different (same priority 44, core 0, stack 0x100000). The only new import is
  `nn::oe::SetUserInactivityDetectionTimeExtended`.
- main.npdm: main thread priority 44, core 0, stack 0x100000 (matches `runtime/kernel.c` MAIN_*).
- Base RomFS: 8 701 files, 10 309 560 892 B. The 1.0.1 update carries a BKTR delta section (about 183 MB),
  merged with the base Program NCA by hactool; its image and hash are in `tools/data/expected-v1.0.1.json`.
- Ticketless base dump (key-area AES-CTR); the update NSP carries a common ticket. hactool is only needed for
  the BKTR merge.

## Textures: ASTC

Astral Chain stores most textures as ASTC (PlatinumGames pipeline). The host covers the full NVN ASTC set
(runtime/nvn.h, formats 0x79-0x94: 14 LDR + 14 sRGB block sizes) and decodes on the GPU when the device lacks
native support (runtime/vk_astc.c: `AC_ASTC=auto|native|gpu|cpu|check`, `AC_ASTC_BC=1` re-encodes to BC3).
The Intel Arc 140T reports textureCompressionASTC_LDR, so `auto` samples natively.

Watch at runtime:
- ASTC **3D** textures: Vulkan has no native 3D ASTC; the gpu/cpu decoder handles 2D only. If the game uploads
  ASTC 3D (NVNtextureType 3D), the upload path must decode it (nvxnative notes do not mention any).
- sRGB ASTC: mapped; verify the game's linear/sRGB expectations match the TotK behaviour (nvn_api_tex.c).

## Game facts to re-verify (from nxnative's 1.0.1 profile)

- SDK 6.4.0, NVN, PlatinumGames engine, Wwise audio through nn::audio (audren), Bink 2 movies (.bk2, decoder
  most likely statically linked in main = guest code, nothing to do).
- 82 PKZ archives (version 0x10000), 8 556 entries, zstd compression (not Oodle); DAT/DTT containers.
- 1 348 shaders (540 VS, 738 PS, 70 CS) in PlatinumGames VSO/PSO/CSO/HSO/DSO objects; 1 620 cover pairs.
- GPU code versions 1.9/1.12/1.15; the loader compares byte 7 with the driver at main+0x9e97d0.
- Swap chain 1600x900, present interval 2 at the shipping 30 fps; with the 60 fps patch interval 1. Heap ~3 GB, BSS
  191 MB (image ~0xccf0000).
- mem_reserve 0xf0000000, app_pool 0xCD500000, sys_resource 0; depth mode near=0; upper-left origin;
  alpha test GREATER via its own lib; swkbd for character creation.
- 60 fps: `recomp/patches-v101.txt` set `fps60` runs the community static 60 fps patch (MrComputerRevo's IPS, its
  offsets include the NSO header): the frame-time constants go from 1.0/2.0 to 0.5/1.0 and the frame call's wait
  argument is cleared. The menu's `AC_FPS` (60 default, 30) switches the set live. `runtime/frame_rate.c`
  `cutscene_rate` writes the cutscene selector 2 (30 fps) while the game's cutscene speed variable is 1/30, as
  Hazerou's cutscene fix does.

## Risks to check during the census and boot

- C++ exceptions / unwinder: if main or sdk import `__cxa_throw`, `_Unwind_*` or `longjmp` with callers, the
  callee-saved trust of recomp/regs.py is unsound: use `recomp/gen.py --no-callee-saved-trust`.
- SDK 6.4.0 host services not covered by the TotK override set (178 overrides exist; Astral Chain imports a
  different subset).
- NVN ABI version: runtime/nvn_api_core.c reports major 55 / minor >= 15 (TotK); SDK 6.4.0's NVN may require a
  different version and abort (verify_boot / the first nvnDeviceGetProcAddress).
- NVN proc table: runtime/nvn_procs.inc has 537 TotK proc names; unknown asks stop the game (add as needed).
- Bink 2 movies: guest code, but they may use nn::codec or other host services not yet implemented.
- Heap size 3 GB: the TotK memory layer reserves a large heap; keep the reservation but watch the game's
  requested sizes at boot (nv::Initialize graphics work memory).

## Census results (tools/import_census.py, base 1.0.0)

- main imports 337 symbols, sdk exports 24 751, the host overrides 121 (plus 7 observers). No strong import of
  main is unresolved (every one is an sdk export or a host override), so the loader's link step cannot stop on
  a missing symbol.
- No `__cxa_throw`, `_Unwind_*` or `longjmp` among the imports (only `__cxa_guard_acquire/_release`,
  `__cxa_pure_virtual`): the callee-saved trust of recomp/regs.py holds.
- Import groups: nn::fs 19 (all directly imported), nn::oe 9, nn::os 9, nn::hid (no GetNpadStates: input goes
  elsewhere, watch at boot), nn::mem, nn::nifm (Initialize), nn::time, nn::util, nn::codec (1 direct opus
  import), nn::image JpegDecoder/JpegEncoder (new vs TotK, resolved by sdk), nn::prepo, nn::swkbd, nn::fontll,
  nn::account, nn::friends, nv::InitializeGraphics, plain libc/musl/math (73) and C++/libc++ support (25).
- Dead host overrides to purge later (not imported by main 1.0.0): hid GetNpadStates/GetSixAxisSensor/GetVibrationDevice/
  detail::*, fs InitializeForApplication, oe Initialize/TryPopLaunchParameter/FinishStartupLogo/SetCpuBoostMode/
  GetExpectedVolumeBalance/PopNotificationMessage, pl
  GetSharedFontLoadState, vi GetDisplayVsyncEvent, aoc, err, am, nfp. 1.0.1's `main` newly imports
  `oe::SetUserInactivityDetectionTimeExtended`, which the host already answers as a no-op (`runtime/ov_system.c`).

## Code generation notes (base 1.0.0, update 1.0.1)

- main 1.0.0: 27 788 functions, 2 670 jump tables, 44 297 traps, 1 552 branches through dispatch, 0 unsupported
  instructions. sdk: 33 564 functions, 516 jump tables, 36 919 traps, 0 unsupported.
- main 1.0.1: 27 890 functions, 2 701 jump tables, 44 452 traps, 1 558 branches through dispatch, 0 unsupported
  instructions. sdk is byte-identical to 1.0.0.
- Three R_AARCH64_RELATIVE relocations in main's .rodata point at module base +0/+1 (the MOD0 header itself).
  recomp/module.py seeds() now ignores RELATIVE addends below 0x24 so the lifter no longer decodes header words
  as code (fix for the bogus `rt_udf` at 0x7000000014).

## RomFS: fixed image address

- Unlike TotK, Astral Chain never opens RomFS files through nn::fs (the boot traces only
  QueryMountRomCacheSize and MountRom). The game parses the RomFS image in memory at a fixed address baked
  into its content manager: **0x7000d1b000** in 1.0.0 (f_70005693e0), **0x7000d35000** in 1.0.1
  (f_700056e800). The MountRom cache pointer the game passes is just scratch; the image itself must be
  mapped at that address, so `recomp/layout.py` picks `ROMFS_BASE` per version (`gen.py --version`).
- The host does not map the file directly: `MapViewOfFileEx` needs a 64 KiB-aligned base and neither address
  is. `recomp/layout.py` reserves `[ROMFS_BASE, ROMFS_BASE + ROMFS_SIZE)` between main and sdk, and
  `runtime/romfs.c` `romfs_read_fault` commits each 4 KiB page on the first guest read and fills it from
  `romfs.bin`. The image is ~10.3 GB, so sdk starts above the image; `runtime/host.h` keeps the kernel at
  0x7268400000 and the host-function window at 0x7300000000. Regenerating after changing layout.py is
  mandatory (all absolute addresses change).
- `nn::fs::MountRom` receives a cache buffer, and the SDK parses the RomFS metadata from it, not from the
  image. `runtime/romfs.c` `ov_mount_rom` builds that cache: the 0x50-byte header followed by the four
  directory/file hash and metadata tables with their offsets rebased into the cache. `QueryMountRomCacheSize`
  returns the aligned size. Without this the SDK reports an empty RomFS and the game's file table stays empty.
- The game lists directories with a trailing slash (`Contents:/st8/`); `romfs_dir_list` trims it and also
  accepts the mount root, so every nested archive is enumerated.
- The game's resource pipeline (registry hash at [main+0xda8000+0x3e0/+0x428], file table at +0x370,
  IO queue at +0x3e8) registers entries found by parsing the image; the entryPoint looks up
  "shader2.dat" in the registry and fails the boot if it is not registered and loaded.
