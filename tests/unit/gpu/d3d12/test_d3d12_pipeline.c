/* test_d3d12_pipeline.c — D3D12 pipelines, draws, dispatches and the pipeline library (DENG-324).
 * CTest names: gpu_d3d12.d3d12.<test>.
 *
 * Every host (pure, no device):
 *   - scissor_rect, pipeline_key: the scissor cut and the pipeline-library key.
 *   - pipeline_library_check: the library file's header, checksum and driver-version checks.
 *   - dxil_scan: the DXIL reader on a hand-built container (DXIL part with LLVM bitcode written
 *     here, with BLOCKINFO and local abbreviations, char6 arrays, skipped blocks; PSV0; OSG1),
 *     and on damaged ones.
 *   - dxil_scan_cooked: the reader on the shared S10 shaders cooked to DXIL, against the
 *     importer's reflection. Needs DXC on the host (Windows CI has it); SKIPs elsewhere.
 *
 * Windows with the D3D12 backend: the shared S10 pipeline tests (tests/unit/gpu/data/pipelines/,
 * DENG-323's names and expected values) on the best adapter, debug layer and GPU-based validation
 * on, zero debug-layer errors required:
 *   - pipeline_triangle_readback, pipeline_compute_bindless, draw_indirect_count, mesh_pipeline,
 *     push_constant_limit, pipeline_validation: as in test_vk_pipeline.c. One difference: buffers
 *     the shaders read are GPU_ONLY buffers filled by a copy, not UPLOAD buffers, because
 *     de_buffer<T> is a RWStructuredBuffer and an UPLOAD buffer only has an SRV on D3D12 (a
 *     known gap of bindless.slang, "gpu: D3D12").
 *   - root_signature: the serialized root signature read back; whether the device can make
 *     pipelines, and why not.
 *   - pipeline_library_roundtrip: the second device loads the stored PSOs (hit counter).
 *   - pipeline_library_mismatch: files from another driver version, damaged, truncated or not a
 *     library are discarded with a warning, and a good one is saved again.
 * Pipelines need shader model 6.6 and resource binding tier 3. CI's WARP (windows-2022) has shader
 * model 6.2: there the checks that come before the device's (blob checks, DE_ERR_UNSUPPORTED,
 * library file rejection) run and the rest SKIPs with the reason (needs-hw-verify).
 */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "de_test.h"

#include <dengine/de_gpu.h>
#include <dengine/de_platform.h>

#include "gpu/d3d12/d3d12_internal.h"
#include "pipeline_shaders.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Pure: scissor, keys ---------- */

TEST(d3d12, scissor_rect) {
    de__d3d12_rect r = de__d3d12_scissor_rect(4, 8, 16, 32);
    EXPECT_EQ(r.left, 4);
    EXPECT_EQ(r.top, 8);
    EXPECT_EQ(r.right, 20);
    EXPECT_EQ(r.bottom, 40);
    r = de__d3d12_scissor_rect(-10, -3, 16, 2);
    EXPECT_EQ(r.left, 0);
    EXPECT_EQ(r.top, 0);
    EXPECT_EQ(r.right, 6);
    EXPECT_EQ(r.bottom, 0);
    r = de__d3d12_scissor_rect(INT32_MAX - 1, 5, 100, 0xFFFFFFFFu);
    EXPECT_EQ(r.left, DE__D3D12_SCISSOR_MAX);
    EXPECT_EQ(r.right, DE__D3D12_SCISSOR_MAX);
    EXPECT_EQ(r.top, 5);
    EXPECT_EQ(r.bottom, DE__D3D12_SCISSOR_MAX);
    r = de__d3d12_scissor_rect(-100, -100, 50, 50); /* wholly outside: empty */
    EXPECT_EQ(r.right - r.left, 0);
    EXPECT_EQ(r.bottom - r.top, 0);
}

TEST(d3d12, pipeline_key) {
    static const u8 vs_a[16] = {1, 2, 3, 4}, vs_b[16] = {1, 2, 3, 5}, ps_code[8] = {9};
    de_gpu_pipeline_desc a = {.kind = DE_PIPE_GRAPHICS,
                              .vs = {vs_a, sizeof vs_a, "vs_main"},
                              .ps = {ps_code, sizeof ps_code, "ps_main"},
                              .color_formats = {DE_FMT_RGBA8_UNORM},
                              .color_count = 1,
                              .cull = DE_CULL_BACK,
                              .push_constant_bytes = 24,
                              .name = "a"};
    de_gpu_pipeline_desc b = a;
    EXPECT_EQ(de__d3d12_pipeline_key(&a), de__d3d12_pipeline_key(&b));
    /* Not in the key: the name, the entry name, threadgroups, colors past color_count. */
    b.name = "b";
    b.vs.entry = "other";
    b.color_formats[3] = DE_FMT_R8_UNORM;
    b.blend[5] = DE_BLEND_ADD;
    EXPECT_EQ(de__d3d12_pipeline_key(&a), de__d3d12_pipeline_key(&b));
    /* In the key: blob bytes, blend, cull, depth, formats, kind. */
    b = a;
    b.vs.code = vs_b;
    EXPECT_NE(de__d3d12_pipeline_key(&a), de__d3d12_pipeline_key(&b));
    b = a;
    b.blend[0] = DE_BLEND_ALPHA;
    EXPECT_NE(de__d3d12_pipeline_key(&a), de__d3d12_pipeline_key(&b));
    b = a;
    b.cull = DE_CULL_NONE;
    EXPECT_NE(de__d3d12_pipeline_key(&a), de__d3d12_pipeline_key(&b));
    b = a;
    b.depth_format = DE_FMT_D32_FLOAT;
    b.depth_test = true;
    EXPECT_NE(de__d3d12_pipeline_key(&a), de__d3d12_pipeline_key(&b));
    b = a;
    b.color_formats[0] = DE_FMT_BGRA8_UNORM;
    EXPECT_NE(de__d3d12_pipeline_key(&a), de__d3d12_pipeline_key(&b));
    b = a;
    b.ps = (de_gpu_shader_blob){0};
    EXPECT_NE(de__d3d12_pipeline_key(&a), de__d3d12_pipeline_key(&b));
    /* Compute: the raster state does not count. */
    de_gpu_pipeline_desc c = {.kind = DE_PIPE_COMPUTE, .cs = {vs_a, sizeof vs_a, "cs_main"}};
    de_gpu_pipeline_desc c2 = c;
    c2.cull = DE_CULL_FRONT;
    c2.color_count = 2;
    EXPECT_EQ(de__d3d12_pipeline_key(&c), de__d3d12_pipeline_key(&c2));
    EXPECT_NE(de__d3d12_pipeline_key(&c), de__d3d12_pipeline_key(&a));
}

/* ---------- Pure: the library file ---------- */

#define VENDOR 0x10DEu
#define DEVICE 0x2484u
#define DRIVER ((u64)31 << 48 | (u64)0 << 32 | (u64)15 << 16 | 3699u)

/* A library file of `keys` keys and `data` bytes, checksum right, for VENDOR/DEVICE/`driver`. */
static u64 make_library_file(u8 *out, u32 keys, u32 data, u64 driver) {
    de__d3d12_pipeline_library_file h;
    memset(&h, 0, sizeof h);
    h.magic = DE__D3D12_PIPELINE_LIBRARY_MAGIC;
    h.version = DE__D3D12_PIPELINE_LIBRARY_VERSION;
    h.vendor_id = VENDOR;
    h.device_id = DEVICE;
    h.driver_version = driver;
    h.key_count = keys;
    h.data_size = data;
    u8 *p = out + sizeof h;
    for (u32 i = 0; i < keys * 8u + data; ++i)
        p[i] = (u8)(i * 31u + 7u);
    h.data_hash = de_hash64(p, keys * 8u + data, DE__D3D12_PIPELINE_LIBRARY_MAGIC);
    memcpy(out, &h, sizeof h);
    return sizeof h + keys * 8u + data;
}

TEST(d3d12, pipeline_library_check) {
    static u8 file[48 + 3 * 8 + 100];
    const u64 size = make_library_file(file, 3, 100, DRIVER);
    ASSERT_EQ(size, (u64)sizeof file);
    char why[300];
    de__d3d12_pipeline_library_view v;
    ASSERT_TRUE(
        de__d3d12_pipeline_library_check(file, size, VENDOR, DEVICE, DRIVER, &v, why, sizeof why));
    EXPECT_TRUE(v.keys == file + 48);
    EXPECT_EQ(v.key_count, 3u);
    EXPECT_TRUE(v.data == file + 48 + 24);
    EXPECT_EQ(v.data_size, 100u);

#define REJECT(bytes, n, vendor, driver, needle)                                                   \
    do {                                                                                           \
        why[0] = '\0';                                                                             \
        EXPECT_FALSE(de__d3d12_pipeline_library_check((bytes), (n), (vendor), DEVICE, (driver),    \
                                                      &v, why, sizeof why));                       \
        EXPECT_TRUE(v.data == NULL);                                                               \
        if (!strstr(why, (needle)))                                                                \
            printf("reason \"%s\" lacks \"%s\"\n", why, (needle));                                 \
        EXPECT_TRUE(strstr(why, (needle)) != NULL);                                                \
    } while (0)

    REJECT(file, 20, VENDOR, DRIVER, "shorter");
    REJECT(NULL, 0, VENDOR, DRIVER, "shorter");
    REJECT(file, size - 1, VENDOR, DRIVER, "truncated");
    static u8 bad[sizeof file];
    memcpy(bad, file, sizeof file);
    bad[sizeof bad - 3] ^= 0x40;
    REJECT(bad, size, VENDOR, DRIVER, "checksum");
    memcpy(bad, file, sizeof file);
    bad[0] ^= 1;
    REJECT(bad, size, VENDOR, DRIVER, "not a DEngine");
    REJECT(file, size, 0x1002u, DRIVER, "not this adapter");
    /* Another driver: 31.0.15.3700. */
    REJECT(file, size, VENDOR, DRIVER + 1u, "made by driver 31.0.15.3699");
    REJECT(file, size, VENDOR, DRIVER + 1u, "another driver version");
    /* A key count past the end. */
    memcpy(bad, file, sizeof file);
    de__d3d12_pipeline_library_file h;
    memcpy(&h, bad, sizeof h);
    h.key_count = 1000;
    memcpy(bad, &h, sizeof h);
    REJECT(bad, size, VENDOR, DRIVER, "truncated");
#undef REJECT

    /* An empty library with no keys is fine. */
    static u8 empty[48];
    EXPECT_EQ(make_library_file(empty, 0, 0, DRIVER), (u64)48);
    EXPECT_TRUE(
        de__d3d12_pipeline_library_check(empty, 48, VENDOR, DEVICE, DRIVER, &v, why, sizeof why));

    char path[96];
    ASSERT_TRUE(de__d3d12_pipeline_library_path(path, sizeof path, "cache:/pipelines", 0x1414, 0x8c,
                                                DRIVER));
    EXPECT_STREQ(path, "cache:/pipelines/d3d12-1414-008c-31.0.15.3699.bin");
    EXPECT_FALSE(de__d3d12_pipeline_library_path(path, 16, "cache:/pipelines", 1, 2, 3));
    EXPECT_STREQ(path, "");
}

/* ---------- Pure: DXIL reader on a hand-built container ---------- */

/* A little LLVM bitstream writer: just enough for the reader's test. */
typedef struct bw {
    u8 buf[2048];
    u64 pos;
} bw;

static void bw_put(bw *w, u64 v, u32 n) {
    for (u32 i = 0; i < n; ++i, ++w->pos)
        if ((v >> i) & 1u)
            w->buf[w->pos >> 3] |= (u8)(1u << (w->pos & 7u));
}

static void bw_vbr(bw *w, u64 v, u32 n) {
    const u64 hi = (u64)1 << (n - 1u);
    while (v >= hi) {
        bw_put(w, (v & (hi - 1u)) | hi, n);
        v >>= n - 1u;
    }
    bw_put(w, v, n);
}

static void bw_align(bw *w) {
    w->pos = (w->pos + 31u) & ~(u64)31u;
}

/* ENTER_SUBBLOCK; returns where the length word is. */
static u64 bw_enter(bw *w, u32 width, u32 block, u32 new_width) {
    bw_put(w, 1, width);
    bw_vbr(w, block, 8);
    bw_vbr(w, new_width, 4);
    bw_align(w);
    const u64 at = w->pos;
    bw_put(w, 0, 32);
    return at;
}

static void bw_end(bw *w, u32 width, u64 len_at) {
    bw_put(w, 0, width);
    bw_align(w);
    const u64 words = (w->pos - len_at - 32u) / 32u;
    const u64 save = w->pos;
    w->pos = len_at;
    bw_put(w, words, 32);
    w->pos = save;
}

static void bw_rec(bw *w, u32 width, u32 code, const u64 *ops, u32 n) {
    bw_put(w, 3, width);
    bw_vbr(w, code, 6);
    bw_vbr(w, n, 6);
    for (u32 i = 0; i < n; ++i)
        bw_vbr(w, ops[i], 6);
}

static u32 char6(char c) {
    if (c >= 'a' && c <= 'z')
        return (u32)(c - 'a');
    if (c >= 'A' && c <= 'Z')
        return (u32)(c - 'A') + 26u;
    if (c >= '0' && c <= '9')
        return (u32)(c - '0') + 52u;
    return c == '.' ? 62u : 63u;
}

#define OPS(...) ((const u64[]){__VA_ARGS__}), (u32)(sizeof((const u64[]){__VA_ARGS__}) / 8u)

/* A module with two constant buffers in !dx.resources: b0 space0 (24 bytes) and b1 space7 (64). */
static u32 make_bitcode(u8 *out) {
    static bw w;
    memset(&w, 0, sizeof w);
    bw_put(&w, 'B', 8);
    bw_put(&w, 'C', 8);
    bw_put(&w, 0xC0, 8);
    bw_put(&w, 0xDE, 8);
    /* A top-level block the reader skips (IDENTIFICATION in newer LLVM). */
    u64 ident = bw_enter(&w, 2, 13, 5);
    bw_rec(&w, 5, 1, OPS('x', 'y'));
    bw_end(&w, 5, ident);

    u64 module = bw_enter(&w, 2, 8, 3);
    /* BLOCKINFO: an abbreviation for CONSTANTS: [literal 4 (INTEGER), vbr6]. */
    u64 info = bw_enter(&w, 3, 0, 2);
    bw_rec(&w, 2, 1, OPS(11)); /* SETBID 11 */
    bw_put(&w, 2, 2);          /* DEFINE_ABBREV */
    bw_vbr(&w, 2, 5);
    bw_put(&w, 1, 1);
    bw_vbr(&w, 4, 8); /* literal 4 */
    bw_put(&w, 0, 1);
    bw_put(&w, 2, 3);
    bw_vbr(&w, 6, 5); /* vbr6 */
    bw_end(&w, 2, info);
    /* Three module values: two globals, one function (value ids 0..2). */
    bw_rec(&w, 3, 7, OPS(1, 2, 3));
    bw_rec(&w, 3, 7, OPS(4, 5, 6));
    bw_rec(&w, 3, 8, OPS(7, 8));
    /* A function block (skipped) before the constants. */
    u64 fn = bw_enter(&w, 3, 12, 4);
    bw_rec(&w, 4, 1, OPS(1));
    u64 inner = bw_enter(&w, 4, 11, 4); /* function-level constants: not module values */
    bw_rec(&w, 4, 4, OPS(2 * 999));
    bw_end(&w, 4, inner);
    bw_end(&w, 4, fn);
    /* Module constants: ids 3.. */
    u64 consts = bw_enter(&w, 3, 11, 4);
    bw_rec(&w, 4, 1, OPS(0));  /* SETTYPE: no value */
    bw_rec(&w, 4, 2, NULL, 0); /* id 3 = 0: LLVM writes zero as NULL, not INTEGER */
    bw_put(&w, 4, 4);          /* id 4 = 24, with the BLOCKINFO abbreviation */
    bw_vbr(&w, 48, 6);
    bw_rec(&w, 4, 4, OPS(14));  /* id 5 = 7 (unabbreviated) */
    bw_rec(&w, 4, 3, OPS(0));   /* id 6 = undef */
    bw_rec(&w, 4, 4, OPS(2));   /* id 7 = 1 */
    bw_rec(&w, 4, 4, OPS(128)); /* id 8 = 64 */
    bw_rec(&w, 4, 4, OPS(11));  /* id 9 = -5 */
    bw_end(&w, 4, consts);
    /* Module metadata. Abbreviation 4: [literal 4 (NAME), array of char6]. */
    u64 md = bw_enter(&w, 3, 15, 3);
    bw_put(&w, 2, 3);
    bw_vbr(&w, 3, 5);
    bw_put(&w, 1, 1);
    bw_vbr(&w, 4, 8);
    bw_put(&w, 0, 1);
    bw_put(&w, 3, 3); /* array */
    bw_put(&w, 0, 1);
    bw_put(&w, 4, 3);                              /* char6 */
    bw_rec(&w, 3, 1, OPS('c', 'b'));               /* md0: "cb" */
    bw_rec(&w, 3, 2, OPS(0, 3));                   /* md1: value 3 (0) */
    bw_rec(&w, 3, 2, OPS(0, 4));                   /* md2: value 4 (24) */
    bw_rec(&w, 3, 2, OPS(0, 6));                   /* md3: value 6 (undef) */
    bw_rec(&w, 3, 3, OPS(2, 4, 1, 2, 2, 2, 3, 0)); /* md4: cb b0 space0, 24 bytes */
    bw_rec(&w, 3, 2, OPS(0, 5));                   /* md5: 7 */
    bw_rec(&w, 3, 2, OPS(0, 7));                   /* md6: 1 */
    bw_rec(&w, 3, 2, OPS(0, 8));                   /* md7: 64 */
    bw_rec(&w, 3, 3, OPS(7, 4, 1, 6, 7, 7, 8, 0)); /* md8: cb b1 space7, 64 bytes */
    bw_rec(&w, 3, 6, OPS(1, 'd', 'b', 'g'));       /* KIND: no id */
    bw_rec(&w, 3, 5, OPS(5, 9));                   /* md9: distinct node, the cbuffer list */
    bw_rec(&w, 3, 3, OPS(0, 0, 10, 0));            /* md10: {null, null, cbuffers, null} */
    /* !dx.version (not ours), then !dx.resources through the char6 abbreviation. */
    bw_rec(&w, 3, 4, OPS('d', 'x', '.', 'v', 'e', 'r', 's', 'i', 'o', 'n'));
    bw_rec(&w, 3, 10, OPS(9));
    static const char name[] = "dx.resources";
    bw_put(&w, 4, 3);
    bw_vbr(&w, sizeof name - 1u, 6);
    for (u32 i = 0; i + 1u < sizeof name; ++i)
        bw_put(&w, char6(name[i]), 6);
    bw_rec(&w, 3, 10, OPS(10));
    bw_end(&w, 3, md);
    bw_end(&w, 3, module);
    const u32 bytes = (u32)(w.pos / 8u);
    memcpy(out, w.buf, bytes);
    return bytes;
}

typedef struct part_in {
    u32 fourcc;
    const u8 *data;
    u32 size;
} part_in;

static void wr32(u8 *p, u32 v) {
    for (u32 i = 0; i < 4; ++i)
        p[i] = (u8)(v >> (8u * i));
}

#define FCC(a, b, c, d) ((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))

static u32 make_container(u8 *out, const part_in *parts, u32 n) {
    u32 at = 32u + 4u * n;
    memset(out, 0, at);
    wr32(out, FCC('D', 'X', 'B', 'C'));
    out[20] = 1; /* version 1.0 */
    wr32(out + 28, n);
    for (u32 i = 0; i < n; ++i) {
        wr32(out + 32 + 4 * i, at);
        wr32(out + at, parts[i].fourcc);
        wr32(out + at + 4, parts[i].size);
        memcpy(out + at + 8, parts[i].data, parts[i].size);
        at += 8u + parts[i].size;
    }
    wr32(out + 24, at);
    return at;
}

/* The DXIL part: program header (compute 6.6) + bitcode. */
static u32 make_dxil_part(u8 *out, const u8 *bitcode, u32 size, u32 kind) {
    wr32(out, (kind << 16) | (6u << 4) | 6u);
    wr32(out + 4, (24u + size) / 4u);
    wr32(out + 8, FCC('D', 'X', 'I', 'L'));
    wr32(out + 12, 0x106);
    wr32(out + 16, 16); /* bitcode right after the 16-byte bitcode header */
    wr32(out + 20, size);
    memcpy(out + 24, bitcode, size);
    return 24u + size;
}

TEST(d3d12, dxil_scan) {
    const de_allocator *a = de_heap_allocator();
    static u8 bitcode[2048], dxil[2100], psv[64], osg[160], container[4096];
    const u32 bc_size = make_bitcode(bitcode);
    ASSERT_EQ(bc_size % 4u, 0u);
    const u32 dxil_size = make_dxil_part(dxil, bitcode, bc_size, DE__DXIL_KIND_COMPUTE);
    /* PSV0: runtime info v3 (52 bytes), numthreads (8, 4, 2) at 36. */
    memset(psv, 0, sizeof psv);
    wr32(psv, 52);
    wr32(psv + 4 + 36, 8);
    wr32(psv + 4 + 40, 4);
    wr32(psv + 4 + 44, 2);
    /* OSG1: SV_Target0 (system value 64) and "SV_TARGET" 2 (by name), SV_Depth (ignored). */
    memset(osg, 0, sizeof osg);
    wr32(osg, 3);
    wr32(osg + 4, 8);
    const u32 names = 8 + 3 * 32;
    memcpy(osg + names, "SV_Target\0SV_TARGET\0SV_Depth", 29);
    wr32(osg + 8 + 4, names);
    wr32(osg + 8 + 8, 0);
    wr32(osg + 8 + 12, 64);
    wr32(osg + 40 + 4, names + 10);
    wr32(osg + 40 + 8, 2);
    wr32(osg + 40 + 12, 0);
    wr32(osg + 72 + 4, names + 20);
    wr32(osg + 72 + 8, 0);
    wr32(osg + 72 + 12, 65);
    const part_in parts[3] = {{FCC('P', 'S', 'V', '0'), psv, 56},
                              {FCC('O', 'S', 'G', '1'), osg, names + 29},
                              {FCC('D', 'X', 'I', 'L'), dxil, dxil_size}};
    const u32 size = make_container(container, parts, 3);

    de__d3d12_dxil_info info;
    char why[200] = "";
    de_result r = de__d3d12_dxil_scan(container, size, a, &info, why, sizeof why);
    if (r != DE_OK)
        printf("scan: %s\n", why);
    ASSERT_EQ(r, DE_OK);
    EXPECT_EQ(info.kind, DE__DXIL_KIND_COMPUTE);
    EXPECT_EQ(info.shader_model, 0x66u);
    EXPECT_TRUE(info.has_psv && info.has_outputs && info.has_bitcode);
    EXPECT_EQ(info.threadgroup[0], 8u);
    EXPECT_EQ(info.threadgroup[1], 4u);
    EXPECT_EQ(info.threadgroup[2], 2u);
    EXPECT_EQ(info.target_mask, 0x5u);
    ASSERT_EQ(info.cbuffer_count, 2u);
    EXPECT_EQ(info.cbuffers[0].space, 0u);
    EXPECT_EQ(info.cbuffers[0].lower_bound, 0u);
    EXPECT_EQ(info.cbuffers[0].size, 24u);
    EXPECT_EQ(info.cbuffers[1].space, 7u);
    EXPECT_EQ(info.cbuffers[1].lower_bound, 1u);
    EXPECT_EQ(info.cbuffers[1].size, 64u);
    EXPECT_EQ(info.push_constant_bytes, 24u);
    EXPECT_STREQ(de__d3d12_dxil_kind_name(info.kind), "compute");

    /* Without PSV0 and OSG1: those parts are unknown, the rest is read. */
    const u32 only = make_container(container, &parts[2], 1);
    r = de__d3d12_dxil_scan(container, only, a, &info, why, sizeof why);
    ASSERT_EQ(r, DE_OK);
    EXPECT_FALSE(info.has_psv);
    EXPECT_FALSE(info.has_outputs);
    EXPECT_EQ(info.threadgroup[0], 0u);
    EXPECT_EQ(info.push_constant_bytes, 24u);

    /* Damaged containers. */
    static u8 bad[4096];
#define SCAN_FAILS(bytes, n, needle)                                                               \
    do {                                                                                           \
        why[0] = '\0';                                                                             \
        EXPECT_EQ(de__d3d12_dxil_scan((bytes), (n), a, &info, why, sizeof why), DE_ERR_PARSE);     \
        if (!strstr(why, (needle)))                                                                \
            printf("reason \"%s\" lacks \"%s\"\n", why, (needle));                                 \
        EXPECT_TRUE(strstr(why, (needle)) != NULL);                                                \
    } while (0)
    SCAN_FAILS(container, 16, "not DXIL");
    memcpy(bad, container, size);
    bad[0] = 'X';
    SCAN_FAILS(bad, size, "not DXIL");
    SCAN_FAILS(container, only - 4, "not DXIL"); /* the container says more than there is */
    /* A part offset past the end. */
    memcpy(bad, container, only);
    wr32(bad + 32, only + 100);
    SCAN_FAILS(bad, only, "not DXIL");
    /* No DXIL part. */
    const part_in psv_only = {FCC('P', 'S', 'V', '0'), psv, 56};
    u32 n = make_container(bad, &psv_only, 1);
    SCAN_FAILS(bad, n, "no DXIL part");
    /* Not bitcode. */
    static u8 junk_dxil[64];
    static const u8 junk[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    part_in jp = {FCC('D', 'X', 'I', 'L'), junk_dxil, make_dxil_part(junk_dxil, junk, 16, 5)};
    n = make_container(bad, &jp, 1);
    SCAN_FAILS(bad, n, "no LLVM bitcode");
    /* Bitcode cut short: its blocks claim more than there is. */
    static u8 short_dxil[2100];
    jp.data = short_dxil;
    jp.size = make_dxil_part(short_dxil, bitcode, bc_size - 64u, 5);
    n = make_container(bad, &jp, 1);
    SCAN_FAILS(bad, n, "bad DXIL bitcode");
    /* The first top-level entry is not ENTER_SUBBLOCK: the reader stops, the module is not read. */
    static u8 bad_bc[2048];
    memcpy(bad_bc, bitcode, bc_size);
    bad_bc[4] = 0xFF;
    jp.data = short_dxil;
    jp.size = make_dxil_part(short_dxil, bad_bc, bc_size, 5);
    n = make_container(bad, &jp, 1);
    ASSERT_EQ(de__d3d12_dxil_scan(bad, n, a, &info, why, sizeof why), DE_OK);
    EXPECT_EQ(info.cbuffer_count, 0u); /* no module: no resources */
#undef SCAN_FAILS

    /* The kind names. */
    EXPECT_STREQ(de__d3d12_dxil_kind_name(DE__DXIL_KIND_PIXEL), "pixel");
    EXPECT_STREQ(de__d3d12_dxil_kind_name(DE__DXIL_KIND_VERTEX), "vertex");
    EXPECT_STREQ(de__d3d12_dxil_kind_name(DE__DXIL_KIND_MESH), "mesh");
    EXPECT_STREQ(de__d3d12_dxil_kind_name(DE__DXIL_KIND_AMPLIFICATION), "amplification");
    EXPECT_STREQ(de__d3d12_dxil_kind_name(99), "unknown kind");
}

/* ---------- The reader on cooked DXIL (DXC on the host) ---------- */

static de__shader_stage stage_of_kind(u32 kind) {
    switch (kind) {
    case DE__DXIL_KIND_VERTEX:
        return DE__SHADER_STAGE_VERTEX;
    case DE__DXIL_KIND_PIXEL:
        return DE__SHADER_STAGE_PIXEL;
    case DE__DXIL_KIND_COMPUTE:
        return DE__SHADER_STAGE_COMPUTE;
    case DE__DXIL_KIND_MESH:
        return DE__SHADER_STAGE_MESH;
    case DE__DXIL_KIND_AMPLIFICATION:
        return DE__SHADER_STAGE_AMPLIFICATION;
    default:
        return DE__SHADER_STAGE_NONE;
    }
}

static u32 rd32le(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* The entry-point name PSV0 v3 records (EntryFunctionName, an offset into its string table), or
 * "" when the container has none. D3D12 itself ignores it for non-library shaders. */
static void psv_entry_name(const u8 *c, u64 size, char *out, size_t cap) {
    out[0] = '\0';
    if (size < 32 || rd32le(c) != FCC('D', 'X', 'B', 'C'))
        return;
    const u32 parts = rd32le(c + 28);
    for (u32 i = 0; i < parts && 32u + 4u * i + 4u <= size; ++i) {
        const u32 off = rd32le(c + 32 + 4 * i);
        if ((u64)off + 8u > size || rd32le(c + off) != FCC('P', 'S', 'V', '0'))
            continue;
        const u8 *p = c + off + 8;
        const u64 n = rd32le(c + off + 4);
        if ((u64)off + 8u + n > size || n < 4)
            return;
        const u32 info = rd32le(p);
        if (info < 52 || 4u + (u64)info + 4u > n)
            return;
        const u32 name = rd32le(p + 4 + 48);
        u64 at = 4u + info;
        const u32 resources = rd32le(p + at);
        at += 4;
        if (resources) {
            if (at + 4u > n)
                return;
            const u32 stride = rd32le(p + at);
            at += 4u + (u64)resources * stride;
        }
        if (at + 4u > n)
            return;
        const u32 table = rd32le(p + at);
        at += 4;
        if (at + table > n || name >= table)
            return;
        snprintf(out, cap, "%.*s", (int)(table - name), (const char *)p + at + name);
        return;
    }
}

TEST(d3d12, dxil_scan_cooked) {
    char why[300];
    static char skip[400];
    if (!de_pipe_test_cook(DE__SHADER_TARGET_DXIL, why, sizeof why)) {
        snprintf(skip, sizeof skip, "shared pipeline shaders not cooked to DXIL: %s", why);
        SKIP(skip);
    }
    static const struct {
        const char *file, *entry;
        u32 push; /* expected push-constant bytes */
        bool pixel;
    } k[] = {
        {"triangle.slang", "vs_main", 24, false},    {"triangle.slang", "ps_main", 24, true},
        {"compute.slang", "cs_main", 16, false},     {"indirect_args.slang", "cs_main", 16, false},
        {"push_limit.slang", "cs_main", 128, false}, {"mesh.slang", "as_main", 24, false},
        {"mesh.slang", "ms_main", 24, false},        {"mesh.slang", "ps_main", 24, true},
    };
    for (u32 i = 0; i < sizeof k / sizeof k[0]; ++i) {
        de_pipe_test_shader s;
        if (!de_pipe_test_get(DE__SHADER_TARGET_DXIL, k[i].file, k[i].entry, &s)) {
            printf("%s:%s not cooked for DXIL\n", k[i].file, k[i].entry);
            EXPECT_TRUE(strcmp(k[i].file, "mesh.slang") == 0); /* only mesh may be missing */
            continue;
        }
        de__d3d12_dxil_info info;
        why[0] = '\0';
        const de_result r = de__d3d12_dxil_scan(s.blob.code, s.blob.size, de_heap_allocator(),
                                                &info, why, sizeof why);
        printf("%s:%s: %s", k[i].file, k[i].entry, r == DE_OK ? "" : why);
        if (r == DE_OK) {
            printf("%s SM %u.%u, threads (%u, %u, %u)%s, targets 0x%x, %u cbuffers:",
                   de__d3d12_dxil_kind_name(info.kind), info.shader_model >> 4,
                   info.shader_model & 0xFu, info.threadgroup[0], info.threadgroup[1],
                   info.threadgroup[2], info.has_psv ? "" : " (no PSV)", info.target_mask,
                   info.cbuffer_count);
            for (u32 c = 0; c < info.cbuffer_count && c < DE__DXIL_MAX_CBUFFERS; ++c)
                printf(" b%u space%u %u bytes;", info.cbuffers[c].lower_bound,
                       info.cbuffers[c].space, info.cbuffers[c].size);
        }
        char entry[64];
        psv_entry_name((const u8 *)s.blob.code, s.blob.size, entry, sizeof entry);
        printf(" [bitcode: %u globals, %u int constants, %u metadata, codes 0x%llx, names %s, "
               "resources md %d, trace %s]",
               info.bc.globals, info.bc.constants, info.bc.metadata,
               (unsigned long long)info.bc.md_codes, info.bc.names, (int)info.bc.resources,
               info.bc.trace);
        printf(" (reflection: %u push bytes, threads %u %u %u); PSV0 entry name \"%s\", blob.entry "
               "\"%s\"\n",
               s.push_constant_bytes, s.threadgroup[0], s.threadgroup[1], s.threadgroup[2], entry,
               s.blob.entry);
        ASSERT_EQ(r, DE_OK);
        EXPECT_EQ((u32)stage_of_kind(info.kind), s.stage);
        EXPECT_EQ(info.shader_model, 0x66u);
        EXPECT_TRUE(info.has_bitcode);
        EXPECT_EQ(info.push_constant_bytes, s.push_constant_bytes);
        EXPECT_EQ(info.push_constant_bytes, k[i].push);
        if (s.threadgroup[0]) {
            EXPECT_TRUE(info.has_psv);
            EXPECT_EQ(info.threadgroup[0], s.threadgroup[0]);
            EXPECT_EQ(info.threadgroup[1], s.threadgroup[1]);
            EXPECT_EQ(info.threadgroup[2], s.threadgroup[2]);
        }
        if (k[i].pixel) {
            EXPECT_TRUE(info.has_outputs);
            EXPECT_EQ(info.target_mask, 1u); /* SV_Target0 */
        }
    }
}

/* ---------- Device tests (Windows) ---------- */
#if defined(_WIN32) && DE_HAS_GPU_D3D12

#include "gpu/gpu_backend.h"

#define WAIT_NS ((u64)30 * 1000 * 1000 * 1000)
#define SIZE    DE_PIPE_TEST_SIZE
#define PITCH   (DE_PIPE_TEST_SIZE * 4u)

static const char *errchain(char *buf, size_t cap) {
    size_t used = 0;
    buf[0] = '\0';
    for (const de_error *e = de_error_last(); e && used + 1 < cap; e = e->cause) {
        int w = snprintf(buf + used, cap - used, "%s%s", used ? " <- " : "", e->message);
        if (w < 0)
            break;
        used += (size_t)w < cap - used ? (size_t)w : cap - used - 1;
    }
    return buf;
}

typedef struct pipe_fx {
    de_tracking_allocator tracker;
    de_allocator alloc;
    de_gpu_device *d;
    char skip[1200];
    char err[1100];
} pipe_fx;

static void need_shaders(pipe_fx *f) {
    char why[300];
    if (!de_pipe_test_cook(DE__SHADER_TARGET_DXIL, why, sizeof why)) {
        snprintf(f->skip, sizeof f->skip, "shared pipeline shaders not cooked to DXIL: %s", why);
        SKIP(f->skip);
    }
}

static de_pipe_test_shader shader(const char *file, const char *entry) {
    de_pipe_test_shader s;
    if (!de_pipe_test_get(DE__SHADER_TARGET_DXIL, file, entry, &s)) {
        printf("missing cooked shader %s:%s\n", file, entry);
        ASSERT_TRUE(false);
    }
    return s;
}

/* The best D3D12 adapter with the debug layer, and GPU-based validation except for the pipeline
 * library tests (cache_dir set): the runtime may refuse pipeline libraries under it. */
static de_gpu_device *make_device(pipe_fx *f, const char *cache_dir) {
    de_gpu_device_desc desc = {.backend = DE_GPU_D3D12,
                               .debug_layer = true,
                               .gpu_validation = cache_dir == NULL || cache_dir[0] == '\0',
                               .allocator = &f->alloc,
                               .pipeline_cache_dir = cache_dir};
    de_gpu_device *d = NULL;
    de_result r = de_gpu_device_create(&desc, &d);
    if (r == DE_ERR_UNSUPPORTED) {
        snprintf(f->skip, sizeof f->skip, "no D3D12 device: %s", errchain(f->err, sizeof f->err));
        SKIP(f->skip);
    }
    ASSERT_EQ(r, DE_OK);
    ASSERT_TRUE(de__d3d12_debug_layer_active(d));
    return d;
}

static void pipe_fx_setup(pipe_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
}

static void end_device(de_gpu_device *d) {
    if (!d)
        return;
    de_gpu_wait_idle(d);
    EXPECT_EQ(de__d3d12_debug_error_count(d), 0u);
    de_gpu_device_destroy(d);
}

static void pipe_fx_teardown(pipe_fx *f) {
    end_device(f->d);
    f->d = NULL;
    de_tracking_stats st = de_tracking_allocator_stats(&f->tracker);
    EXPECT_EQ(st.blocks_live, 0u);
    EXPECT_EQ(st.bytes_live, 0u);
}

/* The fixture's device, without a pipeline library. */
static void open_device(pipe_fx *f) {
    need_shaders(f);
    f->d = make_device(f, "");
    const char *why = NULL;
    const bool ok = de__d3d12_pipelines_supported(f->d, &why);
    printf("device \"%s\": pipelines %s%s; mesh shaders %s\n", de_gpu_get_caps(f->d)->device_name,
           ok ? "yes" : "no: ", ok ? "" : why, de_gpu_get_caps(f->d)->mesh_shaders ? "yes" : "no");
}

/* SKIP when the device cannot make pipelines (CI's WARP: shader model 6.2). */
static void need_pipelines(pipe_fx *f, de_gpu_device *d, const char *what) {
    const char *why = NULL;
    if (!de__d3d12_pipelines_supported(d, &why)) {
        snprintf(f->skip, sizeof f->skip,
                 "%s needs pipelines, and this device has none (%s); "
                 "run it on hardware or a newer WARP (needs-hw-verify)",
                 what, why);
        SKIP(f->skip);
    }
}

static de_gpu_barrier tex_barrier(de_gpu_texture t, u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.texture = t;
    b.before = before;
    b.after = after;
    return b;
}

static de_gpu_barrier buf_barrier(de_gpu_buffer buf, u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.buffer = buf;
    b.before = before;
    b.after = after;
    return b;
}

static void submit_wait(de_gpu_device *d, de_gpu_queue q, de_gpu_cmd *c) {
    de_gpu_fence fence = de_gpu_fence_create(d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(fence));
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(d, q, &s), DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(d, fence, 1, WAIT_NS), DE_OK);
    de_gpu_fence_destroy(d, fence);
}

static de_gpu_buffer upload(de_gpu_device *d, const void *data, u64 size, u32 usage, u32 stride,
                            const char *name) {
    de_gpu_buffer b = de_gpu_buffer_create(
        d,
        &(de_gpu_buffer_desc){
            .size = size, .usage = usage, .memory = DE_MEM_UPLOAD, .stride = stride, .name = name});
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    void *p = de_gpu_buffer_map(d, b);
    ASSERT_TRUE(p != NULL);
    memcpy(p, data, (size_t)size);
    return b;
}

/* A GPU_ONLY storage buffer holding `data`: filled by a copy from an UPLOAD buffer on queue `q`,
 * then left in SHADER_WRITE (a UAV: shaders read de_buffer<T> as RWStructuredBuffer). */
static de_gpu_buffer storage(de_gpu_device *d, de_gpu_queue q, const void *data, u64 size,
                             u32 stride, const char *name) {
    de_gpu_buffer staging = upload(d, data, size, DE_USAGE_COPY_SRC, 0, "staging");
    de_gpu_buffer b =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = size,
                                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST,
                                                      .stride = stride,
                                                      .name = name});
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    de_gpu_cmd *c = de_gpu_cmd_begin(d, q, "fill storage");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier bb = buf_barrier(b, DE_ACCESS_NONE, DE_ACCESS_COPY_DST);
    de_gpu_cmd_barriers(c, &bb, 1);
    de_gpu_cmd_copy_buffer(c, b, 0, staging, 0, size);
    bb = buf_barrier(b, DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_WRITE);
    de_gpu_cmd_barriers(c, &bb, 1);
    submit_wait(d, q, c);
    de_gpu_buffer_destroy(d, staging);
    return b;
}

typedef struct target {
    de_gpu_texture color, depth;
    de_gpu_buffer readback;
    u8 *pixels;
} target;

static target make_target(de_gpu_device *d, bool depth) {
    target t;
    memset(&t, 0, sizeof t);
    t.color = de_gpu_texture_create(
        d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                  .width = SIZE,
                                  .height = SIZE,
                                  .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_SRC,
                                  .name = "pipeline target"});
    ASSERT_TRUE(DE_HANDLE_VALID(t.color));
    if (depth) {
        t.depth = de_gpu_texture_create(d, &(de_gpu_texture_desc){.format = DE_FMT_D32_FLOAT,
                                                                  .width = SIZE,
                                                                  .height = SIZE,
                                                                  .usage = DE_USAGE_DEPTH,
                                                                  .name = "pipeline depth"});
        ASSERT_TRUE(DE_HANDLE_VALID(t.depth));
    }
    t.readback = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = (u64)PITCH * SIZE,
                                                               .usage = DE_USAGE_COPY_DST,
                                                               .memory = DE_MEM_READBACK,
                                                               .name = "pipeline readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(t.readback));
    t.pixels = (u8 *)de_gpu_buffer_map(d, t.readback);
    ASSERT_TRUE(t.pixels != NULL);
    memset(t.pixels, 0xEE, (size_t)PITCH * SIZE);
    return t;
}

static void free_target(de_gpu_device *d, target *t) {
    de_gpu_texture_destroy(d, t->color);
    de_gpu_texture_destroy(d, t->depth);
    de_gpu_buffer_destroy(d, t->readback);
}

static void begin_target(de_gpu_cmd *c, const target *t) {
    de_gpu_barrier b[2] = {tex_barrier(t->color, DE_ACCESS_NONE, DE_ACCESS_RENDER_TARGET),
                           tex_barrier(t->depth, DE_ACCESS_NONE, DE_ACCESS_DEPTH_WRITE)};
    de_gpu_cmd_barriers(c, b, DE_HANDLE_VALID(t->depth) ? 2u : 1u);
    de_gpu_attachment color = {
        .texture = t->color, .clear = true, .clear_color = {DE_PIPE_TEST_CLEAR_F}, .store = true};
    de_gpu_attachment depth = {.texture = t->depth, .clear = true, .clear_depth = 0.0f};
    de_gpu_cmd_begin_render(c, &color, 1, DE_HANDLE_VALID(t->depth) ? &depth : NULL);
}

static void end_target(de_gpu_cmd *c, const target *t) {
    de_gpu_cmd_end_render(c);
    de_gpu_barrier b = tex_barrier(t->color, DE_ACCESS_RENDER_TARGET, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_texture_to_buffer(c, t->readback, 0, PITCH, t->color, 0, 0);
}

static void expect_pixels(const target *t, const de_pipe_test_pixel *px, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        const u8 *p = t->pixels + (size_t)px[i].y * PITCH + (size_t)px[i].x * 4u;
        if (memcmp(p, px[i].rgba, 4) != 0)
            printf("pixel (%u, %u): got %u %u %u %u, expected %u %u %u %u\n", px[i].x, px[i].y,
                   p[0], p[1], p[2], p[3], px[i].rgba[0], px[i].rgba[1], px[i].rgba[2],
                   px[i].rgba[3]);
        EXPECT_MEMEQ(p, px[i].rgba, 4);
    }
}

typedef struct draw_data {
    f32 color[4];
    u32 positions;
    u32 first;
} draw_data;

static u32 max_u32(u32 a, u32 b) {
    return a > b ? a : b;
}

static de_gpu_pipeline triangle_pipeline(de_gpu_device *d, bool depth) {
    de_pipe_test_shader vs = shader("triangle.slang", "vs_main");
    de_pipe_test_shader ps = shader("triangle.slang", "ps_main");
    de_gpu_pipeline_desc pd = {.kind = DE_PIPE_GRAPHICS,
                               .vs = vs.blob,
                               .ps = ps.blob,
                               .color_formats = {DE_FMT_RGBA8_UNORM},
                               .color_count = 1,
                               .depth_format = depth ? DE_FMT_D32_FLOAT : DE_FMT_UNKNOWN,
                               .cull = DE_CULL_BACK,
                               .depth_test = depth,
                               .depth_write = depth,
                               .push_constant_bytes =
                                   max_u32(vs.push_constant_bytes, ps.push_constant_bytes),
                               .name = "triangle"};
    de_gpu_pipeline p = de_gpu_pipeline_create(d, &pd);
    if (!DE_HANDLE_VALID(p)) {
        char e[600];
        printf("triangle pipeline: %s\n", errchain(e, sizeof e));
    }
    ASSERT_TRUE(DE_HANDLE_VALID(p));
    return p;
}

static de_gpu_pipeline compute_pipeline(de_gpu_device *d, const char *file, const char *name) {
    de_pipe_test_shader cs = shader(file, "cs_main");
    de_gpu_pipeline_desc pd = {
        .kind = DE_PIPE_COMPUTE,
        .cs = cs.blob,
        .push_constant_bytes = cs.push_constant_bytes,
        .threadgroup_size = {cs.threadgroup[0], cs.threadgroup[1], cs.threadgroup[2]},
        .name = name};
    de_gpu_pipeline p = de_gpu_pipeline_create(d, &pd);
    if (!DE_HANDLE_VALID(p)) {
        char e[600];
        printf("%s pipeline: %s\n", name, errchain(e, sizeof e));
    }
    ASSERT_TRUE(DE_HANDLE_VALID(p));
    return p;
}

/* DENG-618: no clear/copy initializes the RT. Compute creates every background texel;
 * a real draw LOADs the image and replaces only one quadrant. A second compute use writes only
 * the left half; the initialized right half must survive all transitions and the next draw. */
static void first_uav_rt_readback(pipe_fx *f, de_gpu_queue producer) {
    open_device(f);
    need_pipelines(f, f->d, "compute-first RT readback");
    de_gpu_device *d = f->d;
    const bool cross_queue = producer == DE_QUEUE_COMPUTE;
    const bool enhanced = de__d3d12_enhanced_barriers_active(d);
    printf("DENG-618 producer=%s barriers=%s\n", cross_queue ? "compute" : "graphics",
           enhanced ? "enhanced" : "legacy");
    de_gpu_pipeline compute = compute_pipeline(d, "uav_rt_init.slang", "first UAV RT fill");
    const de_pipe_test_shader vs = shader("uav_rt_init.slang", "vs_main"),
                              ps = shader("uav_rt_init.slang", "ps_main");
    de_gpu_pipeline draw = de_gpu_pipeline_create(
        d, &(de_gpu_pipeline_desc){.kind = DE_PIPE_GRAPHICS,
                                   .vs = vs.blob,
                                   .ps = ps.blob,
                                   .color_formats = {DE_FMT_RGBA8_UNORM},
                                   .color_count = 1,
                                   .cull = DE_CULL_NONE,
                                   .push_constant_bytes =
                                       max_u32(vs.push_constant_bytes, ps.push_constant_bytes),
                                   .name = "first UAV RT overlay"});
    ASSERT_TRUE(DE_HANDLE_VALID(draw));
    target t = {0};
    t.color = de_gpu_texture_create(
        d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                  .width = SIZE,
                                  .height = SIZE,
                                  .usage =
                                      DE_USAGE_RENDER_TARGET | DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                  .name = "compute first RT (no clear)"});
    ASSERT_TRUE(DE_HANDLE_VALID(t.color));
    t.readback = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = (u64)PITCH * SIZE,
                                                               .usage = DE_USAGE_COPY_DST,
                                                               .memory = DE_MEM_READBACK,
                                                               .name = "first UAV RT readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(t.readback));
    t.pixels = de_gpu_buffer_map(d, t.readback);
    ASSERT_TRUE(t.pixels != NULL);
    memset(t.pixels, 0xEE, (size_t)PITCH * SIZE);
    de_gpu_barrier to_graphics =
        tex_barrier(t.color, DE_ACCESS_SHADER_WRITE, DE_ACCESS_RENDER_TARGET);
    to_graphics.src_queue = producer;
    to_graphics.dst_queue = DE_QUEUE_GRAPHICS;
    de_gpu_barrier to_compute = tex_barrier(t.color, DE_ACCESS_COPY_SRC, DE_ACCESS_SHADER_WRITE);
    to_compute.src_queue = DE_QUEUE_GRAPHICS;
    to_compute.dst_queue = producer;
    for (u32 phase = 0; phase < 2; ++phase) {
        de_error_clear();
        de_gpu_cmd *c = de_gpu_cmd_begin(d, producer, "compute first RT image");
        ASSERT_TRUE(c != NULL);
        de_gpu_barrier first = tex_barrier(t.color, phase ? DE_ACCESS_COPY_SRC : DE_ACCESS_NONE,
                                           DE_ACCESS_SHADER_WRITE);
        if (phase && cross_queue)
            first = to_compute; /* Acquire initialized contents: must not discard. */
        de_gpu_cmd_barriers(c, &first, 1);
        de_gpu_cmd_set_pipeline(c, compute);
        const u32 params[4] = {de_gpu_texture_bindless(d, t.color, 0, true), SIZE, SIZE, phase};
        de_gpu_cmd_push_constants(c, params, sizeof params);
        de_gpu_cmd_dispatch(c, SIZE / 8, SIZE / 8, 1);
        de_gpu_cmd_barriers(c, &to_graphics, 1);
        if (cross_queue) {
            submit_wait(d, producer, c); /* CPU fence wait orders the two real queues. */
            c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "LOAD compute image and draw UI");
            ASSERT_TRUE(c != NULL);
            de_gpu_cmd_barriers(c, &to_graphics, 1); /* Acquire producer's image. */
        }
        const de_gpu_attachment color = {.texture = t.color, .clear = false, .store = true};
        de_gpu_cmd_begin_render(c, &color, 1, NULL);
        de_gpu_cmd_viewport(c, 0, 0, (f32)SIZE, (f32)SIZE);
        de_gpu_cmd_scissor(c, 0, 0, SIZE / 2, SIZE / 2);
        de_gpu_cmd_set_pipeline(c, draw);
        de_gpu_cmd_draw(c, 3, 1, 0, 0);
        end_target(c, &t);
        if (cross_queue && phase == 0)
            de_gpu_cmd_barriers(c, &to_compute, 1); /* Release for initialized second use. */
        EXPECT_TRUE(de_error_last() == NULL);
        submit_wait(d, DE_QUEUE_GRAPHICS, c);
        u32 bad = 0;
        for (u32 y = 0; y < SIZE; ++y)
            for (u32 x = 0; x < SIZE; ++x) {
                const bool overlay = x < SIZE / 2 && y < SIZE / 2;
                const u8 expected[4] = {
                    overlay ? 17 : (u8)((x * 3u + ((phase && x < SIZE / 2) ? 41u : 0u)) & 255u),
                    overlay ? 211 : (u8)((y * 5u + 17u) & 255u), overlay ? 59 : 91, 255};
                const u8 *got = t.pixels + (size_t)y * PITCH + x * 4;
                if (memcmp(got, expected, 4)) {
                    if (!bad)
                        printf("phase%u (%u,%u): %u,%u,%u,%u expected %u,%u,%u,%u\n", phase, x, y,
                               got[0], got[1], got[2], got[3], expected[0], expected[1],
                               expected[2], expected[3]);
                    ++bad;
                }
            }
        printf("DENG-618 phase%u: %u texels, %u mismatches\n", phase, SIZE * SIZE, bad);
        EXPECT_EQ(bad, 0);
        char dir[768];
        const DWORD dir_length =
            GetEnvironmentVariableA("DE_D3D12_INIT_EVIDENCE_DIR", dir, (DWORD)sizeof dir);
        if (dir_length) {
            ASSERT_LT(dir_length, (DWORD)sizeof dir);
            char path[1024];
            snprintf(path, sizeof path, "%s/%s-%s-phase%u.rgba", dir,
                     cross_queue ? "compute" : "graphics", enhanced ? "enhanced" : "legacy", phase);
            FILE *file = NULL;
            ASSERT_EQ(fopen_s(&file, path, "wb"), 0);
            ASSERT_TRUE(file != NULL);
            EXPECT_EQ(fwrite(t.pixels, 1, (size_t)PITCH * SIZE, file), (size_t)PITCH * SIZE);
            EXPECT_EQ(fclose(file), 0);
        }
    }
    free_target(d, &t);
    de_gpu_pipeline_destroy(d, draw);
    de_gpu_pipeline_destroy(d, compute);
}
TEST_F(d3d12, compute_first_rt_graphics, pipe_fx) {
    first_uav_rt_readback(f, DE_QUEUE_GRAPHICS);
}
TEST_F(d3d12, compute_first_rt_compute, pipe_fx) {
    first_uav_rt_readback(f, DE_QUEUE_COMPUTE);
}

/* ---------- Triangle ---------- */

TEST_F(d3d12, pipeline_triangle_readback, pipe_fx) {
    open_device(f);
    need_pipelines(f, f->d, "the triangle test");
    de_gpu_pipeline pipe = triangle_pipeline(f->d, true);
    de_gpu_buffer pos = storage(f->d, DE_QUEUE_GRAPHICS, de_pipe_test_triangle_pos,
                                sizeof de_pipe_test_triangle_pos, 16, "triangle positions");
    target t = make_target(f->d, true);
    /* Twice: the second pass reuses the recycled list, so the root signature is set again. */
    for (u32 pass = 0; pass < 2; ++pass) {
        de_error_clear();
        de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "triangle");
        ASSERT_TRUE(c != NULL);
        begin_target(c, &t);
        de_gpu_cmd_set_pipeline(c, pipe);
        for (u32 k = 0; k < 3; ++k) {
            draw_data dd;
            memcpy(dd.color, de_pipe_test_triangle_color[k], sizeof dd.color);
            dd.positions = de_gpu_buffer_bindless(f->d, pos);
            dd.first = 3u * k;
            de_gpu_cmd_push_constants(c, &dd, sizeof dd);
            de_gpu_cmd_draw(c, 3, 1, 0, 0);
        }
        end_target(c, &t);
        EXPECT_TRUE(de_error_last() == NULL);
        submit_wait(f->d, DE_QUEUE_GRAPHICS, c);
        expect_pixels(&t, de_pipe_test_triangle_expect, 3);
        memset(t.pixels, 0xEE, (size_t)PITCH * SIZE);
    }
    free_target(f->d, &t);
    de_gpu_buffer_destroy(f->d, pos);
    de_gpu_pipeline_destroy(f->d, pipe);
}

/* ---------- Compute ---------- */

TEST_F(d3d12, pipeline_compute_bindless, pipe_fx) {
    open_device(f);
    need_pipelines(f, f->d, "the compute test");
    enum { N = DE_PIPE_TEST_COMPUTE_COUNT };
    static u32 src[N];
    for (u32 i = 0; i < N; ++i)
        src[i] = DE_PIPE_TEST_COMPUTE_SRC(i);
    de_gpu_pipeline pipe = compute_pipeline(f->d, "compute.slang", "compute");
    de_gpu_buffer in = storage(f->d, DE_QUEUE_COMPUTE, src, sizeof src, 4, "compute src");
    de_gpu_buffer out = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = sizeof src,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                    .stride = 4,
                                    .name = "compute dst"});
    de_gpu_buffer rb = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = sizeof src,
                                                                        .usage = DE_USAGE_COPY_DST,
                                                                        .memory = DE_MEM_READBACK,
                                                                        .name = "compute rb"});
    ASSERT_TRUE(DE_HANDLE_VALID(out) && DE_HANDLE_VALID(rb));
    u32 *res = (u32 *)de_gpu_buffer_map(f->d, rb);
    memset(res, 0, sizeof src);

    de_error_clear();
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "compute");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier b = buf_barrier(out, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_set_pipeline(c, pipe);
    const u32 params[4] = {de_gpu_buffer_bindless(f->d, in), de_gpu_buffer_bindless(f->d, out), N,
                           DE_PIPE_TEST_COMPUTE_SEED};
    de_gpu_cmd_push_constants(c, params, sizeof params);
    de_gpu_cmd_dispatch(c, (N + 63u) / 64u, 1, 1);
    b = buf_barrier(out, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_buffer(c, rb, 0, out, 0, sizeof src);
    EXPECT_TRUE(de_error_last() == NULL);
    submit_wait(f->d, DE_QUEUE_COMPUTE, c);

    u32 bad = 0;
    for (u32 i = 0; i < N; ++i)
        if (res[i] != DE_PIPE_TEST_COMPUTE_DST(i)) {
            if (bad++ == 0)
                printf("element %u: 0x%08x, expected 0x%08x\n", i, res[i],
                       DE_PIPE_TEST_COMPUTE_DST(i));
        }
    EXPECT_EQ(bad, 0u);
    de_gpu_buffer_destroy(f->d, in);
    de_gpu_buffer_destroy(f->d, out);
    de_gpu_buffer_destroy(f->d, rb);
    de_gpu_pipeline_destroy(f->d, pipe);
}

/* ---------- Indirect count ---------- */

TEST_F(d3d12, draw_indirect_count, pipe_fx) {
    open_device(f);
    need_pipelines(f, f->d, "the indirect-count test");
    de_gpu_pipeline args_pipe = compute_pipeline(f->d, "indirect_args.slang", "indirect args");
    de_gpu_pipeline draw_pipe = triangle_pipeline(f->d, false);
    de_gpu_buffer pos = storage(f->d, DE_QUEUE_GRAPHICS, de_pipe_test_indirect_pos,
                                sizeof de_pipe_test_indirect_pos, 16, "indirect positions");
    u16 idx[10] = {0};
    memcpy(idx, de_pipe_test_indirect_indices, sizeof de_pipe_test_indirect_indices);
    de_gpu_buffer ib = upload(f->d, idx, sizeof idx, DE_USAGE_INDEX, 0, "indirect indices");
    de_gpu_buffer args = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = DE_PIPE_TEST_INDIRECT_RECORDS * 20u,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_INDIRECT,
                                    .stride = 4,
                                    .name = "indirect args"});
    de_gpu_buffer count = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = 16,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_INDIRECT,
                                    .stride = 4,
                                    .name = "indirect count"});
    ASSERT_TRUE(DE_HANDLE_VALID(args) && DE_HANDLE_VALID(count));
    target t = make_target(f->d, false);

    /* Pass 0: with the count buffer (2 of 3 records). Pass 1: without it (all 3). */
    for (u32 pass = 0; pass < 2; ++pass) {
        de_error_clear();
        de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "indirect count");
        ASSERT_TRUE(c != NULL);
        de_gpu_barrier b[2] = {buf_barrier(args, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE),
                               buf_barrier(count, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE)};
        de_gpu_cmd_barriers(c, b, 2);
        de_gpu_cmd_set_pipeline(c, args_pipe);
        const u32 params[4] = {de_gpu_buffer_bindless(f->d, args),
                               de_gpu_buffer_bindless(f->d, count), DE_PIPE_TEST_INDIRECT_RECORDS,
                               DE_PIPE_TEST_INDIRECT_COUNT};
        de_gpu_cmd_push_constants(c, params, sizeof params);
        de_gpu_cmd_dispatch(c, 1, 1, 1);
        b[0] = buf_barrier(args, DE_ACCESS_SHADER_WRITE, DE_ACCESS_INDIRECT);
        b[1] = buf_barrier(count, DE_ACCESS_SHADER_WRITE, DE_ACCESS_INDIRECT);
        de_gpu_cmd_barriers(c, b, 2);
        begin_target(c, &t);
        de_gpu_cmd_set_pipeline(c, draw_pipe);
        draw_data dd;
        memcpy(dd.color, de_pipe_test_triangle_color[0], sizeof dd.color);
        dd.positions = de_gpu_buffer_bindless(f->d, pos);
        dd.first = 0;
        de_gpu_cmd_push_constants(c, &dd, sizeof dd);
        de_gpu_cmd_set_index_buffer(c, ib, 0, true);
        de_gpu_cmd_draw_indexed_indirect(c, args, 0, pass == 0 ? count : (de_gpu_buffer){0}, 0,
                                         DE_PIPE_TEST_INDIRECT_RECORDS);
        end_target(c, &t);
        EXPECT_TRUE(de_error_last() == NULL);
        submit_wait(f->d, DE_QUEUE_GRAPHICS, c);
        if (pass == 0) {
            expect_pixels(&t, de_pipe_test_indirect_expect, 3);
        } else {
            de_pipe_test_pixel all[3];
            memcpy(all, de_pipe_test_indirect_expect, sizeof all);
            memcpy(all[2].rgba, all[0].rgba, 4); /* the third draw ran too */
            expect_pixels(&t, all, 3);
        }
    }
    free_target(f->d, &t);
    de_gpu_buffer_destroy(f->d, pos);
    de_gpu_buffer_destroy(f->d, ib);
    de_gpu_buffer_destroy(f->d, args);
    de_gpu_buffer_destroy(f->d, count);
    de_gpu_pipeline_destroy(f->d, args_pipe);
    de_gpu_pipeline_destroy(f->d, draw_pipe);
}

/* ---------- Mesh ---------- */

TEST_F(d3d12, mesh_pipeline, pipe_fx) {
    open_device(f);
    de_pipe_test_shader as, ms, ps;
    memset(&as, 0, sizeof as);
    memset(&ms, 0, sizeof ms);
    memset(&ps, 0, sizeof ps);
    const bool cooked = de_pipe_test_get(DE__SHADER_TARGET_DXIL, "mesh.slang", "as_main", &as) &&
                        de_pipe_test_get(DE__SHADER_TARGET_DXIL, "mesh.slang", "ms_main", &ms) &&
                        de_pipe_test_get(DE__SHADER_TARGET_DXIL, "mesh.slang", "ps_main", &ps);
    if (!cooked)
        SKIP("mesh.slang did not cook to DXIL on this host");
    de_gpu_pipeline_desc pd = {
        .kind = DE_PIPE_MESH,
        .as = as.blob,
        .ms = ms.blob,
        .ps = ps.blob,
        .color_formats = {DE_FMT_RGBA8_UNORM},
        .color_count = 1,
        .cull = DE_CULL_BACK,
        .push_constant_bytes = max_u32(max_u32(as.push_constant_bytes, ms.push_constant_bytes),
                                       ps.push_constant_bytes),
        .threadgroup_size = {ms.threadgroup[0], ms.threadgroup[1], ms.threadgroup[2]},
        .object_threadgroup_size = {as.threadgroup[0], as.threadgroup[1], as.threadgroup[2]},
        .name = "mesh"};
    EXPECT_EQ(ms.threadgroup[0], 3u);
    EXPECT_EQ(as.threadgroup[0], 1u);
    de_error_clear();
    de_gpu_pipeline pipe = de_gpu_pipeline_create(f->d, &pd);
    if (!de_gpu_get_caps(f->d)->mesh_shaders) {
        EXPECT_FALSE(DE_HANDLE_VALID(pipe));
        ASSERT_TRUE(de_error_last() != NULL);
        EXPECT_EQ(de_error_last()->result, DE_ERR_UNSUPPORTED);
        de_error_clear();
        /* Recording a mesh dispatch without a mesh pipeline is refused (Debug/Dev). */
        SKIP(
            "no mesh shaders on this device (D3D12_MESH_SHADER_TIER_1 missing): DE_ERR_UNSUPPORTED "
            "checked; the mesh draw needs hardware with mesh shaders (needs-hw-verify)");
    }
    need_pipelines(f, f->d, "the mesh test");
    if (!DE_HANDLE_VALID(pipe)) {
        char e[600];
        printf("mesh pipeline: %s\n", errchain(e, sizeof e));
    }
    ASSERT_TRUE(DE_HANDLE_VALID(pipe));
    de_gpu_buffer pos = storage(f->d, DE_QUEUE_GRAPHICS, de_pipe_test_triangle_pos,
                                sizeof de_pipe_test_triangle_pos, 16, "mesh positions");
    const u32 one[3] = {1, 1, 1};
    de_gpu_buffer args = upload(f->d, one, sizeof one, DE_USAGE_INDIRECT, 0, "mesh args");
    const u32 cnt[1] = {1};
    de_gpu_buffer count = upload(f->d, cnt, sizeof cnt, DE_USAGE_INDIRECT, 0, "mesh count");
    target t = make_target(f->d, false);
    /* Pass 0: de_gpu_cmd_dispatch_mesh. Pass 1: the indirect-count variant. */
    for (u32 pass = 0; pass < 2; ++pass) {
        de_error_clear();
        de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "mesh");
        ASSERT_TRUE(c != NULL);
        begin_target(c, &t);
        de_gpu_cmd_set_pipeline(c, pipe);
        draw_data dd;
        memcpy(dd.color, de_pipe_test_triangle_color[0], sizeof dd.color);
        dd.positions = de_gpu_buffer_bindless(f->d, pos);
        dd.first = 0;
        de_gpu_cmd_push_constants(c, &dd, sizeof dd);
        if (pass == 0)
            de_gpu_cmd_dispatch_mesh(c, 1, 1, 1);
        else
            de_gpu_cmd_dispatch_mesh_indirect(c, args, 0, count, 0, 1);
        end_target(c, &t);
        EXPECT_TRUE(de_error_last() == NULL);
        submit_wait(f->d, DE_QUEUE_GRAPHICS, c);
        expect_pixels(&t, de_pipe_test_triangle_expect, 3);
    }
    free_target(f->d, &t);
    de_gpu_buffer_destroy(f->d, pos);
    de_gpu_buffer_destroy(f->d, args);
    de_gpu_buffer_destroy(f->d, count);
    de_gpu_pipeline_destroy(f->d, pipe);
}

/* ---------- Push-constant limit ---------- */

TEST_F(d3d12, push_constant_limit, pipe_fx) {
    open_device(f);
    EXPECT_EQ(de_gpu_get_caps(f->d)->max_push_constant_bytes, DE_GPU_MAX_PUSH_CONSTANT_BYTES);
    de_pipe_test_shader cs = shader("push_limit.slang", "cs_main");
    EXPECT_EQ(cs.push_constant_bytes, 128u);
    de_gpu_pipeline_desc pd = {
        .kind = DE_PIPE_COMPUTE, .cs = cs.blob, .push_constant_bytes = 132, .name = "push 132"};
    /* Over the limit: refused by the front-end. */
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_pipeline_create(f->d, &pd)));
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(de_error_last()->message, "limit is 128") != NULL);
    /* Fewer bytes than the shader uses: refused by the DXIL check (on any device). */
    pd.push_constant_bytes = 120;
    pd.name = "push 120";
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_pipeline_create(f->d, &pd)));
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    if (!strstr(de_error_last()->message, "uses 128 bytes of push constants"))
        printf("message: %s\n", de_error_last()->message);
    EXPECT_TRUE(strstr(de_error_last()->message, "uses 128 bytes of push constants") != NULL);
    de_error_clear();
    need_pipelines(f, f->d, "the 128-byte round trip");
    /* Exactly 128: every byte arrives. */
    pd.push_constant_bytes = 128;
    pd.name = "push 128";
    de_gpu_pipeline pipe = de_gpu_pipeline_create(f->d, &pd);
    ASSERT_TRUE(DE_HANDLE_VALID(pipe));
    de_gpu_buffer out = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = 128,
                                    .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                    .stride = 4,
                                    .name = "push out"});
    de_gpu_buffer rb = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = 128,
                                                                        .usage = DE_USAGE_COPY_DST,
                                                                        .memory = DE_MEM_READBACK,
                                                                        .name = "push rb"});
    ASSERT_TRUE(DE_HANDLE_VALID(out) && DE_HANDLE_VALID(rb));
    u32 *res = (u32 *)de_gpu_buffer_map(f->d, rb);
    memset(res, 0, 128);
    u32 words[32];
    for (u32 i = 0; i < 31; ++i)
        words[i] = DE_PIPE_TEST_PUSH_WORD(i);
    words[31] = de_gpu_buffer_bindless(f->d, out);
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "push limit");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier b = buf_barrier(out, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_set_pipeline(c, pipe);
#if DE__GPU_VALIDATE_COMMANDS
    u32 too_many[33] = {0};
    de_error_clear();
    de_gpu_cmd_push_constants(c, too_many, sizeof too_many);
    EXPECT_TRUE(de_error_last() != NULL && de_error_last()->result == DE_ERR_INVALID_ARGUMENT);
#endif
    de_error_clear();
    de_gpu_cmd_push_constants(c, words, sizeof words);
    de_gpu_cmd_dispatch(c, 1, 1, 1);
    b = buf_barrier(out, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_buffer(c, rb, 0, out, 0, 128);
    EXPECT_TRUE(de_error_last() == NULL);
    submit_wait(f->d, DE_QUEUE_GRAPHICS, c);
    EXPECT_MEMEQ(res, words, sizeof words);
    de_gpu_buffer_destroy(f->d, out);
    de_gpu_buffer_destroy(f->d, rb);
    de_gpu_pipeline_destroy(f->d, pipe);
}

/* ---------- Creation checks ---------- */

static void expect_refused(de_gpu_device *d, const de_gpu_pipeline_desc *pd, const char *needle) {
    de_error_clear();
    de_gpu_pipeline p = de_gpu_pipeline_create(d, pd);
    EXPECT_FALSE(DE_HANDLE_VALID(p));
    if (DE_HANDLE_VALID(p)) {
        de_gpu_pipeline_destroy(d, p);
        return;
    }
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    if (!strstr(de_error_last()->message, needle))
        printf("message \"%s\" lacks \"%s\"\n", de_error_last()->message, needle);
    EXPECT_TRUE(strstr(de_error_last()->message, needle) != NULL);
    de_error_clear();
}

TEST_F(d3d12, pipeline_validation, pipe_fx) {
    open_device(f);
    de_pipe_test_shader vs = shader("triangle.slang", "vs_main");
    de_pipe_test_shader ps = shader("triangle.slang", "ps_main");
    de_pipe_test_shader cs = shader("compute.slang", "cs_main");
    /* A vertex shader given as the compute stage. */
    de_gpu_pipeline_desc pd = {.kind = DE_PIPE_COMPUTE, .cs = vs.blob, .push_constant_bytes = 24};
    expect_refused(f->d, &pd, "is a vertex shader");
    /* A blob of a stage the kind does not use (front-end). */
    pd = (de_gpu_pipeline_desc){
        .kind = DE_PIPE_COMPUTE, .cs = cs.blob, .vs = vs.blob, .push_constant_bytes = 16};
    expect_refused(f->d, &pd, "has no vs stage");
    /* Not DXIL. */
    static const u32 junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    pd = (de_gpu_pipeline_desc){
        .kind = DE_PIPE_COMPUTE, .cs = {junk, sizeof junk, "cs_main"}, .push_constant_bytes = 16};
    expect_refused(f->d, &pd, "not DXIL");
    /* A threadgroup size that is not the shader's (64, 1, 1). */
    pd = (de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                .cs = cs.blob,
                                .push_constant_bytes = 16,
                                .threadgroup_size = {32, 1, 1}};
    expect_refused(f->d, &pd, "threadgroup size (32, 1, 1)");
    /* A pixel shader output without a color format. */
    pd = (de_gpu_pipeline_desc){.kind = DE_PIPE_GRAPHICS,
                                .vs = vs.blob,
                                .ps = ps.blob,
                                .push_constant_bytes = 24,
                                .depth_format = DE_FMT_D32_FLOAT,
                                .depth_test = true};
    expect_refused(f->d, &pd, "SV_Target0");
    /* Without pipelines on the device, a good desc is DE_ERR_UNSUPPORTED with the reason. */
    const char *why = NULL;
    if (!de__d3d12_pipelines_supported(f->d, &why)) {
        pd.color_count = 1;
        pd.color_formats[0] = DE_FMT_RGBA8_UNORM;
        de_error_clear();
        EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_pipeline_create(f->d, &pd)));
        ASSERT_TRUE(de_error_last() != NULL);
        EXPECT_EQ(de_error_last()->result, DE_ERR_UNSUPPORTED);
        EXPECT_TRUE(strstr(de_error_last()->message, "shader model") != NULL);
        de_error_clear();
    }
    need_pipelines(f, f->d, "creating the valid pipelines");
    /* The same pipeline with its color format is fine; so are the blend and cull modes, with and
     * without depth. */
    for (u32 blend = DE_BLEND_OPAQUE; blend <= DE_BLEND_ADD; ++blend) {
        pd.color_count = 1;
        pd.color_formats[0] = DE_FMT_RGBA8_UNORM;
        pd.blend[0] = (de_gpu_blend)blend;
        pd.cull = (de_gpu_cull)(blend % 3u);
        pd.depth_format = blend % 2u ? DE_FMT_UNKNOWN : DE_FMT_D32_FLOAT;
        pd.depth_test = blend % 2u == 0;
        pd.depth_write = blend == 2u;
        de_error_clear();
        de_gpu_pipeline p = de_gpu_pipeline_create(f->d, &pd);
        if (!DE_HANDLE_VALID(p)) {
            char e[600];
            printf("blend %u: %s\n", blend, errchain(e, sizeof e));
        }
        EXPECT_TRUE(DE_HANDLE_VALID(p));
        de_gpu_pipeline_destroy(f->d, p);
    }
}

/* ---------- Root signature ---------- */

TEST_F(d3d12, root_signature, pipe_fx) {
    ID3DBlob *blob = NULL;
    ASSERT_EQ(de__d3d12_root_signature_serialize(&blob), DE_OK);
    ID3D12VersionedRootSignatureDeserializer *des = NULL;
    HRESULT hr = D3D12CreateVersionedRootSignatureDeserializer(
        ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
        &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&des);
    ASSERT_TRUE(SUCCEEDED(hr));
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *vd = NULL;
    hr = ID3D12VersionedRootSignatureDeserializer_GetRootSignatureDescAtVersion(
        des, D3D_ROOT_SIGNATURE_VERSION_1_1, &vd);
    ASSERT_TRUE(SUCCEEDED(hr) && vd != NULL);
    EXPECT_EQ((u32)vd->Version, (u32)D3D_ROOT_SIGNATURE_VERSION_1_1);
    EXPECT_EQ((u32)vd->Desc_1_1.Flags,
              (u32)(D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                    D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED));
    ASSERT_EQ(vd->Desc_1_1.NumParameters, 1u);
    const D3D12_ROOT_PARAMETER1 *p = &vd->Desc_1_1.pParameters[0];
    EXPECT_EQ((u32)p->ParameterType, (u32)D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
    EXPECT_EQ(p->Constants.ShaderRegister, 0u);
    EXPECT_EQ(p->Constants.RegisterSpace, 0u);
    EXPECT_EQ(p->Constants.Num32BitValues, 32u);
    EXPECT_EQ((u32)p->ShaderVisibility, (u32)D3D12_SHADER_VISIBILITY_ALL);
    EXPECT_EQ(vd->Desc_1_1.NumStaticSamplers, 0u);
    ID3D12VersionedRootSignatureDeserializer_Release(des);
    ID3D10Blob_Release(blob);

    /* The device: pipelines exactly when it has SM 6.6 and binding tier 3. */
    f->d = make_device(f, "");
    const char *why = NULL;
    const bool ok = de__d3d12_pipelines_supported(f->d, &why);
    const de_gpu_caps *caps = de_gpu_get_caps(f->d);
    printf("device \"%s\" (Base tier %s): pipelines %s %s\n", caps->device_name,
           caps->base_tier_ok ? "yes" : "no", ok ? "yes" : "no", why);
    EXPECT_EQ(ok, caps->base_tier_ok);
    if (!ok)
        EXPECT_TRUE(strstr(why, "shader model") != NULL);
}

/* ---------- Pipeline library ---------- */

static const char *cache_dir(const char *sub) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("d3d12cache", DE_D3D12_TEST_CACHE_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
        mounted = true;
    }
    static char buf[64];
    snprintf(buf, sizeof buf, "d3d12cache:/%s", sub);
    (void)de_dir_create(buf);
    return buf;
}

static void create_pipelines(de_gpu_device *d, de_gpu_pipeline out[3]) {
    out[0] = triangle_pipeline(d, true);
    out[1] = compute_pipeline(d, "compute.slang", "compute");
    out[2] = compute_pipeline(d, "indirect_args.slang", "indirect args");
}

static void destroy_pipelines(de_gpu_device *d, de_gpu_pipeline p[3]) {
    for (u32 i = 0; i < 3; ++i)
        de_gpu_pipeline_destroy(d, p[i]);
}

TEST_F(d3d12, pipeline_library_roundtrip, pipe_fx) {
    need_shaders(f);
    const char *dir = cache_dir("roundtrip");
    de_gpu_device *d = make_device(f, dir);
    const char *p = NULL;
    u32 hits = 0, misses = 0;
    u64 loaded = 0;
    u32 state = de__d3d12_pipeline_library_state(d, &hits, &misses, &loaded, &p);
    char path[512];
    snprintf(path, sizeof path, "%s", p);
    printf("library file: %s (state %u)\n", path, state);
    if (state == DE__D3D12_LIBRARY_OFF) {
        end_device(d);
        SKIP("this device has no pipeline library support");
    }
    ASSERT_TRUE(strstr(path, "d3d12cache:/roundtrip/d3d12-") == path);
    if (de_path_exists(path)) {
        end_device(d);
        ASSERT_EQ(de_file_delete(path), DE_OK);
        d = make_device(f, dir);
        state = de__d3d12_pipeline_library_state(d, &hits, &misses, &loaded, NULL);
    }
    EXPECT_EQ(state, (u32)DE__D3D12_LIBRARY_EMPTY);
    EXPECT_EQ(loaded, 0u);
    const char *why = NULL;
    if (!de__d3d12_pipelines_supported(d, &why)) {
        end_device(d);
        EXPECT_FALSE(de_path_exists(path)); /* nothing stored, nothing saved */
        need_pipelines(f, NULL, "the library round trip");
    }
    de_gpu_pipeline pipes[3];
    u64 t0 = de_time_now_ns();
    create_pipelines(d, pipes);
    const u64 cold = de_time_now_ns() - t0;
    de__d3d12_pipeline_library_state(d, &hits, &misses, NULL, NULL);
    EXPECT_EQ(hits, 0u);
    EXPECT_EQ(misses, 3u);
    /* The same desc again on this device: the library has it now. */
    de_gpu_pipeline again = triangle_pipeline(d, true);
    de__d3d12_pipeline_library_state(d, &hits, &misses, NULL, NULL);
    EXPECT_EQ(hits, 1u);
    de_gpu_pipeline_destroy(d, again);
    destroy_pipelines(d, pipes);
    end_device(d);

    void *file = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all(path, NULL, &file, &size), DE_OK);
    EXPECT_GT(size, (u64)sizeof(de__d3d12_pipeline_library_file) + 3u * 8u);
    de__d3d12_pipeline_library_file h;
    memcpy(&h, file, sizeof h);
    EXPECT_EQ(h.key_count, 3u);
    DE_FREE(de_heap_allocator(), file, (size_t)size + 1u);

    /* Second run: the library loads and every pipeline comes from it. */
    d = make_device(f, dir);
    state = de__d3d12_pipeline_library_state(d, &hits, &misses, &loaded, NULL);
    EXPECT_EQ(state, (u32)DE__D3D12_LIBRARY_LOADED);
    EXPECT_GT(loaded, 0u);
    t0 = de_time_now_ns();
    create_pipelines(d, pipes);
    const u64 warm = de_time_now_ns() - t0;
    de__d3d12_pipeline_library_state(d, &hits, &misses, NULL, NULL);
    EXPECT_EQ(hits, 3u);
    EXPECT_EQ(misses, 0u);
    printf("pipeline creation: %.3f ms without the library, %.3f ms from it (%llu bytes)\n",
           (double)cold / 1e6, (double)warm / 1e6, (unsigned long long)loaded);
    destroy_pipelines(d, pipes);
    end_device(d);
}

/* Counts warnings about the pipeline library (log sink). */
static de_atomic_u32 g_library_warnings;
static void library_sink(const de_log_record *rec, void *user) {
    (void)user;
    if (rec->level == DE_LOG_WARN && rec->message.len > 0 &&
        strstr(rec->message.ptr, "pipeline library") != NULL)
        de_atomic_fetch_add_u32(&g_library_warnings, 1u, DE_ATOMIC_RELAXED);
}

/* Writes `bytes` as the library file, then checks that a device discards it and still works. */
static void expect_discarded(pipe_fx *f, const char *dir, const char *path, const void *bytes,
                             u64 size, const char *what) {
    printf("case: %s\n", what);
    ASSERT_EQ(de_file_write_all_atomic(path, bytes, size), DE_OK);
    de_gpu_device *d = make_device(f, dir);
    u64 loaded = 1;
    u32 hits = 9, misses = 0;
    EXPECT_EQ(de__d3d12_pipeline_library_state(d, &hits, &misses, &loaded, NULL),
              (u32)DE__D3D12_LIBRARY_REJECTED);
    EXPECT_EQ(loaded, 0u);
    if (de__d3d12_pipelines_supported(d, NULL)) {
        de_gpu_pipeline pipes[3];
        create_pipelines(d, pipes);
        destroy_pipelines(d, pipes);
        de__d3d12_pipeline_library_state(d, &hits, &misses, NULL, NULL);
        EXPECT_EQ(hits, 0u); /* rebuilt */
        EXPECT_EQ(misses, 3u);
    }
    end_device(d);
}

TEST_F(d3d12, pipeline_library_mismatch, pipe_fx) {
    need_shaders(f);
    const char *dir = cache_dir("mismatch");
    de_gpu_device *d = make_device(f, dir);
    const char *p = NULL;
    const u32 state = de__d3d12_pipeline_library_state(d, NULL, NULL, NULL, &p);
    char path[512];
    snprintf(path, sizeof path, "%s", p);
    const bool pipelines = de__d3d12_pipelines_supported(d, NULL);
    const de_gpu_caps caps = *de_gpu_get_caps(d);
    end_device(d);
    if (state == DE__D3D12_LIBRARY_OFF)
        SKIP("this device has no pipeline library support");
    /* The driver version is in the file name: d3d12-<vendor>-<device>-<a.b.c.d>.bin. */
    unsigned v[4] = {0, 0, 0, 0};
    const char *name = strstr(path, "/d3d12-");
    ASSERT_TRUE(name != NULL);
    char *at = NULL;
    const unsigned vendor = (unsigned)strtoul(name + 7, &at, 16);
    ASSERT_TRUE(*at == '-');
    const unsigned device = (unsigned)strtoul(at + 1, &at, 16);
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_TRUE(*at == (i == 0 ? '-' : '.'));
        v[i] = (unsigned)strtoul(at + 1, &at, 10);
    }
    ASSERT_TRUE(strcmp(at, ".bin") == 0);
    EXPECT_EQ(vendor, caps.vendor_id);
    EXPECT_EQ(device, caps.device_id);
    const u64 driver = (u64)v[0] << 48 | (u64)v[1] << 32 | (u64)v[2] << 16 | (u64)v[3];

    /* A good file: from a device that stores pipelines, else made here (never loaded by these
     * cases: each is refused before the runtime sees it). */
    static u8 good[1u << 20], bad[1u << 20];
    u64 size = 0;
    if (pipelines) {
        (void)de_file_delete(path);
        d = make_device(f, dir);
        de_gpu_pipeline pipes[3];
        create_pipelines(d, pipes);
        destroy_pipelines(d, pipes);
        end_device(d);
        void *file = NULL;
        ASSERT_EQ(de_file_read_all(path, NULL, &file, &size), DE_OK);
        ASSERT_LE(size, (u64)sizeof good);
        memcpy(good, file, (size_t)size);
        DE_FREE(de_heap_allocator(), file, (size_t)size + 1u);
    } else {
        de__d3d12_pipeline_library_file h;
        memset(&h, 0, sizeof h);
        h.magic = DE__D3D12_PIPELINE_LIBRARY_MAGIC;
        h.version = DE__D3D12_PIPELINE_LIBRARY_VERSION;
        h.vendor_id = caps.vendor_id;
        h.device_id = caps.device_id;
        h.driver_version = driver;
        h.key_count = 1;
        h.data_size = 200;
        for (u32 i = 0; i < 208; ++i)
            good[sizeof h + i] = (u8)(i * 13u + 1u);
        h.data_hash = de_hash64(good + sizeof h, 208, DE__D3D12_PIPELINE_LIBRARY_MAGIC);
        memcpy(good, &h, sizeof h);
        size = sizeof h + 208;
    }
    char why[300];
    de__d3d12_pipeline_library_view view;
    ASSERT_TRUE(de__d3d12_pipeline_library_check(good, size, caps.vendor_id, caps.device_id, driver,
                                                 &view, why, sizeof why));

    const bool log_started = de_log_init(&(de_log_desc){.no_console_sink = false}) == DE_OK;
    de_error_clear();
    de_log_add_sink(library_sink, NULL);
    de_log_flush();
    const u32 before = de_atomic_load_u32(&g_library_warnings, DE_ATOMIC_RELAXED);

    /* Another driver version (checksum still right: the header is not covered). */
    memcpy(bad, good, (size_t)size);
    de__d3d12_pipeline_library_file h;
    memcpy(&h, bad, sizeof h);
    h.driver_version ^= 0x10000u;
    memcpy(bad, &h, sizeof h);
    expect_discarded(f, dir, path, bad, size, "a library from another driver version");
    /* One data byte flipped. */
    memcpy(bad, good, (size_t)size);
    bad[size - 5] ^= 0x5A;
    expect_discarded(f, dir, path, bad, size, "a flipped data byte");
    /* Truncated. */
    expect_discarded(f, dir, path, good, size - 16, "truncated");
    /* Not a library. */
    expect_discarded(f, dir, path, "hello, not a library", 20, "garbage");

    de_log_flush();
    const u32 warnings = de_atomic_load_u32(&g_library_warnings, DE_ATOMIC_RELAXED) - before;
    de_log_remove_sink(library_sink, NULL);
    if (log_started)
        de_log_shutdown();
    printf("pipeline library warnings logged: %u\n", warnings);
    EXPECT_EQ(warnings, 4u); /* one per discarded file */
    if (pipelines) {
        /* The last device saved a good library over the bad file. */
        void *again = NULL;
        u64 again_size = 0;
        ASSERT_EQ(de_file_read_all(path, NULL, &again, &again_size), DE_OK);
        EXPECT_TRUE(de__d3d12_pipeline_library_check(
            again, again_size, caps.vendor_id, caps.device_id, driver, &view, why, sizeof why));
        DE_FREE(de_heap_allocator(), again, (size_t)again_size + 1u);
    }
}

#endif /* _WIN32 && DE_HAS_GPU_D3D12 */
