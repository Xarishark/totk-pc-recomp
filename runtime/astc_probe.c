// --probe-astc: the compute ASTC decoder (vk_astc.c, shaders/astc_decode.comp) against astc_decode_ldr, bit for bit,
// for every block footprint, linear and sRGB. Synthetic blocks only (no game files): valid void extents, invalid and
// HDR void extents, random blocks kept by what they exercise (1-4 partitions, dual plane with 1-3 partitions, every
// weight grid and quantization the random modes reach), then plain random blocks (mostly error blocks, HDR endpoint
// modes among them). Each case decodes two regions into two mip levels of an RGBA8 image with cropped edges, and
// compares both the decoder's texels (readback) and what the image holds afterwards. The same regions also go through
// the BC3 encoder (AC_ASTC_BC) into a BC3 image, whose blocks are decoded here and held to error bounds (bc_check).
// Every block the alpha scan (astc_block_alpha_possible, the BC1 choice of vk_texture.c) calls opaque is decoded and
// its alpha bytes checked, and the counts reported; a region of such blocks also goes through the BC1 encoder into a
// BC1 image and is held to the same colour bounds with alpha exact (bc1_check, test_bc1). Also times one 2048x2048
// texture on the CPU (astc_decode_ldr_mt)
// and on the GPU (wall time of the upload batch).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "host.h"
#include "tex.h"
#include "vk.h"

static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint64_t rnd64(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

static const uint8_t dims[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                    {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

// Buckets of valid blocks: partitions 1-4 single plane, then dual plane with 1-3 partitions.
enum { NB = 7, PER_BUCKET = 48 };
static const char *const bucket_name[NB] = {"1p", "2p", "3p", "4p", "dual 1p", "dual 2p", "dual 3p"};
static uint64_t g_bucket[NB], g_void, g_err, g_blocks, g_texels, g_bc_cases;
static double g_bc_db = 99;   // the lowest BC3 PSNR of all cases
static uint64_t g_scan_blocks, g_scan_opaque;   // the alpha scan oracle: blocks scanned / called opaque
static uint64_t g_bc1_cases;
static double g_bc1_db = 99;   // the lowest BC1 PSNR of all cases

static int dual_bit(uint32_t m) {
    if (!(m & 3) && (m >> 7 & 3) == 2) return 0;   // the 6..10 x 6..10 modes have no dual plane
    return (int)(m >> 10 & 1);
}

static void void_block(uint8_t b[16], int kind) {
    // kind 0: valid LDR extent (random coordinates s0 < s1, t0 < t1), 1: all-ones extent, 2: s0 >= s1 (error),
    // 3: HDR (error), 4: reserved bits not 11 (error)
    uint64_t lo = 0x1FC | 3ull << 10, s0 = rnd64() & 0xFFF, s1 = s0 + 1 + (rnd64() & 0xFFF), t0 = rnd64() & 0xFFF,
             t1 = t0 + 1 + (rnd64() & 0xFFF);
    if (s1 > 0x1FFF) s1 = 0x1FFF;
    if (t1 > 0x1FFF) t1 = 0x1FFF;
    if (kind == 1) s0 = s1 = t0 = t1 = 0x1FFF;
    if (kind == 2) { uint64_t x = s0; s0 = s1; s1 = x; }
    lo |= s0 << 12 | s1 << 25 | t0 << 38 | t1 << 51;
    if (kind == 3) lo |= 0x200;
    if (kind == 4) lo &= ~(1ull << 11);
    uint64_t hi = rnd64();
    memcpy(b, &lo, 8), memcpy(b + 8, &hi, 8);
}

// n blocks for footprint bw x bh: the buckets first, void extents, then random.
static void make_blocks(uint8_t *blk, size_t n, uint32_t bw, uint32_t bh) {
    size_t k = 0;
    uint8_t tmp[12 * 12 * 4];
    int have[NB] = {0};
    for (int tries = 0; tries < 400000 && k < n; tries++) {
        uint64_t v[2] = {rnd64(), rnd64()};
        uint8_t *b = (uint8_t *)v;
        uint32_t m = (uint32_t)(v[0] & 0x7FF);
        if ((m & 0x1FF) == 0x1FC || !astc_decode_block(b, bw, bh, tries & 1, tmp)) continue;
        int p = (int)(v[0] >> 11 & 3), bk = dual_bit(m) ? 4 + p : p;
        if (bk >= NB || have[bk] >= PER_BUCKET) continue;
        have[bk]++, g_bucket[bk]++;
        memcpy(blk + 16 * k++, b, 16);
    }
    for (int i = 0; i < 40 && k < n; i++, g_void++) void_block(blk + 16 * k++, i % 5);
    for (; k < n; k++) {
        uint64_t v[2] = {rnd64(), rnd64()};
        memcpy(blk + 16 * k, v, 16);
        if (!astc_decode_block(blk + 16 * k, bw, bh, false, tmp)) g_err++;
    }
}

typedef struct {
    VkImage img;
    VmaAllocation alloc;
} Img;

static int make_image(VkCtx *c, uint32_t w, uint32_t h, uint32_t levels, VkFormat format, Img *o) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = format, .extent = {w, h, 1},
                            .mipLevels = levels, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return vmaCreateImage(c->vma, &ci, &ai, &o->img, &o->alloc, NULL) == VK_SUCCESS;
}

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                    VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
#define ALL_RW VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT

static void to_general(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static double now_ms(void) { return (double)plat_now_ns() / 1e6; }

// Decodes regions (w0 x h0 at level 0, w1 x h1 at level 1) of blk on the GPU; out_rb: the decoder's texels, out_img:
// the image's two levels read back. 0 on a Vulkan failure.
static int gpu_decode(VkCtx *c, const uint8_t *blk, size_t n0, size_t n1, uint32_t w0, uint32_t h0, uint32_t w1,
                      uint32_t h1, uint32_t bw, uint32_t bh, int srgb, uint8_t *out_rb, uint8_t *out_img, double *ms) {
    Img im;
    if (!make_image(c, w0, h0, 2, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, &im)) return 0;
    size_t t0 = (size_t)w0 * h0 * 4, t1 = (size_t)w1 * h1 * 4;
    VkCommandBuffer cb = vk_upload_begin();
    VkBuffer sb, rb, ib;
    VmaAllocation ra, ia;
    uint8_t *stg = vk_staging((n0 + n1) * 16, &sb, NULL);
    uint8_t *rmap = vk_staging(t0 + t1, &rb, &ra), *imap = vk_staging(t0 + t1, &ib, &ia);
    memcpy(stg, blk, (n0 + n1) * 16);
    to_general(cb, im.img);
    VkAstcRegion ar[2] = {{.src = 0, .w = w0, .h = h0, .slices = 1, .level = 0},
                          {.src = n0 * 16, .w = w1, .h = h1, .slices = 1, .level = 1}};
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int ok = vk_astc_record(cb, sb, (n0 + n1) * 16, ar, 2, bw, bh, srgb, 0, im.img, VK_IMAGE_ASPECT_COLOR_BIT, rb);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    VkBufferImageCopy cp[2] = {{.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w0, h0, 1}},
                               {.bufferOffset = t0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1}, .imageExtent = {w1, h1, 1}}};
    vkCmdCopyImageToBuffer(cb, im.img, VK_IMAGE_LAYOUT_GENERAL, ib, 2, cp);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    vk_upload_end();
    double a = now_ms();
    vk_upload_flush(1);
    if (ms) *ms = now_ms() - a;
    vmaInvalidateAllocation(c->vma, ra, 0, VK_WHOLE_SIZE);
    vmaInvalidateAllocation(c->vma, ia, 0, VK_WHOLE_SIZE);
    if (out_rb) memcpy(out_rb, rmap, t0 + t1);
    if (out_img) memcpy(out_img, imap, t0 + t1);
    vmaDestroyBuffer(c->vma, rb, ra), vmaDestroyBuffer(c->vma, ib, ia);
    vk_defer_image(im.img, im.alloc);
    vk_upload_flush(1);
    return ok > 0;
}

// The same two regions decoded and encoded to BC3 (bc 1) or BC1 (bc 2) (AC_ASTC_BC) into an image of that
// format; out: its blocks read back, level 0 then level 1, rows of 4x4 blocks. n1 = 0: one region, one level.
// 0 on a Vulkan failure or when nothing was recorded.
static int gpu_bc(VkCtx *c, const uint8_t *blk, size_t n0, size_t n1, uint32_t w0, uint32_t h0, uint32_t w1, uint32_t h1,
                  uint32_t bw, uint32_t bh, int srgb, int bc, uint8_t *out) {
    VkFormat fmt = bc == 2 ? (srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK)
                           : (srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK);
    size_t bpb = bc == 2 ? 8 : 16;
    Img im;
    if (!make_image(c, w0, h0, n1 ? 2 : 1, fmt, &im)) return 0;
    size_t b0 = (size_t)((w0 + 3) / 4) * ((h0 + 3) / 4) * bpb, b1 = n1 ? (size_t)((w1 + 3) / 4) * ((h1 + 3) / 4) * bpb : 0;
    VkCommandBuffer cb = vk_upload_begin();
    VkBuffer sb, ib;
    VmaAllocation ia;
    uint8_t *stg = vk_staging((n0 + n1) * 16, &sb, NULL), *imap = vk_staging(b0 + b1, &ib, &ia);
    memcpy(stg, blk, (n0 + n1) * 16);
    to_general(cb, im.img);
    VkAstcRegion ar[2] = {{.src = 0, .w = w0, .h = h0, .slices = 1, .level = 0}};
    int nr = 1;
    if (n1) ar[nr++] = (VkAstcRegion){.src = n0 * 16, .w = w1, .h = h1, .slices = 1, .level = 1};
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int ok = vk_astc_record(cb, sb, (n0 + n1) * 16, ar, nr, bw, bh, srgb, bc, im.img,
                            VK_IMAGE_ASPECT_COLOR_BIT, VK_NULL_HANDLE);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    VkBufferImageCopy cp[2] = {{.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w0, h0, 1}},
                               {.bufferOffset = b0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1}, .imageExtent = {w1, h1, 1}}};
    vkCmdCopyImageToBuffer(cb, im.img, VK_IMAGE_LAYOUT_GENERAL, ib, nr, cp);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    vk_upload_end();
    vk_upload_flush(1);
    vmaInvalidateAllocation(c->vma, ia, 0, VK_WHOLE_SIZE);
    memcpy(out, imap, b0 + b1);
    vmaDestroyBuffer(c->vma, ib, ia);
    vk_defer_image(im.img, im.alloc);
    vk_upload_flush(1);
    return ok > 0;
}

// One BC3 block to 16 RGBA8 texels, as the format defines it (565 endpoints widened by bit replication; the colour
// block always in four-colour mode). pos: each texel's place on the line from endpoint 0 (0) to endpoint 1 (3).
static void bc3_decode(const uint8_t b[16], uint8_t t[16][4], uint8_t ep[2][3], int pos[16]) {
    uint8_t a[8] = {b[0], b[1]}, p[4][3];
    if (a[0] > a[1])
        for (int i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1] + 3) / 7);
    else {
        for (int i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1] + 2) / 5);
        a[6] = 0, a[7] = 255;
    }
    uint64_t ai = 0;
    for (int i = 0; i < 6; i++) ai |= (uint64_t)b[2 + i] << 8 * i;
    for (int k = 0; k < 2; k++) {
        unsigned v = b[8 + 2 * k] | b[9 + 2 * k] << 8, r = v >> 11, g = v >> 5 & 63, bl = v & 31;
        p[k][0] = (uint8_t)(r << 3 | r >> 2), p[k][1] = (uint8_t)(g << 2 | g >> 4), p[k][2] = (uint8_t)(bl << 3 | bl >> 2);
    }
    for (int ch = 0; ch < 3; ch++)
        p[2][ch] = (uint8_t)((2 * p[0][ch] + p[1][ch] + 1) / 3), p[3][ch] = (uint8_t)((p[0][ch] + 2 * p[1][ch] + 1) / 3);
    uint32_t ci = b[12] | b[13] << 8 | b[14] << 16 | (uint32_t)b[15] << 24;
    static const int code_pos[4] = {0, 3, 1, 2};
    for (int i = 0; i < 16; i++) {
        memcpy(t[i], p[ci >> 2 * i & 3], 3);
        t[i][3] = a[ai >> 3 * i & 7];
        pos[i] = code_pos[ci >> 2 * i & 3];
    }
    memcpy(ep, p, 6);
}

// BC3 is lossy, so these are bounds, not bit-exactness. In every block: alpha within a fourteenth of the block's alpha
// range (the eight-value mode's half step); each texel on the palette entry nearest its projection onto the endpoint
// line (within rounding, when the endpoints are apart); a block of one colour within the 565 rounding, alpha exact.
// Per level, a PSNR floor against astc_decode_ldr for gross damage: the random blocks are noise, so it is low (19 dB
// at worst for 5x5 level 1 on an RX 9070). *db: the level's PSNR.
enum { BC_MIN_DB = 15 };
static int bc_check(const uint8_t *want, const uint8_t *blocks, uint32_t w, uint32_t h, const char *what, double *db) {
    uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    double se = 0;
    for (uint32_t y4 = 0; y4 < by; y4++)
        for (uint32_t x4 = 0; x4 < bx; x4++) {
            uint8_t t[16][4], ep[2][3], lo[4] = {255, 255, 255, 255}, hi[4] = {0};
            int pos[16];
            bc3_decode(blocks + 16 * ((size_t)y4 * bx + x4), t, ep, pos);
            double axis[3], len2 = 0;
            for (int ch = 0; ch < 3; ch++) axis[ch] = (double)ep[1][ch] - ep[0][ch], len2 += axis[ch] * axis[ch];
            for (int pass = 0; pass < 2; pass++)
                for (int i = 0; i < 16; i++) {
                    uint32_t x = x4 * 4 + (i & 3), y = y4 * 4 + (i >> 2);
                    if (x >= w || y >= h) continue;
                    const uint8_t *s = want + 4 * ((size_t)y * w + x);
                    if (!pass) {
                        for (int ch = 0; ch < 4; ch++) lo[ch] = s[ch] < lo[ch] ? s[ch] : lo[ch], hi[ch] = s[ch] > hi[ch] ? s[ch] : hi[ch];
                        continue;
                    }
                    int flat = !memcmp(lo, hi, 4), lim[4] = {5, 3, 5, 0};
                    if (len2 >= 64) {   // shorter lines: rounding alone can move a texel to the next entry
                        double proj = 0;
                        for (int ch = 0; ch < 3; ch++) proj += ((double)s[ch] - ep[0][ch]) * axis[ch];
                        proj = proj / len2 * 3;
                        proj = proj < 0 ? 0 : proj > 3 ? 3 : proj;
                        if (fabs(pos[i] - proj) > 0.75) {   // a wrong code mapping is a whole place off
                            printf("probe-astc: FAIL BC3 %s: texel (%u,%u) on palette place %d, its projection is %.2f\n",
                                   what, x, y, pos[i], proj);
                            return 1;
                        }
                    }
                    for (int ch = 0; ch < 4; ch++) {
                        int d = abs((int)t[i][ch] - s[ch]);
                        se += (double)d * d;
                        if ((ch == 3 && d > (hi[3] - lo[3]) / 14 + 1) || (flat && d > lim[ch])) {
                            printf("probe-astc: FAIL BC3 %s: texel (%u,%u) channel %d: astc %02x%02x%02x%02x bc3 %02x%02x%02x%02x\n",
                                   what, x, y, ch, s[0], s[1], s[2], s[3], t[i][0], t[i][1], t[i][2], t[i][3]);
                            return 1;
                        }
                    }
                }
        }
    double mse = se / ((double)w * h * 4);
    *db = mse > 0 ? 10 * log10(255.0 * 255.0 / mse) : 99;
    if (*db < BC_MIN_DB) {
        printf("probe-astc: FAIL BC3 %s: PSNR %.1f dB, below %d\n", what, *db, BC_MIN_DB);
        return 1;
    }
    return 0;
}

// One BC1 block to 16 RGBA8 texels: the colour part of bc3_decode (the four-colour mode, as bc1_encode.comp
// writes it), alpha 255 — BC1_RGB samples alpha as 1.
static void bc1_decode(const uint8_t b[8], uint8_t t[16][4], uint8_t ep[2][3], int pos[16]) {
    uint8_t p[4][3];
    for (int k = 0; k < 2; k++) {
        unsigned v = b[2 * k] | b[2 * k + 1] << 8, r = v >> 11, g = v >> 5 & 63, bl = v & 31;
        p[k][0] = (uint8_t)(r << 3 | r >> 2), p[k][1] = (uint8_t)(g << 2 | g >> 4), p[k][2] = (uint8_t)(bl << 3 | bl >> 2);
    }
    for (int ch = 0; ch < 3; ch++)
        p[2][ch] = (uint8_t)((2 * p[0][ch] + p[1][ch] + 1) / 3), p[3][ch] = (uint8_t)((p[0][ch] + 2 * p[1][ch] + 1) / 3);
    uint32_t ci = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
    static const int code_pos[4] = {0, 3, 1, 2};
    for (int i = 0; i < 16; i++) {
        memcpy(t[i], p[ci >> 2 * i & 3], 3);
        t[i][3] = 255;
        pos[i] = code_pos[ci >> 2 * i & 3];
    }
    memcpy(ep, p, 6);
}

// The BC1 bounds: the colour ones of bc_check (each texel on the palette entry nearest its projection onto
// the endpoint line, a block of one colour within the 565 rounding) and alpha exact — the corpus is opaque by
// construction and BC1_RGB samples 1. The same PSNR floor as BC3: the colour encoding is the same one.
static int bc1_check(const uint8_t *want, const uint8_t *blocks, uint32_t w, uint32_t h, const char *what, double *db) {
    uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    double se = 0;
    for (uint32_t y4 = 0; y4 < by; y4++)
        for (uint32_t x4 = 0; x4 < bx; x4++) {
            uint8_t t[16][4], ep[2][3], lo[3] = {255, 255, 255}, hi[3] = {0};
            int pos[16];
            bc1_decode(blocks + 8 * ((size_t)y4 * bx + x4), t, ep, pos);
            double axis[3], len2 = 0;
            for (int ch = 0; ch < 3; ch++) axis[ch] = (double)ep[1][ch] - ep[0][ch], len2 += axis[ch] * axis[ch];
            for (int pass = 0; pass < 2; pass++)
                for (int i = 0; i < 16; i++) {
                    uint32_t x = x4 * 4 + (i & 3), y = y4 * 4 + (i >> 2);
                    if (x >= w || y >= h) continue;
                    const uint8_t *s = want + 4 * ((size_t)y * w + x);
                    if (!pass) {
                        for (int ch = 0; ch < 3; ch++) lo[ch] = s[ch] < lo[ch] ? s[ch] : lo[ch], hi[ch] = s[ch] > hi[ch] ? s[ch] : hi[ch];
                        continue;
                    }
                    if (s[3] != 0xFF) {
                        printf("probe-astc: FAIL BC1 %s: texel (%u,%u) decodes alpha %02x; the corpus must be opaque\n",
                               what, x, y, s[3]);
                        return 1;
                    }
                    int flat = !memcmp(lo, hi, 3), lim[3] = {5, 3, 5};
                    if (len2 >= 64) {   // shorter lines: rounding alone can move a texel to the next entry
                        double proj = 0;
                        for (int ch = 0; ch < 3; ch++) proj += ((double)s[ch] - ep[0][ch]) * axis[ch];
                        proj = proj / len2 * 3;
                        proj = proj < 0 ? 0 : proj > 3 ? 3 : proj;
                        if (fabs(pos[i] - proj) > 0.75) {   // a wrong code mapping is a whole place off
                            printf("probe-astc: FAIL BC1 %s: texel (%u,%u) on palette place %d, its projection is %.2f\n",
                                   what, x, y, pos[i], proj);
                            return 1;
                        }
                    }
                    for (int ch = 0; ch < 3; ch++) {
                        int d = abs((int)t[i][ch] - s[ch]);
                        se += (double)d * d;
                        if (flat && d > lim[ch]) {
                            printf("probe-astc: FAIL BC1 %s: texel (%u,%u) channel %d: astc %02x%02x%02x bc1 %02x%02x%02x\n",
                                   what, x, y, ch, s[0], s[1], s[2], t[i][0], t[i][1], t[i][2]);
                            return 1;
                        }
                    }
                }
        }
    double mse = se / ((double)w * h * 3);
    *db = mse > 0 ? 10 * log10(255.0 * 255.0 / mse) : 99;
    if (*db < BC_MIN_DB) {
        printf("probe-astc: FAIL BC1 %s: PSNR %.1f dB, below %d\n", what, *db, BC_MIN_DB);
        return 1;
    }
    return 0;
}

// The BC1 case: one region of blocks the scan calls opaque — the only ones the runtime encodes to BC1 —
// decoded to the RGBA8 reference, encoded to a BC1 image and held to the bounds above. Valid blocks without
// alpha-carrying endpoints make up most of it; error blocks and opaque void extents the rest, as any storage
// the scan accepts can hold them.
static int test_bc1(VkCtx *c, uint32_t bw, uint32_t bh, int srgb) {
    enum { NB = 32 };   // 1024 ASTC blocks
    uint32_t w = NB * bw, h = NB * bh;
    size_t n = (size_t)NB * NB;
    uint8_t *blk = malloc(n * 16), tmp[12 * 12 * 4];
    size_t k = 0;
    for (int tries = 0; tries < 4000000 && k < n - n / 4; tries++) {   // valid opaque blocks first
        uint64_t v[2] = {rnd64(), rnd64()};
        uint8_t *b = (uint8_t *)v;
        uint32_t m = (uint32_t)(v[0] & 0x7FF);
        if ((m & 0x1FF) == 0x1FC || !astc_decode_block(b, bw, bh, tries & 1, tmp)) continue;
        if (astc_block_alpha_possible(b, bw, bh)) continue;
        memcpy(blk + 16 * k++, b, 16);
    }
    for (int tries = 0; tries < 4000000 && k < n; tries++) {   // then any opaque 16 bytes: error blocks, void extents
        uint64_t v[2] = {rnd64(), rnd64()};
        if (astc_block_alpha_possible((const uint8_t *)v, bw, bh)) continue;
        memcpy(blk + 16 * k++, (const uint8_t *)v, 16);
    }
    int bad = 0;
    if (k < n) {
        printf("probe-astc: FAIL BC1 %ux%u %s: only %zu of %zu opaque blocks generated\n", bw, bh,
               srgb ? "sRGB" : "linear", k, n);
        bad = 1;
    }
    if (!bad) {
        uint8_t *want = malloc((size_t)w * h * 4), *out = malloc((size_t)((w + 3) / 4) * ((h + 3) / 4) * 8);
        astc_decode_ldr(blk, w, h, bw, bh, srgb, want);
        char what[48];
        double db = 0;
        snprintf(what, sizeof what, "%ux%u %s", bw, bh, srgb ? "sRGB" : "linear");
        if (!gpu_bc(c, blk, n, 0, w, h, 0, 0, bw, bh, srgb, 2, out)) {
            printf("probe-astc: FAIL BC1 %s: the encode was not recorded\n", what);
            bad = 1;
        } else {
            bad = bc1_check(want, out, w, h, what, &db);
            if (!bad && db < g_bc1_db) g_bc1_db = db;
            g_bc1_cases += !bad;
        }
        free(want), free(out);
    }
    free(blk);
    return bad;
}

static int first_diff(const uint8_t *a, const uint8_t *b, size_t texels, size_t *at) {
    for (size_t i = 0; i < texels; i++)
        if (memcmp(a + 4 * i, b + 4 * i, 4)) { *at = i; return 1; }
    return 0;
}

static int test_case(VkCtx *c, uint32_t bw, uint32_t bh, int srgb) {
    // level 0: 37 x 23 blocks, cropped by 1-3 texels; level 1 half of that, other blocks
    uint32_t w0 = 37 * bw - 1 - (bw > 5), h0 = 23 * bh - 2, w1 = w0 > 1 ? w0 >> 1 : 1, h1 = h0 > 1 ? h0 >> 1 : 1;
    size_t n0 = (size_t)((w0 + bw - 1) / bw) * ((h0 + bh - 1) / bh), n1 = (size_t)((w1 + bw - 1) / bw) * ((h1 + bh - 1) / bh);
    uint8_t *blk = malloc((n0 + n1) * 16);
    make_blocks(blk, n0, bw, bh);
    make_blocks(blk + n0 * 16, n1, bw, bh);
    size_t t0 = (size_t)w0 * h0, t1 = (size_t)w1 * h1;
    uint8_t *want = malloc((t0 + t1) * 4), *rb = malloc((t0 + t1) * 4), *im = malloc((t0 + t1) * 4);
    memset(rb, 0x5A, (t0 + t1) * 4), memset(im, 0xA5, (t0 + t1) * 4);
    astc_decode_ldr(blk, w0, h0, bw, bh, srgb, want);
    astc_decode_ldr(blk + n0 * 16, w1, h1, bw, bh, srgb, want + t0 * 4);
    int bad = 0;
    // The alpha scan oracle (tex_astc.c astc_block_alpha_possible, the BC1 choice of vk_texture.c): every
    // block the scan calls opaque must decode with alpha 255 in every texel. Blocks it calls alpha-carrying
    // are only counted: there the scan is allowed to be conservative.
    {
        size_t op = 0;
        for (size_t i = 0; i < n0 + n1 && !bad; i++) {
            if (astc_block_alpha_possible(blk + 16 * i, bw, bh)) continue;
            op++;
            uint8_t tmp[12 * 12 * 4];
            astc_decode_block(blk + 16 * i, bw, bh, srgb, tmp);
            for (uint32_t t = 0; t < bw * bh; t++)
                if (tmp[4 * t + 3] != 0xFF) {
                    printf("probe-astc: FAIL %ux%u %s: the scan calls block %zu opaque; texel %u decodes alpha "
                           "%02x\n", bw, bh, srgb ? "sRGB" : "linear", i, t, tmp[4 * t + 3]);
                    bad = 1;
                    break;
                }
        }
        g_scan_blocks += n0 + n1, g_scan_opaque += op;
    }
    if (!gpu_decode(c, blk, n0, n1, w0, h0, w1, h1, bw, bh, srgb, rb, im, NULL)) {
        printf("probe-astc: FAIL %ux%u %s: the GPU decode was not recorded\n", bw, bh, srgb ? "sRGB" : "linear");
        bad = 1;
    }
    for (int lv = 0; lv < 2 && !bad; lv++) {
        size_t off = lv ? t0 * 4 : 0, n = lv ? t1 : t0, at;
        uint32_t w = lv ? w1 : w0;
        for (int which = 0; which < 2 && !bad; which++) {
            const uint8_t *got = (which ? im : rb) + off;
            if (first_diff(want + off, got, n, &at)) {
                const uint8_t *e = want + off + 4 * at, *g = got + 4 * at;
                printf("probe-astc: FAIL %ux%u %s level %d (%s): texel (%zu,%zu) cpu %02x%02x%02x%02x gpu %02x%02x%02x%02x\n",
                       bw, bh, srgb ? "sRGB" : "linear", lv, which ? "image" : "decoder output", at % w, at / w,
                       e[0], e[1], e[2], e[3], g[0], g[1], g[2], g[3]);
                bad = 1;
            }
        }
    }
    if (!bad && vk_astc_bc()) {
        size_t b0 = (size_t)((w0 + 3) / 4) * ((h0 + 3) / 4) * 16, b1 = (size_t)((w1 + 3) / 4) * ((h1 + 3) / 4) * 16;
        uint8_t *bc = malloc(b0 + b1);
        char what[2][48];
        double db[2] = {0, 0};
        for (int lv = 0; lv < 2; lv++) snprintf(what[lv], sizeof what[lv], "%ux%u %s level %d", bw, bh, srgb ? "sRGB" : "linear", lv);
        if (!gpu_bc(c, blk, n0, n1, w0, h0, w1, h1, bw, bh, srgb, 1, bc)) {
            printf("probe-astc: FAIL BC3 %s: the encode was not recorded\n", what[0]);
            bad = 1;
        }
        bad = bad || bc_check(want, bc, w0, h0, what[0], &db[0]) || bc_check(want + t0 * 4, bc + b0, w1, h1, what[1], &db[1]);
        for (int lv = 0; lv < 2 && !bad; lv++)
            if (db[lv] < g_bc_db) g_bc_db = db[lv];
        g_bc_cases += !bad;
        free(bc);
    }
    g_blocks += n0 + n1, g_texels += t0 + t1;
    free(blk), free(want), free(rb), free(im);
    return bad;
}

// One 2048 x 2048 texture of valid blocks: the CPU worker-pool decode against the GPU time of the dispatches alone
// (vk_astc.c timestamps, AC_ASTC_TIME) and the wall time of the upload batch.
static void timing(VkCtx *c, uint32_t bw, uint32_t bh) {
    uint32_t w = 2048, h = 2048;
    size_t n = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh);
    uint8_t *blk = malloc(n * 16), *out = malloc((size_t)w * h * 4), tmp[12 * 12 * 4];
    for (size_t k = 0; k < 4096;) {   // valid, non-constant blocks: error blocks would flatter the GPU
        uint64_t v[2] = {rnd64(), rnd64()};
        if ((v[0] & 0x1FF) != 0x1FC && astc_decode_block((uint8_t *)v, bw, bh, false, tmp)) memcpy(blk + 16 * k++, v, 16);
    }
    for (size_t k = 4096; k < n; k++) memcpy(blk + 16 * k, blk + 16 * (k % 4096), 16);
    double a = now_ms();
    astc_decode_ldr_mt(blk, w, h, bw, bh, false, out);
    double cpu = now_ms() - a, wall = 0;
    vk_astc_time_enable(1);
    uint64_t t0, c0, t1, c1;
    double u0, u1;
    vk_astc_time_get(bw, bh, &t0, &c0, &u0);
    for (int i = 0; i < 4; i++) gpu_decode(c, blk, n, 1, w, h, 1, 1, bw, bh, 0, NULL, NULL, i ? &wall : NULL);
    vk_astc_time_get(bw, bh, &t1, &c1, &u1);
    vk_astc_time_enable(0);
    double mtex = (double)(t1 - t0) / 1e6;
    printf("probe-astc: 2048x2048 %ux%u: CPU (worker pool) %.1f ms; GPU dispatches %.0f us per M texel (%llu decodes "
           "timed, %.2f M texels), batch wall %.1f ms\n", bw, bh, cpu, mtex > 0 ? (u1 - u0) / mtex : 0.0,
           (unsigned long long)(c1 - c0), mtex, wall);
    free(blk), free(out);
}

int probe_astc(void) {
    plat_setenv("AC_ASTC_BC", "bc1");   // builds both encoders; the RGBA8 cases pass bc 0 and use neither
    if (!vk_init()) { printf("probe-astc: FAIL vk_init\n"); return 1; }
    VkCtx *c = vk_ctx();
    printf("probe-astc: GPU %s, textureCompressionASTC_LDR %d, mode %d\n", c->props.deviceName, c->has_astc_ldr,
           vk_astc_mode());
    int bad = 0;
    if (!vk_astc_gpu()) {
        printf("probe-astc: FAIL the compute decoder is not available (AC_ASTC=cpu or pipeline creation failed)\n");
        bad = 1;
    }
    for (int i = 0; i < 14 && !bad; i++)
        for (int s = 0; s < 2 && !bad; s++) bad = test_case(c, dims[i][0], dims[i][1], s);
    for (int i = 0; i < 14 && !bad; i++)
        for (int s = 0; s < 2 && !bad; s++) bad = test_bc1(c, dims[i][0], dims[i][1], s);
    for (int b = 0; b < NB && !bad; b++)
        if (!g_bucket[b]) { printf("probe-astc: FAIL no valid %s block was generated\n", bucket_name[b]); bad = 1; }
    if (!bad && !vk_astc_bc()) printf("probe-astc: no BC encoder available on this device: not checked\n");
    else if (!bad) {
        printf("probe-astc: BC3 encode: %llu cases within the bounds, lowest PSNR %.1f dB (floor %d)\n",
               (unsigned long long)g_bc_cases, g_bc_db, BC_MIN_DB);
        if (vk_astc_bc() == 2)
            printf("probe-astc: BC1 encode: %llu cases within the bounds, lowest PSNR %.1f dB (floor %d)\n",
                   (unsigned long long)g_bc1_cases, g_bc1_db, BC_MIN_DB);
        else
            printf("probe-astc: BC1 encoder not available on this device: not checked\n");
    }
    if (!bad) {
        timing(c, 4, 4);
        timing(c, 6, 6);
        timing(c, 8, 8);
        timing(c, 12, 12);
    }
    if (!bad && g_scan_blocks)
        printf("probe-astc: alpha scan: %llu blocks scanned, %llu called opaque (%.1f%%), each verified to "
               "decode with alpha 255\n",
               (unsigned long long)g_scan_blocks, (unsigned long long)g_scan_opaque,
               100.0 * (double)g_scan_opaque / (double)g_scan_blocks);
    vk_mem_shutdown();
    vk_shutdown();
    long errors = vk_validation_errors_total();
    if (bad || errors) {
        printf("probe-astc: FAILED (%s, %ld validation errors)\n", bad ? "a check failed" : "checks passed", errors);
        return 1;
    }
    printf("probe-astc: passed (14 footprints x linear/sRGB, %llu blocks, %llu texels bit-exact with astc_decode_ldr; "
           "valid blocks by kind: 1p %llu, 2p %llu, 3p %llu, 4p %llu, dual 1p %llu, dual 2p %llu, dual 3p %llu; "
           "%llu void extents, %llu random error blocks; 0 validation errors)\n",
           (unsigned long long)g_blocks, (unsigned long long)g_texels, (unsigned long long)g_bucket[0],
           (unsigned long long)g_bucket[1], (unsigned long long)g_bucket[2], (unsigned long long)g_bucket[3],
           (unsigned long long)g_bucket[4], (unsigned long long)g_bucket[5], (unsigned long long)g_bucket[6],
           (unsigned long long)g_void, (unsigned long long)g_err);
    return 0;
}
