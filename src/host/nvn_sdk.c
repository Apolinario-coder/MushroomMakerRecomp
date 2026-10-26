/* NVN functions answered by the SDK's own code (lifted from binaries\13.0.4\sdk.elf by sdklift.py).
 *
 *   nvnTextureBuilderGetStorageAlignment [sdk 0x4278b0]
 *   nvnTextureBuilderGetStorageSize      [sdk 0x428760] -> layout [0x427ed0] -> [0x410e60, 0x410f00, 0x411330]
 *
 * Both read the NVNtextureBuilder in its real layout and a handful of fields of the NVNdevice, and end in the GPU
 * library's choice of a memory "kind": f [0x420410] -> NvRmGpuDeviceChooseKind [0x3fbbc0] -> device method +0x30
 * [0x408e10: the object at device +0x140] -> that object's method +0x80 [0x401ae0] and its choosers. All of that is
 * lifted code; this file only supplies the objects it reads, with the values the SDK's own start-up code gives them:
 *
 * NVNdevice (model, only the fields the two functions read):
 *   +0x90    u32  0x10000   big page size of the GPU address space. The device copies it [0x41c40c] from the block
 *                           filled at [0x4206c0]: field +0x14 of NvRmGpuAddressSpaceGetInfo [0x403930] = address-space
 *                           object +0x1c = first word of the attributes given to NvRmGpuAddressSpaceCreate [0x402880
 *                           at 0x4028b8], and NVN passes the constant 0x10000 there [0x420228 / 0x420240]. The driver
 *                           is asked for exactly that size (ioctl 0x40284109 at [0x402ac8], big_page_size = object
 *                           +0x1c); the reference accepts it (suyu nvdrv/devices/nvhost_as_gpu.cpp AllocAsEx:
 *                           0x10000 is one of VM::SUPPORTED_BIG_PAGE_SIZES).
 *   +0x928   ptr  the GPU device object (NVN keeps {lib, device, address space} at +0x920 [0x4201f4 .. 0x420244])
 *   +0x19a8  16 bytes copied from the constant at [sdk 0x9f19a0] = {0, 0, 1, 0} [0x41c448 / 0x41c45c]. Each word can
 *                           be replaced by a named setting [0x420580], but the lookup behind it is the function
 *                           `return NULL` [0x41e080, installed at 0x41e5c0]: the constants stay.
 *   +0x1ac8  u32  0         [0x41ce58: `str wzr`, the only store]
 *   +0x1acc  u8   1         [0x41cef8 .. 0x41cf18: 1 unless (+0x19b0 has bit 2 and not bit 1); +0x19b0 is 1]
 * GPU device object (class with method table 0xc11978): only +0x140 is read here.
 * Chip object (class 0xc114e8, made by [0x40cc90] with the chip descriptor at +8): the descriptor is chosen by the GPU
 *   architecture | implementation the driver reports [0x404d6c .. 0x404dbc]; sdk.elf has two descriptors, 0x12b
 *   [0xc11e88] and 0x12e [0xc11f20], and their kind tables (descriptor +0x48, 0x100 entries of 2 bytes; 0xa927a1 and
 *   0xa92bd9) are byte-identical. The reference reports arch 0x120, impl 0xb (suyu nvhost_ctrl_gpu.cpp) = 0x12b.
 *   The choosers read only the method table and the descriptor (checked on the disassembly of 0x401ae0 .. 0x402760).
 *
 * Nothing in this file computes a size or an alignment. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "sdklift.h"
#include "nvn_sdk.h"
#include "nvn_c4.h"
#include "nvn_c5.h"

#define SDK(a) (SDK_BASE + (uint64_t)(a))
#define P(v) ((uint64_t)(uintptr_t)(v))

static uint8_t g_dev[0x1b00];            /* NVNdevice model */
static uint64_t g_gpu_dev[0x148 / 8 + 1];/* GPU device object: method table, chip object at +0x140 */
static uint64_t g_chip[7];               /* chip object, 0x38 bytes [0x40ccd0] */
static volatile LONG g_state;            /* 0 not built, 1 building, 2 ready */

static void build_model(void) {
    if (!sdk_image_ready()) {
        printf("\nnvn_sdk: sdk.elf is not mapped; the texture storage rules are the SDK's own code and cannot be answered without it.\n");
        report_and_exit(3);
    }
    g_chip[0] = SDK(0xc114e8);
    g_chip[1] = SDK(0xc11e88);
    g_gpu_dev[0] = SDK(0xc11978);
    g_gpu_dev[0x140 / 8] = P(g_chip);
    *(uint32_t *)(g_dev + 0x90) = 0x10000;
    *(uint64_t *)(g_dev + 0x928) = P(g_gpu_dev);
    memcpy(g_dev + 0x19a8, (const void *)(uintptr_t)SDK(0x9f19a0), 16);
    *(uint32_t *)(g_dev + 0x1ac8) = 0;
    g_dev[0x1acc] = (uint8_t)(((*(uint32_t *)(g_dev + 0x19b0) & 6) == 4) ? 0 : 1);       /* [0x41cf08 .. 0x41cf18] */
}
static void model(void) {
    for (;;) {
        LONG s = InterlockedCompareExchange(&g_state, 1, 0);
        if (s == 2) return;
        if (s == 0) { build_model(); InterlockedExchange(&g_state, 2); return; }
        Sleep(0);
    }
}

/* The real NVNtextureBuilder, as the setters of sdk.elf write it [0x427600 .. 0x427740, 0x427b90]. */
static void real_builder(uint8_t *rb, const NvnSdkTexBuilder *b) {
    memset(rb, 0, 0x80);
    *(uint64_t *)(rb + 8) = P(g_dev);
    *(uint32_t *)(rb + 0x20) = b->flags;
    *(uint32_t *)(rb + 0x24) = b->target;
    *(uint32_t *)(rb + 0x28) = b->width;
    *(uint32_t *)(rb + 0x2c) = b->height;
    *(uint32_t *)(rb + 0x30) = b->depth;
    *(uint32_t *)(rb + 0x34) = b->levels;
    *(uint32_t *)(rb + 0x38) = b->format;
    *(uint32_t *)(rb + 0x3c) = b->samples;
    if (b->has_layout) *(uint16_t *)(rb + 0x60) = (uint16_t)(1u | ((b->layout & 0xfffu) << 4));   /* [0x427b90] */
    *(uint32_t *)(rb + 0x64) = b->stride;
    *(uint32_t *)(rb + 0x68) = b->glname;
}

static uint64_t run(Ctx *c, uint64_t fn, const NvnSdkTexBuilder *b) {
    uint8_t rb[0x80];
    model();
    real_builder(rb, b);
    c->x[0] = P(rb);
    sdk_dispatch(c, SDK(fn));
    return c->x[0];
}
uint64_t nvn_sdk_texture_storage_alignment(Ctx *c, const NvnSdkTexBuilder *b) { return run(c, 0x4278b0, b); }
uint64_t nvn_sdk_texture_storage_size(Ctx *c, const NvnSdkTexBuilder *b) { return run(c, 0x428760, b); }
uint32_t nvn_sdk_texture_storage_class(Ctx *c, const NvnSdkTexBuilder *b) { return (uint32_t)run(c, 0x427a10, b); }

/* ---- close4: raw storage class, sparse tile layout, and the texture forms.
 *   nvnTextureBuilderGetRawStorageClass  [sdk 0x427b30]  flags byte & 0x50 -> 1, else by format through the table at [0xa97334]
 *   nvnTextureGetRawStorageClass         [sdk 0x429870]  the same on the NVNtexture (table [0xa97474], identical entries)
 *   nvnTextureGetStorageClass            [sdk 0x428c00]  0x100 | byte +0x70 of the NVNtexture
 *   nvnTextureBuilderGetSparseTileLayout [sdk 0x4290b0] -> [0x410e60] -> [0x411090]
 *   nvnTextureGetSparseTileLayout        [sdk 0x429150] -> the same two
 * NVNtextureSparseTileLayout is the 16 bytes [0x411090] writes: +0 number of tiled levels, +4 / +8 / +0xc tile width, height, depth.
 * The NVNtexture these read is the one nvnTextureInitialize [sdk 0x4287d0] builds from a builder that has no OpenGL texture name:
 * +8 device, +0x20 .. +0x50 copied from the builder [0x42888c .. 0x4288f0], +0x54 two format bits [0x428900 .. 0x428930], and the
 * layout block at +0x58 written by [0x427ed0](texture + 0x58, builder) (so +0x64 = bytes per element, +0x6c / +0x6d = block
 * width / height, +0x70 = the GPU memory kind). That function is the lifted code; this file only copies the fields. A builder
 * with an OpenGL texture name takes the other path of nvnTextureInitialize [0x428810], which asks the GL interop object; the
 * port has none (nvnSyncCreateGLSync etc. answer "no interop"), so such a texture is not modelled: the caller stops. */
static int real_texture(Ctx *c, uint8_t *rt, uint8_t *rb, const NvnSdkTexBuilder *b) {
    if (b->glname) return 0;
    model();
    real_builder(rb, b);
    memset(rt, 0, 0x100);
    memcpy(rt + 8, rb + 8, 8);
    memcpy(rt + 0x20, rb + 0x20, 0x34);
    const uint8_t *fr = (const uint8_t *)(uintptr_t)(SDK(0xc26568) + 0x30ull * (b->format < NVN_SDK_FORMAT_COUNT ? b->format : 0));
    rt[0x54] = (uint8_t)((fr[0xe] ? 1 : 0) | (fr[0xf] ? 2 : 0));
    c->x[0] = P(rt + 0x58); c->x[1] = P(rb);
    sdk_dispatch(c, SDK(0x427ed0));
    return 1;
}
static uint64_t run_tex(Ctx *c, uint64_t fn, const NvnSdkTexBuilder *b, uint64_t x1, int *ok) {
    uint8_t rb[0x80], rt[0x100];
    *ok = real_texture(c, rt, rb, b);
    if (!*ok) return 0;
    c->x[0] = P(rt); c->x[1] = x1;
    sdk_dispatch(c, SDK(fn));
    return c->x[0];
}
uint32_t nvn_sdk_texture_raw_storage_class(Ctx *c, const NvnSdkTexBuilder *b) { return (uint32_t)run(c, 0x427b30, b); }
void nvn_sdk_texture_sparse_layout(Ctx *c, const NvnSdkTexBuilder *b, uint32_t out[4]) {
    uint8_t rb[0x80];
    model();
    real_builder(rb, b);
    c->x[0] = P(rb); c->x[1] = P(out);
    sdk_dispatch(c, SDK(0x4290b0));
}
int nvn_sdk_tex_storage_class(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out) { int ok; *out = (uint32_t)run_tex(c, 0x428c00, b, 0, &ok); return ok; }
int nvn_sdk_tex_raw_storage_class(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out) { int ok; *out = (uint32_t)run_tex(c, 0x429870, b, 0, &ok); return ok; }
int nvn_sdk_tex_sparse_layout(Ctx *c, const NvnSdkTexBuilder *b, uint32_t out[4]) { int ok; run_tex(c, 0x429150, b, P(out), &ok); return ok; }
/* nvnTextureGetStorageSize [sdk 0x429720] = `ldr w0, [x0, #0x78]`: the 32-bit word at +0x20 of the layout block that [0x427ed0] wrote
 * at texture +0x58 [0x428934] (the same word nvnTextureBuilderGetStorageSize [0x4287ac] returns as 64 bits, without the builder's
 * format-flag test [0x4287b4]). close5. */
int nvn_sdk_tex_storage_size(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out) {
    uint8_t rb[0x80], rt[0x100];
    if (!real_texture(c, rt, rb, b)) return 0;
    memcpy(out, rt + 0x78, 4);
    return 1;
}

/* Format table of the NVN library: 0x30-byte records at [sdk 0xc26568] (the pointer every texture function loads from
 * [0xc3b338]); bit 0 of the first byte = "this format exists". */
int nvn_sdk_format_known(uint32_t format) {
    model();
    return format < NVN_SDK_FORMAT_COUNT && (*(const uint8_t *)(uintptr_t)(SDK(0xc26568) + 0x30ull * format) & 1);
}

/* Self-test: cases whose answer was read by hand in the disassembly, plus a sweep that must not stop anywhere
 * (every method the choosers reach must have been lifted). SSBU_NVN_STORAGE_TABLE=<file> writes the sweep. */
int nvn_sdk_selftest(Ctx *c) {
    int bad = 0, n = 0;
    NvnSdkTexBuilder b;
    const char *out = getenv("SSBU_NVN_STORAGE_TABLE");
    FILE *f = out && *out ? fopen(out, "w") : NULL;
#define SET(fl, tg, w_, h_, d_, lv, fm, sm) do { memset(&b, 0, sizeof b); b.flags = (fl); b.target = (tg); b.width = (w_); b.height = (h_); b.depth = (d_); b.levels = (lv); b.format = (fm); b.samples = (sm); } while (0)
#define WANT(what, got, want) do { uint64_t g_ = (got), w_ = (want); n++; if (g_ != w_) { bad++; printf("  FAIL nvn storage %s: 0x%llx, expected 0x%llx\n", what, (unsigned long long)g_, (unsigned long long)w_); } } while (0)
    SET(0x40, 1, 64, 64, 1, 1, 0x25, 0); WANT("alignment, flag bit 6 [0x427918]", nvn_sdk_texture_storage_alignment(c, &b), 0x80);
    SET(0x10, 1, 64, 64, 1, 1, 0x25, 0); WANT("alignment, flag bit 4 [0x427920]", nvn_sdk_texture_storage_alignment(c, &b), 0x20);
    b.stride = 256; WANT("size, flag bit 4 = stride x height [0x428020]", nvn_sdk_texture_storage_size(c, &b), 256 * 64);
    for (uint32_t fm = 1; fm < NVN_SDK_FORMAT_COUNT; fm++) {
        if (!nvn_sdk_format_known(fm)) continue;
        uint32_t bpe = *(const uint8_t *)(uintptr_t)(SDK(0xc26568) + 0x30ull * fm + 0x10);
        SET(0, 10, 100, 1, 1, 1, fm, 0);
        WANT("alignment, buffer target = bytes per element, 12 counts as 4 [0x4278d8]", nvn_sdk_texture_storage_alignment(c, &b), bpe == 12 ? 4 : bpe);
        WANT("size, buffer target = width x bytes per element [0x427fe8]", nvn_sdk_texture_storage_size(c, &b), 100ull * bpe);
        static const uint32_t flags[] = {0, 8, 0x100, 0x108, 0x1008};
        for (int tg = 0; tg < 10; tg++) for (int fi = 0; fi < 5; fi++) for (uint32_t sm = 0; sm <= 4; sm += 2) {
            if ((tg == 5 || tg == 6) != (sm != 0)) continue;                 /* multisample targets take a sample count, the others none */
            SET(flags[fi], (uint32_t)tg, 128, tg == 0 || tg == 3 ? 1 : 64, tg == 8 ? 1 : tg == 9 ? 6 : tg == 2 || tg == 4 || tg == 6 || tg == 3 ? 4 : 1, 1, fm, sm);
            uint64_t al = nvn_sdk_texture_storage_alignment(c, &b), sz = nvn_sdk_texture_storage_size(c, &b);
            uint32_t cl = nvn_sdk_texture_storage_class(c, &b);
            n++;
            if (al != 0x200 && al != 0x10000) { bad++; printf("  FAIL nvn storage alignment 0x%llx for format 0x%x target %d flags 0x%x: neither 0x200 nor the page size [0x4279f0 .. 0x427910]\n", (unsigned long long)al, fm, tg, flags[fi]); }
            if (al == 0x10000 && (sz & 0xffff)) { bad++; printf("  FAIL nvn storage size 0x%llx not a multiple of the page size for format 0x%x target %d [0x428200]\n", (unsigned long long)sz, fm, tg); }
            if (f) fprintf(f, "%x\t%d\t%x\t%u\t%llx\t%llx\t%x\n", fm, tg, flags[fi], sm, (unsigned long long)al, (unsigned long long)sz, cl);
        }
    }
    if (f) fclose(f);
    /* close4: storage class (raw and GPU kind) and sparse tile layout, builder and texture forms. Answers read by hand first,
     * then a sweep written to SSBU_NVN_CLASS_TABLE=<file> for tools_audit/patches/close4/uc_nvn_class.py (the emulator on sdk.elf). */
    {
        uint32_t v, sp[4];
        static const struct { uint32_t flags, format, want; const char *where; } raw[] = {
            {0x10, 0x25, 1, "flag bit 4 [0x427b40]"}, {0x40, 0x25, 1, "flag bit 6 [0x427b40]"}, {0, 0, 0, "format 0: table entry 0 = the ret at [0x427b7c], w0 still the format"},
            {0, 1, 2, "table [0xa97334] entry 1 -> [0x427b78]"}, {0, 0x30, 2, "entry 0x30 -> [0x427b78]"}, {0, 0x31, 3, "entry 0x31 -> [0x427b68]"}, {0, 0x32, 3, "entry 0x32 -> [0x427b68]"},
            {0, 0x33, 4, "entry 0x33 -> [0x427b70]"}, {0, 0x34, 2, "entry 0x34 -> [0x427b78]"}, {0, 0x35, 4, "entry 0x35 -> [0x427b70]"}, {0, 0x36, 5, "entry 0x36 -> [0x427b80]"},
            {0, 0x37, 2, "format above 0x36 [0x427b50 -> 0x427b78]"}, {0, 0x60, 2, "format above 0x36"},
        };
        for (size_t i = 0; i < sizeof raw / sizeof raw[0]; i++) {
            SET(raw[i].flags, 1, 64, 64, 1, 1, raw[i].format, 0);
            WANT(raw[i].where, nvn_sdk_texture_raw_storage_class(c, &b), raw[i].want);
            if (raw[i].format && nvn_sdk_format_known(raw[i].format)) { v = ~0u; WANT("texture form: no OpenGL name", nvn_sdk_tex_raw_storage_class(c, &b, &v), 1); WANT("texture form [0x429870], same table at [0xa97474]", v, raw[i].want); }
        }
        SET(0x10, 1, 64, 64, 1, 1, 0x25, 0); b.stride = 256; v = 0;
        WANT("texture class, flag bit 4: kind byte cleared [0x428044], 0x100 | 0 [0x428c04]", (nvn_sdk_tex_storage_class(c, &b, &v), v), 0x100);
        WANT("builder class, flag bit 4: 0x100 | device +0x1ac8 [0x427a3c]", nvn_sdk_texture_storage_class(c, &b), 0x100);
        SET(0, 10, 100, 1, 1, 1, 0x25, 0); v = 0;
        WANT("texture class, buffer target [0x42800c -> 0x428044]", (nvn_sdk_tex_storage_class(c, &b, &v), v), 0x100);
        SET(0, 1, 256, 256, 1, 1, 0x25, 0); sp[0] = sp[1] = sp[2] = sp[3] = 0xdeadbeef;
        nvn_sdk_texture_sparse_layout(c, &b, sp);
        WANT("sparse layout without the sparse flag: zero words 0,1 [0x4110c8 -> 0x411100]", (uint64_t)sp[0] | sp[1], 0);
        WANT("sparse layout without the sparse flag: zero words 2,3", (uint64_t)sp[2] | sp[3], 0);
        b.glname = 7; v = 0x55;
        WANT("texture made from an OpenGL name is not modelled", nvn_sdk_tex_storage_class(c, &b, &v), 0);
        const char *out2 = getenv("SSBU_NVN_CLASS_TABLE");
        FILE *g = out2 && *out2 ? fopen(out2, "w") : NULL;
        static const uint32_t fl2[] = {0, 8, 0x20, 0x28, 0x120, 0x10, 0x100};
        static const struct { uint32_t tg, w, h, d, lv, sm; } geo[] = {
            {1, 128, 64, 1, 1, 0}, {1, 1024, 1024, 1, 11, 0}, {1, 300, 200, 1, 4, 0}, {4, 512, 256, 6, 5, 0}, {2, 256, 256, 32, 9, 0}, {8, 256, 256, 1, 9, 0},
            {9, 128, 128, 12, 1, 0}, {0, 512, 1, 1, 1, 0}, {5, 256, 128, 1, 1, 4}, {7, 640, 360, 1, 1, 0}, {10, 100, 1, 1, 1, 0},
        };
        for (uint32_t fm = 1; fm < NVN_SDK_FORMAT_COUNT; fm++) {
            if (!nvn_sdk_format_known(fm)) continue;
            for (size_t gi = 0; gi < sizeof geo / sizeof geo[0]; gi++) for (size_t fi = 0; fi < sizeof fl2 / sizeof fl2[0]; fi++) {
                uint32_t rawb, rawt = 0, clb, clt = 0, sb[4] = {0, 0, 0, 0}, st[4] = {0, 0, 0, 0};
                if (geo[gi].tg == 10 && fl2[fi]) continue;
                SET(fl2[fi], geo[gi].tg, geo[gi].w, geo[gi].h, geo[gi].d, geo[gi].lv, fm, geo[gi].sm);
                if (fl2[fi] & 0x10) b.stride = geo[gi].w * 16;
                rawb = nvn_sdk_texture_raw_storage_class(c, &b);
                clb = nvn_sdk_texture_storage_class(c, &b);
                nvn_sdk_texture_sparse_layout(c, &b, sb);
                int ok = nvn_sdk_tex_raw_storage_class(c, &b, &rawt) && nvn_sdk_tex_storage_class(c, &b, &clt) && nvn_sdk_tex_sparse_layout(c, &b, st);
                uint32_t tsz = 0;                                    /* close5: nvnTextureGetStorageSize [sdk 0x429720] against the builder form [0x428760] */
                uint64_t bsz = nvn_sdk_texture_storage_size(c, &b);
                n++;
                if (!nvn_sdk_tex_storage_size(c, &b, &tsz) || (bsz && tsz != (uint32_t)bsz)) { bad++; printf("  FAIL nvn texture storage size: format 0x%x target %u flags 0x%x: texture 0x%x, builder 0x%llx\n", fm, geo[gi].tg, fl2[fi], tsz, (unsigned long long)bsz); }
                n++;
                if (!ok || rawb != rawt || (clb & ~0xffu) != 0x100 || (clt & ~0xffu) != 0x100) { bad++; printf("  FAIL nvn storage class: format 0x%x target %u flags 0x%x: raw %u / %u, class 0x%x / 0x%x\n", fm, geo[gi].tg, fl2[fi], rawb, rawt, clb, clt); }
                if (!(fl2[fi] & 0x20) && (sb[0] | sb[1] | sb[2] | sb[3] | st[0] | st[1] | st[2] | st[3])) { bad++; printf("  FAIL nvn sparse layout not zero without the sparse flag: format 0x%x target %u flags 0x%x\n", fm, geo[gi].tg, fl2[fi]); }
                if (g) fprintf(g, "%x\t%u\t%x\t%u\t%u\t%u\t%u\t%u\t%x\t%x\t%x\t%x\t%x %x %x %x\t%x %x %x %x\t%x\n", fm, geo[gi].tg, fl2[fi], geo[gi].sm, geo[gi].w, geo[gi].h, geo[gi].d, geo[gi].lv,
                               rawb, rawt, clb, clt, sb[0], sb[1], sb[2], sb[3], st[0], st[1], st[2], st[3], tsz);
            }
        }
        if (g) fclose(g);
    }
    printf("  nvn_sdk: %d texture storage cases, %d failed\n", n, bad);
    bad += nvn_c4_selftest();
    bad += nvn_c5_selftest(c);
    return bad;
}
