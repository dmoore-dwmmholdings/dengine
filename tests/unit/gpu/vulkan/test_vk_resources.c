/* test_vk_resources.c — Vulkan buffers, textures, samplers, memory and bindless descriptors
 * (DENG-283). CTest names: gpu_vulkan.vk.<test>.
 *
 * Every test runs with debug_layer on: when VK_LAYER_KHRONOS_validation is installed, it must
 * report no error. Without a Vulkan driver the tests SKIP (see test_vk_device.c). GPU copies
 * (upload -> GPU -> readback) need command lists and are tested by DENG-284.
 */
#include "de_test.h"

#include <dengine/de_gpu.h>
#include <dengine/de_platform.h>

#include "gpu/vulkan/vk_internal.h"

#include <stdio.h>
#include <string.h>

#define KIB ((u64)1 << 10)
#define MIB ((u64)1 << 20)

static const char *errmsg(void) {
    const de_error *e = de_error_last();
    return e ? e->message : "";
}

/* The whole error chain ("outer: inner: ..."), for skip reasons. */
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

/* A Vulkan device on a tracking allocator (every test also checks for leaks), debug layer on. */
typedef struct vk_fx {
    de_tracking_allocator tracker;
    de_allocator alloc;
    de_gpu_device *d;
    char skip[1200];
    char err[1100];
} vk_fx;

static void vk_fx_setup(vk_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
    de_gpu_device_desc desc = {
        .backend = DE_GPU_VULKAN, .debug_layer = true, .allocator = &f->alloc};
    de_result r = de_gpu_device_create(&desc, &f->d);
    if (r == DE_ERR_UNSUPPORTED) {
        snprintf(f->skip, sizeof f->skip, "no usable Vulkan device: %s",
                 errchain(f->err, sizeof f->err));
        SKIP(f->skip);
    }
    ASSERT_EQ(r, DE_OK);
    if (!de__gpu_vk_validation_enabled(f->d))
        printf("note: VK_LAYER_KHRONOS_validation is not installed; validation is not checked\n");
}

static void vk_fx_teardown(vk_fx *f) {
    if (f->d) {
        u32 warnings = 0;
        EXPECT_EQ(de__gpu_vk_validation_messages(f->d, &warnings), 0u);
        de_gpu_device_destroy(f->d);
    }
    de_tracking_stats st = de_tracking_allocator_stats(&f->tracker);
    EXPECT_EQ(st.blocks_live, 0u);
    EXPECT_EQ(st.bytes_live, 0u);
}

static u64 class_used(de_gpu_device *d) {
    u64 used = 0;
    for (u32 c = 0; c < de__gpu_mem_class_count(d); ++c) {
        de__gpu_mem_stats st;
        if (de__gpu_mem_class_stats(d, c, &st) == DE_OK)
            used += st.used;
    }
    return used;
}

/* ---------- 1000 of each ---------- */

#define N 1000

static de_gpu_buffer_desc buffer_desc(u32 i) {
    static const u32 k_usage[] = {
        DE_USAGE_STORAGE,  DE_USAGE_VERTEX | DE_USAGE_COPY_DST,  DE_USAGE_INDEX | DE_USAGE_COPY_DST,
        DE_USAGE_CONSTANT, DE_USAGE_INDIRECT | DE_USAGE_STORAGE, DE_USAGE_COPY_SRC,
    };
    de_gpu_buffer_desc d;
    memset(&d, 0, sizeof d);
    d.usage = k_usage[i % 6];
    d.size = 256 + (u64)(i * 977u) % (256 * KIB);
    d.memory = DE_MEM_GPU_ONLY;
    if (i % 7 == 3) {
        d.memory = DE_MEM_UPLOAD;
        d.usage = DE_USAGE_COPY_SRC | DE_USAGE_VERTEX;
    } else if (i % 7 == 5) {
        d.memory = DE_MEM_READBACK;
        d.usage = DE_USAGE_COPY_DST;
    }
    d.name = "vk-test-buffer";
    return d;
}

static de_gpu_texture_desc texture_desc(u32 i) {
    de_gpu_texture_desc d;
    memset(&d, 0, sizeof d);
    d.name = "vk-test-texture";
    switch (i % 8) {
    case 0: /* 2D with mips */
        d.format = DE_FMT_RGBA8_UNORM;
        d.width = d.height = 64;
        d.mip_count = 4;
        d.usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST;
        break;
    case 1: /* storage mips */
        d.format = DE_FMT_RGBA16_FLOAT;
        d.width = d.height = 32;
        d.mip_count = 3;
        d.usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE;
        break;
    case 2: /* render target */
        d.format = DE_FMT_RGBA8_SRGB;
        d.width = 48;
        d.height = 32;
        d.usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC;
        break;
    case 3: /* depth */
        d.format = DE_FMT_D32_FLOAT;
        d.width = d.height = 64;
        d.usage = DE_USAGE_DEPTH | DE_USAGE_SAMPLED;
        break;
    case 4: /* array */
        d.dim = DE_TEX_2D_ARRAY;
        d.format = DE_FMT_RGBA8_UNORM; /* storage support is mandatory for RGBA8, not R8 */
        d.width = d.height = 16;
        d.depth_or_layers = 4;
        d.mip_count = 2;
        d.usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE;
        break;
    case 5: /* cube */
        d.dim = DE_TEX_CUBE;
        d.format = DE_FMT_R11G11B10_FLOAT;
        d.width = d.height = 16;
        d.mip_count = 5;
        d.usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST;
        break;
    case 6: /* volume */
        d.dim = DE_TEX_3D;
        d.format = DE_FMT_R32_FLOAT;
        d.width = d.height = 16;
        d.depth_or_layers = 8;
        d.mip_count = 2;
        d.usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE;
        break;
    default: /* MSAA target */
        d.format = DE_FMT_RGBA8_UNORM;
        d.width = d.height = 32;
        d.sample_count = 4;
        d.usage = DE_USAGE_RENDER_TARGET;
        break;
    }
    return d;
}

static de_gpu_sampler_desc sampler_desc(u32 i) {
    de_gpu_sampler_desc d;
    memset(&d, 0, sizeof d);
    d.min = d.mag = (i & 1) ? DE_FILTER_LINEAR : DE_FILTER_NEAREST;
    d.mip = (i & 2) ? DE_FILTER_LINEAR : DE_FILTER_NEAREST;
    d.u = d.v = d.w = (de_gpu_address)(i % 4);
    d.max_aniso = (i % 5 == 0) ? 16.0f : 0.0f;
    d.compare_reverse_z = (i % 9 == 0);
    return d;
}

/* Creates nb buffers, nt textures and ns samplers of every kind above, checks counts and maps,
 * destroys them and checks that slots, indices and memory all come back. */
static void create_destroy_all(de_gpu_device *d, u32 nb, u32 nt, u32 ns) {
    static de_gpu_buffer b[N];
    static de_gpu_texture t[N];
    static de_gpu_sampler s[N];
    ASSERT_TRUE(nb <= N && nt <= N && ns <= N);
    for (u32 i = 0; i < N; ++i) {
        if (i < nb) {
            de_gpu_buffer_desc bd = buffer_desc(i);
            b[i] = de_gpu_buffer_create(d, &bd);
            ASSERT_TRUE(DE_HANDLE_VALID(b[i]));
        }
        if (i < nt) {
            de_gpu_texture_desc td = texture_desc(i);
            t[i] = de_gpu_texture_create(d, &td);
            if (!DE_HANDLE_VALID(t[i]))
                printf("texture %u (case %u): %s\n", i, i % 8, errmsg());
            ASSERT_TRUE(DE_HANDLE_VALID(t[i]));
        }
        if (i < ns) {
            de_gpu_sampler_desc sd = sampler_desc(i);
            s[i] = de_gpu_sampler_create(d, &sd);
            ASSERT_TRUE(DE_HANDLE_VALID(s[i]));
        }
    }
    EXPECT_EQ(de__gpu_live_count(d, DE__GPU_BUFFER), nb);
    EXPECT_EQ(de__gpu_live_count(d, DE__GPU_TEXTURE), nt);
    EXPECT_EQ(de__gpu_live_count(d, DE__GPU_SAMPLER), ns);
    EXPECT_EQ(de__gpu_bindless_count(d, DE__GPU_BINDLESS_SAMPLER), ns);
    EXPECT_GT(class_used(d), (u64)0);

    /* UPLOAD and READBACK buffers are mapped. */
    for (u32 i = 0; i < nb; ++i) {
        de_gpu_buffer_desc bd = buffer_desc(i);
        if (bd.memory != DE_MEM_GPU_ONLY)
            EXPECT_TRUE(de_gpu_buffer_map(d, b[i]) != NULL);
    }

    for (u32 i = 0; i < N; ++i) {
        if (i < nb)
            de_gpu_buffer_destroy(d, b[i]);
        if (i < nt)
            de_gpu_texture_destroy(d, t[i]);
        if (i < ns)
            de_gpu_sampler_destroy(d, s[i]);
    }
    de_gpu_wait_idle(d); /* releases the deferred destructions */
    EXPECT_EQ(de__gpu_deferred_count(d), 0u);
    EXPECT_EQ(de__gpu_live_count(d, DE__GPU_BUFFER), 0u);
    EXPECT_EQ(de__gpu_live_count(d, DE__GPU_TEXTURE), 0u);
    EXPECT_EQ(de__gpu_live_count(d, DE__GPU_SAMPLER), 0u);
    for (u32 sp = 0; sp < DE__GPU_BINDLESS_SPACE_COUNT; ++sp)
        EXPECT_EQ(de__gpu_bindless_count(d, (de__gpu_bindless_space)sp), 0u);
    EXPECT_EQ(class_used(d), (u64)0);

    /* A second round reuses slots, indices and heap memory. */
    for (u32 i = 0; i < nb; ++i) {
        de_gpu_buffer_desc bd = buffer_desc(nb - 1 - i);
        b[i] = de_gpu_buffer_create(d, &bd);
        ASSERT_TRUE(DE_HANDLE_VALID(b[i]));
    }
    for (u32 i = 0; i < nb; ++i)
        de_gpu_buffer_destroy(d, b[i]);
    de_gpu_wait_idle(d);
    EXPECT_EQ(class_used(d), (u64)0);
}

TEST_F(vk, resources_create_destroy_1000, vk_fx) {
    create_destroy_all(f->d, N, N, N);
}

static u32 min_u32(u32 a, u32 b) {
    return a < b ? a : b;
}

/*
 * A device forced with de_gpu_device_desc.adapter is created even when it is not Base-tier
 * capable (MoltenVK refuses the full bindless set: its tables are halved until the set is
 * supported). Its caps say why, and resources work within its tables, validation-clean.
 */
TEST(vk, forced_adapter_resources) {
    static de_tracking_allocator tracker;
    de_allocator alloc = de_tracking_allocator_init(&tracker, NULL, DE_MEM_RENDER);
    de_gpu_device *d = NULL;
    de_gpu_device_desc desc = {
        .backend = DE_GPU_VULKAN, .adapter = 1, .debug_layer = true, .allocator = &alloc};
    de_result r = de_gpu_device_create(&desc, &d);
    if (r == DE_ERR_UNSUPPORTED) {
        static char skip[1200], err[1100];
        snprintf(skip, sizeof skip, "no Vulkan device: %s", errchain(err, sizeof err));
        SKIP(skip);
    }
    ASSERT_EQ(r, DE_OK);
    const de_gpu_caps *c = de_gpu_get_caps(d);
    printf("forced adapter 1: \"%s\", base tier %s%s%s; tables %u/%u/%u/%u\n", c->device_name,
           c->base_tier_ok ? "ok" : "NOT ok", c->base_tier_ok ? "" : ": ", c->base_tier_reason,
           c->max_bindless_textures, c->max_bindless_storage_textures, c->max_bindless_samplers,
           c->max_bindless_buffers);
    EXPECT_EQ(c->base_tier_ok, c->base_tier_reason[0] == '\0');
    EXPECT_GE(c->max_bindless_samplers, 2u);

    /* Per 8 textures of texture_desc: 7 sampled indices and 7 storage indices. */
    const u32 tex_room = min_u32(c->max_bindless_textures, c->max_bindless_storage_textures) - 1u;
    const u32 nt = min_u32(N, tex_room * 8u / 7u - 8u);
    const u32 nb = min_u32(N, c->max_bindless_buffers - 1u);
    const u32 ns = min_u32(N, c->max_bindless_samplers - 1u);
    create_destroy_all(d, nb, nt, ns);

    u32 warnings = 0;
    EXPECT_EQ(de__gpu_vk_validation_messages(d, &warnings), 0u);
    de_gpu_device_destroy(d);
    de_tracking_stats st = de_tracking_allocator_stats(&tracker);
    EXPECT_EQ(st.blocks_live, 0u);
    EXPECT_EQ(st.bytes_live, 0u);
}

/* ---------- Mapping ---------- */

TEST_F(vk, buffer_map_upload_readback, vk_fx) {
    enum { B = 24 };
    de_gpu_buffer bufs[B];
    u8 *maps[B];
    u64 sizes[B];
    for (u32 i = 0; i < B; ++i) {
        de_gpu_buffer_desc bd;
        memset(&bd, 0, sizeof bd);
        bd.size = sizes[i] = 4 * KIB + (u64)i * 3001u;
        bd.memory = (i & 1) ? DE_MEM_READBACK : DE_MEM_UPLOAD;
        bd.usage = (i & 1) ? DE_USAGE_COPY_DST : (DE_USAGE_COPY_SRC | DE_USAGE_VERTEX);
        bd.name = (i & 1) ? "readback" : "upload";
        bufs[i] = de_gpu_buffer_create(f->d, &bd);
        ASSERT_TRUE(DE_HANDLE_VALID(bufs[i]));
        maps[i] = (u8 *)de_gpu_buffer_map(f->d, bufs[i]);
        ASSERT_TRUE(maps[i] != NULL);
        EXPECT_EQ((uintptr_t)maps[i] % 16u, (uintptr_t)0);
        /* Persistent: the same pointer every time. */
        EXPECT_TRUE(de_gpu_buffer_map(f->d, bufs[i]) == maps[i]);
    }
    /* Distinct buffers never overlap: fill all, then check all. */
    for (u32 i = 0; i < B; ++i)
        for (u64 k = 0; k < sizes[i]; ++k)
            maps[i][k] = (u8)(i * 31u + (u32)k * 7u);
    u32 bad = 0;
    for (u32 i = 0; i < B; ++i)
        for (u64 k = 0; k < sizes[i]; ++k)
            bad += maps[i][k] != (u8)(i * 31u + (u32)k * 7u);
    EXPECT_EQ(bad, 0u);

    /* A large upload buffer (above the dedicated threshold) is mapped too. */
    de_gpu_buffer_desc big = {
        .size = 80 * MIB, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD, .name = "big"};
    de_gpu_buffer bb = de_gpu_buffer_create(f->d, &big);
    ASSERT_TRUE(DE_HANDLE_VALID(bb));
    u8 *bm = (u8 *)de_gpu_buffer_map(f->d, bb);
    ASSERT_TRUE(bm != NULL);
    bm[0] = 1;
    bm[big.size - 1] = 2;
    EXPECT_EQ(bm[0] + bm[big.size - 1], 3);
    de_gpu_buffer_destroy(f->d, bb);

    /* GPU_ONLY buffers cannot be mapped. */
    de_gpu_buffer_desc gd = {.size = 4 * KIB, .usage = DE_USAGE_STORAGE, .name = "gpu"};
    de_gpu_buffer g = de_gpu_buffer_create(f->d, &gd);
    ASSERT_TRUE(DE_HANDLE_VALID(g));
    EXPECT_TRUE(de_gpu_buffer_map(f->d, g) == NULL);
    EXPECT_TRUE(strstr(errmsg(), "GPU_ONLY") != NULL);
    de_gpu_buffer_destroy(f->d, g);

    for (u32 i = 0; i < B; ++i)
        de_gpu_buffer_destroy(f->d, bufs[i]);
    de_gpu_wait_idle(f->d);
    EXPECT_EQ(class_used(f->d), (u64)0);
}

/* ---------- Bindless ---------- */

TEST_F(vk, bindless_indices, vk_fx) {
#if !DE__GPU_VALIDATE_COMMANDS
    SKIP("the bindless shadow tables exist in Debug and Dev builds only");
#else
    u64 h = 0;
    /* Index 0 of every table holds the zeroed dummy. */
    const de__gpu_bindless_space spaces[] = {DE__GPU_BINDLESS_SAMPLED_TEXTURE,
                                             DE__GPU_BINDLESS_STORAGE_TEXTURE,
                                             DE__GPU_BINDLESS_BUFFER, DE__GPU_BINDLESS_SAMPLER};
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_TRUE(de__gpu_vk_bindless_written(f->d, spaces[i], 0, &h));
        EXPECT_NE(h, (u64)0);
    }

    de_gpu_buffer_desc bd = {.size = 64 * KIB, .usage = DE_USAGE_STORAGE, .name = "sb"};
    de_gpu_buffer b = de_gpu_buffer_create(f->d, &bd);
    de_gpu_texture_desc td = {.format = DE_FMT_RGBA8_UNORM,
                              .width = 64,
                              .height = 64,
                              .mip_count = 3,
                              .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE,
                              .name = "tex"};
    de_gpu_texture t = de_gpu_texture_create(f->d, &td);
    de_gpu_sampler s = de_gpu_sampler_create(f->d, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR});
    ASSERT_TRUE(DE_HANDLE_VALID(b) && DE_HANDLE_VALID(t) && DE_HANDLE_VALID(s));

    const u32 bi = de_gpu_buffer_bindless(f->d, b);
    ASSERT_NE(bi, DE_GPU_BINDLESS_INVALID);
    ASSERT_TRUE(de__gpu_vk_bindless_written(f->d, DE__GPU_BINDLESS_BUFFER, bi, &h));
    EXPECT_EQ(h, de__gpu_vk_buffer_handle(f->d, b));
    EXPECT_NE(h, (u64)0);

    const u32 ti = de_gpu_texture_bindless(f->d, t, 0, false);
    ASSERT_NE(ti, DE_GPU_BINDLESS_INVALID);
    ASSERT_TRUE(de__gpu_vk_bindless_written(f->d, DE__GPU_BINDLESS_SAMPLED_TEXTURE, ti, &h));
    EXPECT_EQ(h, de__gpu_vk_texture_view(f->d, t, 0, false));
    EXPECT_NE(h, (u64)0);
    u32 storage_idx[3];
    for (u32 m = 0; m < 3; ++m) {
        storage_idx[m] = de_gpu_texture_bindless(f->d, t, m, true);
        ASSERT_NE(storage_idx[m], DE_GPU_BINDLESS_INVALID);
        ASSERT_TRUE(de__gpu_vk_bindless_written(f->d, DE__GPU_BINDLESS_STORAGE_TEXTURE,
                                                storage_idx[m], &h));
        EXPECT_EQ(h, de__gpu_vk_texture_view(f->d, t, m, true));
        EXPECT_NE(h, (u64)0);
        if (m)
            EXPECT_NE(storage_idx[m], storage_idx[m - 1]);
    }

    const u32 si = de_gpu_sampler_bindless(f->d, s);
    ASSERT_NE(si, DE_GPU_BINDLESS_INVALID);
    ASSERT_TRUE(de__gpu_vk_bindless_written(f->d, DE__GPU_BINDLESS_SAMPLER, si, &h));
    EXPECT_EQ(h, de__gpu_vk_sampler_handle(f->d, s));

    /* After the GPU is done, a new object that gets the same index gets its own descriptor. */
    de_gpu_buffer_destroy(f->d, b);
    de_gpu_wait_idle(f->d);
    de_gpu_buffer b2 = de_gpu_buffer_create(f->d, &bd);
    ASSERT_TRUE(DE_HANDLE_VALID(b2));
    EXPECT_EQ(de_gpu_buffer_bindless(f->d, b2), bi); /* LIFO reuse */
    ASSERT_TRUE(de__gpu_vk_bindless_written(f->d, DE__GPU_BINDLESS_BUFFER, bi, &h));
    EXPECT_EQ(h, de__gpu_vk_buffer_handle(f->d, b2));

    de_gpu_buffer_destroy(f->d, b2);
    de_gpu_texture_destroy(f->d, t);
    de_gpu_sampler_destroy(f->d, s);
#endif
}

/* ---------- Placed resources (transient regions) ---------- */

TEST_F(vk, placed_resources, vk_fx) {
    /* Textures: a region in a class from the texture's requirements; two aliased textures. */
    de_gpu_texture_desc td = {.format = DE_FMT_RGBA16_FLOAT,
                              .width = 256,
                              .height = 256,
                              .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED,
                              .name = "transient"};
    de_gpu_texture_desc full = td;
    full.height = 256;
    full.depth_or_layers = 1;
    full.mip_count = 1;
    full.sample_count = 1;
    de__gpu_mem_req req;
    ASSERT_EQ(de__gpu_texture_requirements(f->d, &full, &req), DE_OK);
    EXPECT_GT(req.size, (u64)0);
    EXPECT_NE(req.class_mask, 0u);
    u32 cls = 0;
    while (!(req.class_mask & (1u << cls)))
        ++cls;
    de__gpu_transient_region *r = NULL;
    ASSERT_EQ(de__gpu_transient_region_create(f->d, cls, 2 * req.size + 64 * KIB, &r), DE_OK);
    de_gpu_texture t0 = de__gpu_place_transient_texture(r, 0, &td);
    if (!DE_HANDLE_VALID(t0))
        printf("place: %s\n", errmsg());
    ASSERT_TRUE(DE_HANDLE_VALID(t0));
    de_gpu_texture t1 = de__gpu_place_transient_texture(r, 0, &td); /* aliases t0 */
    ASSERT_TRUE(DE_HANDLE_VALID(t1));
    EXPECT_NE(de_gpu_texture_bindless(f->d, t0, 0, false),
              de_gpu_texture_bindless(f->d, t1, 0, false));
    EXPECT_EQ(de__gpu_transient_region_placed(r), 2u);
    de_gpu_texture_destroy(f->d, t0);
    de_gpu_texture_destroy(f->d, t1);
    de__gpu_transient_region_destroy(r);

    /* An UPLOAD region: a placed buffer maps into the region's heap mapping. */
    de_gpu_buffer_desc bd = {
        .size = 64 * KIB, .usage = DE_USAGE_COPY_SRC, .memory = DE_MEM_UPLOAD, .name = "up"};
    ASSERT_EQ(de__gpu_buffer_requirements(f->d, &bd, &req), DE_OK);
    EXPECT_TRUE(req.class_mask & (1u << DE__GPU_MEM_CLASS_UPLOAD));
    ASSERT_EQ(de__gpu_transient_region_create(f->d, DE__GPU_MEM_CLASS_UPLOAD, MIB, &r), DE_OK);
    const de__gpu_alloc *ra = de__gpu_transient_region_alloc(r);
    ASSERT_TRUE(ra->heap->mapped != NULL);
    const u64 off = 128 * KIB;
    de_gpu_buffer pb = de__gpu_place_transient_buffer(r, off, &bd);
    ASSERT_TRUE(DE_HANDLE_VALID(pb));
    u8 *p = (u8 *)de_gpu_buffer_map(f->d, pb);
    EXPECT_TRUE(p == (u8 *)ra->heap->mapped + ra->offset + off);
    memset(p, 0xab, (size_t)bd.size);
    de_gpu_buffer_destroy(f->d, pb);
    de__gpu_transient_region_destroy(r);

    de_gpu_wait_idle(f->d);
    EXPECT_EQ(class_used(f->d), (u64)0);
}

/* ---------- Errors ---------- */

TEST_F(vk, unsupported_texture_is_an_error, vk_fx) {
    /* Combinations the front-end accepts but Vulkan devices may not support. Each one this device
     * refuses must fail cleanly with DE_ERR_UNSUPPORTED naming the texture, before vkCreateImage.
     */
    const de_gpu_texture_desc probes[] = {
        {.format = DE_FMT_RGB10A2_UNORM,
         .width = 64,
         .height = 64,
         .usage = DE_USAGE_STORAGE,
         .name = "probe-rgb10a2-storage"},
        {.format = DE_FMT_R11G11B10_FLOAT,
         .width = 64,
         .height = 64,
         .usage = DE_USAGE_STORAGE,
         .name = "probe-r11g11b10-storage"},
        {.format = DE_FMT_RGBA32_FLOAT,
         .width = 64,
         .height = 64,
         .sample_count = 8,
         .usage = DE_USAGE_RENDER_TARGET,
         .name = "probe-rgba32f-msaa8"},
        {.format = DE_FMT_D32_FLOAT_S8,
         .width = 64,
         .height = 64,
         .sample_count = 8,
         .usage = DE_USAGE_DEPTH,
         .name = "probe-d32s8-msaa8"},
        {.dim = DE_TEX_3D,
         .format = DE_FMT_BC7_UNORM,
         .width = 64,
         .height = 64,
         .depth_or_layers = 64,
         .usage = DE_USAGE_SAMPLED,
         .name = "probe-bc7-3d"},
    };
    u32 refused = 0;
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; ++i) {
        de_error_clear();
        de_gpu_texture t = de_gpu_texture_create(f->d, &probes[i]);
        if (DE_HANDLE_VALID(t)) {
            de_gpu_texture_destroy(f->d, t);
            continue;
        }
        ++refused;
        printf("refused: %s\n", errmsg());
        EXPECT_EQ(de_error_last() ? de_error_last()->result : DE_OK, DE_ERR_UNSUPPORTED);
        EXPECT_TRUE(strstr(errmsg(), probes[i].name) != NULL);
    }
    de_gpu_wait_idle(f->d);
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_TEXTURE), 0u);
    EXPECT_EQ(de__gpu_bindless_count(f->d, DE__GPU_BINDLESS_STORAGE_TEXTURE), 0u);
    EXPECT_EQ(class_used(f->d), (u64)0);
    if (refused == 0)
        SKIP("this device supports every probe combination");
}

/* ---------- Threads ---------- */

typedef struct mt_ctx {
    de_gpu_device *d;
    u32 seed;
    u32 failures;
} mt_ctx;

static i32 mt_main(void *user) {
    mt_ctx *c = (mt_ctx *)user;
    for (u32 i = 0; i < 200; ++i) {
        const u32 k = c->seed * 1000u + i;
        de_gpu_buffer_desc bd = buffer_desc(k);
        de_gpu_texture_desc td = texture_desc(k);
        de_gpu_sampler_desc sd = sampler_desc(k);
        de_gpu_buffer b = de_gpu_buffer_create(c->d, &bd);
        de_gpu_texture t = de_gpu_texture_create(c->d, &td);
        de_gpu_sampler s = de_gpu_sampler_create(c->d, &sd);
        c->failures += !DE_HANDLE_VALID(b) + !DE_HANDLE_VALID(t) + !DE_HANDLE_VALID(s);
        de_gpu_buffer_destroy(c->d, b);
        de_gpu_texture_destroy(c->d, t);
        de_gpu_sampler_destroy(c->d, s);
        if (i % 50 == 49) {
            de_gpu_submit sub;
            memset(&sub, 0, sizeof sub);
            c->failures += de_gpu_queue_submit(c->d, (de_gpu_queue)(i % 3), &sub) != DE_OK;
        }
    }
    return 0;
}

TEST_F(vk, threads_create_destroy, vk_fx) {
    enum { T = 4 };
    mt_ctx ctx[T];
    de_thread *threads[T];
    for (u32 i = 0; i < T; ++i) {
        ctx[i] = (mt_ctx){.d = f->d, .seed = i + 1};
        ASSERT_EQ(de_thread_create(mt_main, &ctx[i], "vk-mt", DE_THREAD_NORMAL, 0, &threads[i]),
                  DE_OK);
    }
    for (u32 i = 0; i < T; ++i) {
        de_thread_join(threads[i]);
        EXPECT_EQ(ctx[i].failures, 0u);
    }
    de_gpu_wait_idle(f->d);
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_TEXTURE), 0u);
    EXPECT_EQ(class_used(f->d), (u64)0);
}
