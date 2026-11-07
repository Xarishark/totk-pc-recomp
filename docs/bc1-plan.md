# BC1 recompression of opaque ASTC textures

Status: phase 0 and phase 1 are done (November 2026). The knob is `AC_ASTC_BC = bc1 | 1 | 0`, default `bc1`
explicitly set or, unset, BC3 from 6 GiB of device-local budget up and BC1 below. Extensions at the end are
still out of scope until the measurements say otherwise.

## Phase 0 measured (Intel Arc 140T, import/UMA, title screen + HQ, 150 s)

- Cover: 59 % of the recompressed ASTC storage bytes are provably opaque (112 of 191 MiB; a later run read
  64 and 67 %), so phase 1 went ahead.
- Where the images live: every recompressed image took own memory (pool-bound 0 on this device), 783 MiB of
  images for 191 MiB of storage — BC3 inflates the game's 6x6–12x12 footprints (3.6–2 bits per texel) back
  to 8. On this device BC1 cuts the largest texture cost directly.
- The scan: 1.8 ms per MiB of storage on average (warm-cache windows read 20 us per MiB, strided cold ones
  2.5 ms), 340 ms in total over the session, inside the texture uploads it already pays for.

The rest below is the original plan.

## Background

Astral Chain's textures are almost all ASTC. Where the device lacks `textureCompressionASTC_LDR`, `vk_astc.c`
decodes the blocks to RGBA8 with a compute shader and, since `AC_ASTC_BC` (default 1), a second pass
(`runtime/shaders/bc3_encode.comp`) re-encodes the texels to BC3: 8 bits per texel instead of the 32 of the
RGBA8 decode. The format choice is per device, made in `tex_vk_format` (`runtime/vk_formats.c:129`).

## Key insight

The colour part of a BC3 block *is* a BC1 block: the same two 5:6:5 endpoints and the same 2-bit palette
indices; `bc3_encode.comp`'s colour path already produces one. For a texture whose alpha is 255 everywhere,
encoding to BC1 gives **the same quality as the current BC3 output at half the size** (4 bits per texel).
So the only real problem is choosing BC1 per texture, before the image exists: BC1_RGB carries no alpha.

## Design: the alpha verdict, on the CPU, from the raw block headers

- An ASTC LDR block can only produce alpha ≠ 255 if its colour endpoint modes (CEMs) carry an alpha channel.
  That is visible in the 16 header bytes of the block, without decoding it.
- The Tegra block-linear swizzle never moves the low 4 bits of an offset (`runtime/tex_deswizzle.c`: the
  GOB layout permutes 16-byte sectors only). Every 16-byte ASTC block is therefore contiguous in the raw
  pool storage, so the scan walks the storage at 16-byte granularity and needs no deswizzle.
- A padding or garbage 16-byte unit read as a block can only produce a false *positive* ("alpha-capable" →
  stays BC3), never a false negative: the scan is **sound**. A false negative would need a real block whose
  own 16 bytes scan as opaque while it decodes with alpha, which the scan of the same bytes excludes.
- The header/CEM parse is refactored out of `astc_decode_block` (`runtime/tex_astc.c`) and shared with the
  decoder, so the `--probe-astc` oracle (scan verdict vs. decoded alpha, every block bucket) keeps the two
  in step.

## Where the VRAM actually goes (measure before believing)

Two places a recompressed image can live (`bind_pool_memory`, `runtime/vk_texture.c:229`):

- **Bound into pool memory**: the image's bytes sit inside the pool's device-local allocation at the
  texture's storage address. BC3 fits (about half the NVN ASTC span); BC1 fits more easily still, but the
  pool allocation itself is sized by the game, so the saving there is not the full half — the image simply
  stops needing own memory and stops counting against the texture cache budget.
- **Own memory** (`e->alloc`, `own_bytes`): counted against `TEX_CACHE_MB`, dropped under
  `TEX_VRAM_PRESSURE`, re-uploaded when needed again. Here BC1 is a clean 2× saving and, more importantly
  for a 4 GiB card, halves the pressure that causes cache drops and re-upload churn.

An RGBA8-decoded ASTC image (4× the storage span) rarely binds into a pool, which is why `AC_ASTC_BC` was
worth so much: it moved textures out of own memory into the pool span. BC1's further gain is on the
own-memory entries and on whatever fraction of pools cannot bind (sparse/virtual pools, `scale > 1`
textures). Phase 0 measures the split instead of guessing.

## Phase 0 — measurement (no format change, ~1 day)

1. Counters on the existing texture perf line (`vk_texture.c`): ASTC upload bytes total, bytes whose scan
   verdict is "provably opaque" (the scan itself lands here as a dry run), and for each upload whether the
   entry ended up pool-bound or own-memory, with the own bytes.
2. The scan runs at entry creation over the raw storage (see phase 1); in phase 0 its verdict is only
   logged, the image stays BC3. `AC_TRACE=perf` / the overlay show the cost inside `PERF_TEX`.
3. Deliverable, from a normal play session in the heaviest areas (town, monsters fight):
   - the fraction of recompressed bytes that are provably opaque (go/no-go for phase 1's coverage; if the
     Nintendo encoder uses alpha CEMs everywhere, phase 1 is shelved in favour of the GPU probe extension);
   - the bound-vs-own split, so the 4 GiB budget forecast uses real numbers;
   - the scan's CPU cost per MiB of storage.

## Phase 1 — the BC1 path (~3–5 days)

| # | File | Change |
|---|---|---|
| 1 | `runtime/shaders/bc1_encode.comp` (new) | The colour path of `bc3_encode.comp`, writing one 8-byte block (`uvec2`). `tools/build_shaders.py` compiles it to `bc1_encode_spv.h`. |
| 2 | `runtime/tex_astc.c`, `tex.h` | `astc_block_alpha_possible(const uint8_t block[16])` (header-only verdict) and a scan over raw storage; CEM parse shared with the decoder. |
| 3 | `runtime/vk_astc.c` | `G.bc1_pipe`, created like `create_bc_pipeline` but probing `VK_FORMAT_BC1_RGB_UNORM_BLOCK` / `_SRGB_BLOCK` features; `vk_astc_record`'s `bc` becomes 0/1/2 (RGBA8/BC3/BC1) and the block stride 8 bytes for BC1. |
| 4 | `runtime/tex.h`, `vk_formats.c` | `TexVkFormat.astc_bc` becomes 0/1/2. The device default from `tex_vk_format` stays BC3; the entry downgrades itself to BC1 after the scan. |
| 5 | `runtime/vk_texture.c` | At entry creation (the `create` flow around line 476): when the device chose BC3 and the scan says opaque, switch `e->f` to `BC1_RGB(_SRGB)` before the image is created, and count the bytes. `if (e->f.astc_bc) … return 16` (line 969) becomes 8 for BC1; the `bc_cut` WriteTexels logic is unchanged (same 4×4 rule). On every re-upload and WriteTexels re-upload, re-scan: if the verdict flips opaque → alpha-capable, destroy the entry (the existing `destroy_tex` / `vk_exec_image_gone` path) so the next lookup creates it as BC3 — this is the soundness rule for CPU-rewritten storage. |
| 6 | `runtime/cfg.c` | `AC_ASTC_BC = bc1 \| 1 \| 0`, default `bc1` once validated (`1` keeps BC3-only, `0` RGBA8), `RESTART` flag, and the low-VRAM settings pack line updated. |
| 7 | `runtime/astc_probe.c` | `--probe-astc`: BC1 PSNR against the decoded RGBA8 reference (the same floor the BC3 colour already meets), alpha exactness (every decoded alpha 255), and the scan oracle: over every generated block bucket (1p/2p/3p/4p/dual), the scan must never say "opaque" for a block that decodes with alpha ≠ 255. |

### Knob

`AC_ASTC_BC=bc1` (default after validation): BC1 where provably opaque, BC3 for the rest. `=1` restores
today's BC3-everything. `=0` decodes to RGBA8. The choice is per texture but the knob stays a startup
knob (`RESTART`), matching the current one.

## As built (the differences from the table above)

- The scan runs not only at entry creation but at every upload the entry is due for (first upload included),
  and a verdict that turned against the image's format trades the entry in: it is destroyed at the next
  submit (`sweep_flip`) and the next lookup recreates it in the other format from a fresh scan. Nothing
  writes into an image whose format the content no longer matches; a traded entry answers lookups with the
  image it has for one submit.
- Unset knob: BC3 with 6 GiB or more of device-local budget (`vk_mem_vram`), BC1 below; the Steam Deck
  profile sets `bc1` explicitly. The resolved choice is logged at start
  (`vk: AC_ASTC_BC unset, ... GiB of the device-local budget: AC_ASTC_BC=...`).
- The full perf overlay shows the resolved holding (Window block, "Texture compression") and the OS line
  (`plat_os_version`) under the device names.
- `--probe-astc` runs the BC1 cases per footprint and colour space (28 cases, PSNR floor 15 dB met, lowest
  20.3 dB) and an oracle over every block kind: the scan must never call a block opaque that decodes alpha
  other than 255.

## Tests and checks (per AGENTS.md)

- `--probe-astc` green: 14 footprints × linear/sRGB, the scan oracle, BC1 PSNR and alpha exactness, zero
  validation-layer errors.
- Upload path checked against ASTC sRGB, mip chains, arrays and cubemaps; `AC_ASTC=check` unchanged
  (readback ignored when recompressed); 3D ASTC stays out of the decode path as today.
- `AC_TRACE=perf` + the overlay: frame time neutral, the scan inside `PERF_TEX`, and the BC1/BC3/own-bytes
  line for the budget.
- `--shot` comparisons in a cutscene and the town before/after.
- `python tools/check_content.py --staged`: shader and code only, no game bytes.
- Docs updated: `architecture.md` §4 ASTC bullet, `docs/building.md` "Checks".

## Risks

1. **Coverage unknown** — phase 0 answers it before any format change.
2. **Scan cost on the uploading thread** — a strided read over the raw storage, once per texture creation
   and again per re-upload; bounded by the storage size and measured in phase 0.
3. **Mid-life format flip** (BC1 → BC3 after a storage rewrite) — new machinery, but the destroy/create
   path is already exercised by `AC_TEX_ALIAS` and released entries; the oracle case is a texture the CPU
   rewrites with alpha content after an opaque upload.
4. **Devices without BC1 sampling** — essentially none (the game itself uses DXT1 formats 0x41–0x46), but
   the pipeline probe falls back to BC3 exactly like `create_bc_pipeline` does.

## Out of scope until measured (former phase 2)

- Cutout textures (alpha 0/255 only) through `BC1_RGBA` punchthrough.
- A GPU-side alpha probe of the decoded texels with deferred re-creation, if the header scan proves too
  conservative.
- Recompressing the game's non-ASTC RGBA8 mirrored textures with the same encoders.
- Forcing decode+BC1 on devices that *do* sample ASTC natively, for 4×4/5×5 footprints only (8 and 6.4
  bits per texel → 4): beyond 6×6 the native ASTC is already smaller than BC1.
