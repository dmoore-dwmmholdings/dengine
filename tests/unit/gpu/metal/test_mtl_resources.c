/* test_mtl_resources.c — Metal buffers, textures, samplers and the bindless argument buffer
 * (DENG-287). Metal API validation is on (test_mtl_device.c sets MTL_DEBUG_LAYER=1 before the
 * first device); a validation error aborts the test.
 */
#include "de_test.h"

#include <dengine/de_gpu.h>

#include "gpu/metal/mtl_internal.h"
#include "mtl_test_helpers.h"

#include <stdio.h>
#include <string.h>

/* Entry `index` of a table, read back from the CPU-visible argument buffer. */
static u64 table_entry(de_gpu_device *d, u32 table_offset, u32 index) {
    const u8 *cpu = NULL;
    de__mtl_bindless_buffer(d, &cpu);
    u64 v;
    memcpy(&v, cpu + table_offset + (size_t)index * DE__MTL_BINDLESS_ENTRY_BYTES, sizeof v);
    return v;
}

/* ---------- 1000 of each ---------- */

#define N_EACH 1000u

static de_gpu_buffer_desc buffer_desc(u32 i) {
    static const u64 k_sizes[] = {256, 1000, 4096, 65536, 12, 300000};
    de_gpu_buffer_desc bd = {.size = k_sizes[i % 6], .name = "b"};
    switch (i % 3) {
    case 0:
        bd.memory = DE_MEM_GPU_ONLY;
        bd.usage = DE_USAGE_STORAGE | DE_USAGE_VERTEX | DE_USAGE_COPY_DST;
        break;
    case 1:
        bd.memory = DE_MEM_UPLOAD;
        bd.usage = DE_USAGE_CONSTANT | DE_USAGE_COPY_SRC;
        break;
    default:
        bd.memory = DE_MEM_READBACK;
        bd.usage = DE_USAGE_COPY_DST;
        break;
    }
    return bd;
}

/* A spread of kinds: mips, arrays, cubes, 3D, depth, storage views, BC and MSAA when the device
 * has them. */
static de_gpu_texture_desc texture_desc(u32 i, const de_test_mtl_info *info) {
    de_gpu_texture_desc t = {.format = DE_FMT_RGBA8_UNORM, .width = 64, .height = 64, .name = "t"};
    switch (i % 10) {
    case 0:
        t.mip_count = 7;
        t.usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST;
        break;
    case 1:
        t.format = DE_FMT_RGBA16_FLOAT;
        t.width = t.height = 32;
        t.mip_count = 3;
        t.usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE;
        break;
    case 2:
        t.format = DE_FMT_D32_FLOAT;
        t.width = t.height = 128;
        t.usage = DE_USAGE_DEPTH | DE_USAGE_SAMPLED;
        break;
    case 3:
        t.dim = DE_TEX_2D_ARRAY;
        t.format = DE_FMT_RGBA8_SRGB;
        t.width = t.height = 16;
        t.depth_or_layers = 4;
        t.mip_count = 2;
        t.usage = DE_USAGE_SAMPLED;
        break;
    case 4:
        t.dim = DE_TEX_CUBE;
        t.format = DE_FMT_RGBA16_FLOAT;
        t.width = t.height = 16;
        t.mip_count = 2;
        t.usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE;
        break;
    case 5:
        t.dim = DE_TEX_3D;
        t.format = DE_FMT_R32_FLOAT;
        t.width = t.height = 8;
        t.depth_or_layers = 8;
        t.mip_count = 2;
        t.usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE;
        break;
    case 6:
        if (info->bc) {
            t.format = DE_FMT_BC7_UNORM;
            t.usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST;
        } else {
            t.usage = DE_USAGE_SAMPLED;
        }
        break;
    case 7:
        t.usage = DE_USAGE_RENDER_TARGET;
        t.sample_count = info->msaa4 ? 4 : 1;
        break;
    case 8:
        t.format = DE_FMT_R11G11B10_FLOAT;
        t.width = t.height = 32;
        t.usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED;
        break;
    default:
        t.format = DE_FMT_D32_FLOAT_S8;
        t.usage = DE_USAGE_DEPTH;
        break;
    }
    return t;
}

static de_gpu_sampler_desc sampler_desc(u32 i) {
    de_gpu_sampler_desc s = {0};
    s.min = (i & 1) ? DE_FILTER_LINEAR : DE_FILTER_NEAREST;
    s.mag = (i & 2) ? DE_FILTER_LINEAR : DE_FILTER_NEAREST;
    s.mip = (i & 4) ? DE_FILTER_LINEAR : DE_FILTER_NEAREST;
    s.u = (de_gpu_address)(i % 4);
    s.v = (de_gpu_address)((i / 4) % 4);
    s.w = (de_gpu_address)((i / 16) % 4);
    s.max_aniso = (f32)(i % 17);
    s.compare_reverse_z = (i % 5) == 0;
    return s;
}

TEST_F(gpu_metal, resources_1000, mtl_fx) {
    de_gpu_device *d = f->d;
    static de_gpu_buffer bufs[N_EACH];
    static de_gpu_texture texs[N_EACH];
    static de_gpu_sampler smps[N_EACH];
    /* 1000 samplers fit the table on Apple GPUs (2048); a device with a smaller limit makes as
     * many as its table holds. */
    const u32 max_smp = de_gpu_get_caps(d)->max_bindless_samplers - 1u;
    const u32 n_smp = max_smp < N_EACH ? max_smp : N_EACH;
    if (n_smp < N_EACH)
        printf("note: %u samplers (the device's argument-buffer sampler limit)\n", n_smp);
    for (u32 round = 0; round < 2; ++round) {
        for (u32 i = 0; i < N_EACH; ++i) {
            de_gpu_buffer_desc bd = buffer_desc(i);
            de_gpu_texture_desc td = texture_desc(i, &f->info);
            de_gpu_sampler_desc sd = sampler_desc(i);
            bufs[i] = de_gpu_buffer_create(d, &bd);
            texs[i] = de_gpu_texture_create(d, &td);
            smps[i] = i < n_smp ? de_gpu_sampler_create(d, &sd) : (de_gpu_sampler){0};
            ASSERT_NE(bufs[i].id, 0u);
            ASSERT_NE(texs[i].id, 0u);
            ASSERT_TRUE(i >= n_smp || smps[i].id != 0u);
            /* Placed in a front-end heap; shared buffers are mapped. */
            ASSERT_TRUE(de__mtl_buffer_placement(d, bufs[i], NULL, NULL) ==
                        (bd.memory == DE_MEM_GPU_ONLY || de__mtl_dev(d)->shared_heaps));
            ASSERT_TRUE(de__mtl_texture_placement(d, texs[i], NULL, NULL));
            if (bd.memory != DE_MEM_GPU_ONLY)
                ASSERT_TRUE(de_gpu_buffer_map(d, bufs[i]) != NULL);
        }
        EXPECT_EQ(de__gpu_live_count(d, DE__GPU_BUFFER), N_EACH);
        EXPECT_EQ(de__gpu_live_count(d, DE__GPU_TEXTURE), N_EACH);
        EXPECT_EQ(de__gpu_live_count(d, DE__GPU_SAMPLER), n_smp);
        EXPECT_EQ(de__gpu_bindless_count(d, DE__GPU_BINDLESS_SAMPLER), n_smp);
        for (u32 i = 0; i < N_EACH; ++i) {
            de_gpu_buffer_destroy(d, bufs[i]);
            de_gpu_texture_destroy(d, texs[i]);
            de_gpu_sampler_destroy(d, smps[i]);
        }
        de_gpu_wait_idle(d);
        EXPECT_EQ(de__gpu_live_count(d, DE__GPU_BUFFER), 0u);
        EXPECT_EQ(de__gpu_live_count(d, DE__GPU_TEXTURE), 0u);
        EXPECT_EQ(de__gpu_live_count(d, DE__GPU_SAMPLER), 0u);
        for (u32 s = 0; s < DE__GPU_BINDLESS_SPACE_COUNT; ++s)
            EXPECT_EQ(de__gpu_bindless_count(d, (de__gpu_bindless_space)s), 0u);
        EXPECT_EQ(de__gpu_mem_kind_used(d, DE_MEM_GPU_ONLY), 0u);
        EXPECT_EQ(de__gpu_mem_kind_used(d, DE_MEM_UPLOAD), 0u);
        EXPECT_EQ(de__gpu_mem_kind_used(d, DE_MEM_READBACK), 0u);
    }
}

TEST_F(gpu_metal, unsupported_texture, mtl_fx) {
    if (f->info.msaa4 && f->info.bc) {
        /* What this device lacks fails cleanly, and nothing leaks. */
        de_gpu_texture t =
            de_gpu_texture_create(f->d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                                               .width = 64,
                                                               .height = 64,
                                                               .sample_count = 8,
                                                               .usage = DE_USAGE_RENDER_TARGET});
        /* 8x MSAA exists on recent Apple GPUs: either outcome is fine, a crash is not. */
        if (t.id)
            de_gpu_texture_destroy(f->d, t);
        else
            EXPECT_EQ(de_error_last()->result, DE_ERR_UNSUPPORTED);
    }
    de_gpu_wait_idle(f->d);
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_TEXTURE), 0u);
    EXPECT_EQ(de__gpu_mem_kind_used(f->d, DE_MEM_GPU_ONLY), 0u);
}

/* ---------- The argument buffer ---------- */

TEST_F(gpu_metal, bindless_entries, mtl_fx) {
    de_gpu_device *d = f->d;
    const mtl_device *m = de__mtl_dev(d);
    const u8 *cpu = NULL;
    ASSERT_TRUE(de__mtl_bindless_buffer(d, &cpu) != NULL);
    ASSERT_TRUE(cpu != NULL);

    /* Slot 0 of every table is the null resource, and so is every slot never written. */
    const u64 null_tex = table_entry(d, DE__MTL_BINDLESS_OFFSET_TEXTURES, 0);
    const u64 null_rw = table_entry(d, DE__MTL_BINDLESS_OFFSET_STORAGE_TEXTURES, 0);
    const u64 null_smp = table_entry(d, DE__MTL_BINDLESS_OFFSET_SAMPLERS, 0);
    const u64 null_buf = table_entry(d, DE__MTL_BINDLESS_OFFSET_BUFFERS, 0);
    EXPECT_EQ(null_tex, de_test_mtl_texture_id(m->null_texture));
    EXPECT_EQ(null_rw, de_test_mtl_texture_id(m->null_storage_texture));
    EXPECT_EQ(null_smp, de_test_mtl_sampler_id(m->null_sampler));
    EXPECT_EQ(null_buf, de_test_mtl_buffer_address(m->null_buffer));
    EXPECT_NE(null_buf, 0u);
    EXPECT_EQ(table_entry(d, DE__MTL_BINDLESS_OFFSET_TEXTURES, DE__MTL_BINDLESS_TEXTURES - 1),
              null_tex);
    EXPECT_EQ(table_entry(d, DE__MTL_BINDLESS_OFFSET_STORAGE_TEXTURES,
                          DE__MTL_BINDLESS_STORAGE_TEXTURES - 1),
              null_rw);
    EXPECT_EQ(table_entry(d, DE__MTL_BINDLESS_OFFSET_SAMPLERS, DE__MTL_BINDLESS_SAMPLERS - 1),
              null_smp);
    EXPECT_EQ(table_entry(d, DE__MTL_BINDLESS_OFFSET_BUFFERS, DE__MTL_BINDLESS_BUFFERS - 1),
              null_buf);

    enum { N = 40 };
    de_gpu_buffer bufs[N];
    de_gpu_texture texs[N];
    de_gpu_sampler smps[N];
    for (u32 i = 0; i < N; ++i) {
        de_gpu_buffer_desc bd = buffer_desc(i);
        if (bd.memory == DE_MEM_READBACK) /* no bindless usage on READBACK: use UPLOAD */
            bd = (de_gpu_buffer_desc){
                .size = 512, .usage = DE_USAGE_CONSTANT, .memory = DE_MEM_UPLOAD, .name = "c"};
        de_gpu_texture_desc td = texture_desc(i, &f->info);
        td.usage |=
            de__gpu_format_is_depth(td.format) || td.sample_count > 1 ? 0 : DE_USAGE_SAMPLED;
        if (td.sample_count > 1 || (td.usage & DE_USAGE_DEPTH))
            td = (de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                       .width = 32,
                                       .height = 32,
                                       .mip_count = 6,
                                       .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE};
        de_gpu_sampler_desc sd = sampler_desc(i);
        bufs[i] = de_gpu_buffer_create(d, &bd);
        texs[i] = de_gpu_texture_create(d, &td);
        smps[i] = de_gpu_sampler_create(d, &sd);
        ASSERT_NE(bufs[i].id, 0u);
        ASSERT_NE(texs[i].id, 0u);
        ASSERT_NE(smps[i].id, 0u);
    }

    /* Every entry at an allocator index equals the resource's gpuAddress / gpuResourceID. */
    u32 storage_views = 0;
    for (u32 i = 0; i < N; ++i) {
        const u32 bi = de_gpu_buffer_bindless(d, bufs[i]);
        ASSERT_NE(bi, DE_GPU_BINDLESS_INVALID);
        EXPECT_EQ(table_entry(d, DE__MTL_BINDLESS_OFFSET_BUFFERS, bi),
                  de_test_mtl_buffer_address(de__mtl_buffer_object(d, bufs[i])));

        const u32 si = de_gpu_sampler_bindless(d, smps[i]);
        ASSERT_NE(si, DE_GPU_BINDLESS_INVALID);
        EXPECT_EQ(table_entry(d, DE__MTL_BINDLESS_OFFSET_SAMPLERS, si),
                  de_test_mtl_sampler_id(de__mtl_sampler_object(d, smps[i])));

        de__gpu_texture *t = de__gpu_get_texture(d, texs[i]);
        ASSERT_TRUE(t != NULL);
        const u32 ti = de_gpu_texture_bindless(d, texs[i], 0, false);
        ASSERT_NE(ti, DE_GPU_BINDLESS_INVALID);
        EXPECT_EQ(table_entry(d, DE__MTL_BINDLESS_OFFSET_TEXTURES, ti),
                  de_test_mtl_texture_id(de__mtl_texture_object(d, texs[i], -1)));
        if (t->usage & DE_USAGE_STORAGE) {
            for (u32 mip = 0; mip < t->mip_count; ++mip) {
                const u32 wi = de_gpu_texture_bindless(d, texs[i], mip, true);
                ASSERT_NE(wi, DE_GPU_BINDLESS_INVALID);
                void *view = de__mtl_texture_object(d, texs[i], (i32)mip);
                ASSERT_TRUE(view != NULL);
                EXPECT_EQ(table_entry(d, DE__MTL_BINDLESS_OFFSET_STORAGE_TEXTURES, wi),
                          de_test_mtl_texture_id(view));
                /* One mip, all layers; a cube is viewed as a 2D array. */
                u32 levels = 0, layers = 0;
                const u32 type = de_test_mtl_texture_type(view, &levels, &layers);
                EXPECT_EQ(levels, 1u);
                if (t->dim == DE_TEX_CUBE) {
                    EXPECT_EQ(type, de_test_mtl_type_2d_array());
                    EXPECT_EQ(layers, 6u);
                } else if (t->dim == DE_TEX_2D) {
                    EXPECT_EQ(type, de_test_mtl_type_2d());
                }
                ++storage_views;
            }
        }
    }
    EXPECT_GT(storage_views, 0u);

    /* Destroyed resources' slots read the null resource again once the GPU is done. */
    const u32 bi = de_gpu_buffer_bindless(d, bufs[0]);
    const u32 ti = de_gpu_texture_bindless(d, texs[0], 0, false);
    const u32 si = de_gpu_sampler_bindless(d, smps[0]);
    const u32 wi = de_gpu_texture_bindless(d, texs[1], 0, true);
    for (u32 i = 0; i < N; ++i) {
        de_gpu_buffer_destroy(d, bufs[i]);
        de_gpu_texture_destroy(d, texs[i]);
        de_gpu_sampler_destroy(d, smps[i]);
    }
    de_gpu_wait_idle(d);
    EXPECT_EQ(table_entry(d, DE__MTL_BINDLESS_OFFSET_BUFFERS, bi), null_buf);
    EXPECT_EQ(table_entry(d, DE__MTL_BINDLESS_OFFSET_TEXTURES, ti), null_tex);
    EXPECT_EQ(table_entry(d, DE__MTL_BINDLESS_OFFSET_SAMPLERS, si), null_smp);
    EXPECT_EQ(table_entry(d, DE__MTL_BINDLESS_OFFSET_STORAGE_TEXTURES, wi), null_rw);
}

/* The GPU reads the table with the layout Slang emits for bindless.slang. */
TEST_F(gpu_metal, bindless_gpu_read, mtl_fx) {
    mtl_require_base_tier(f);
    if (!f->info.rw_tier2)
        SKIP("the device lacks read-write texture tier 2 (RGBA8 storage textures)");
    de_gpu_device *d = f->d;
    char err[512] = "";

    const de_gpu_texture_desc td = {.format = DE_FMT_RGBA8_UNORM,
                                    .width = 1,
                                    .height = 1,
                                    .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                    .name = "sampled"};
    const de_gpu_texture_desc rd = {.format = DE_FMT_RGBA8_UNORM,
                                    .width = 1,
                                    .height = 1,
                                    .usage =
                                        DE_USAGE_STORAGE | DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                                    .name = "storage"};
    de_gpu_texture tex = de_gpu_texture_create(d, &td);
    de_gpu_texture rw = de_gpu_texture_create(d, &rd);
    de_gpu_sampler smp = de_gpu_sampler_create(d, &(de_gpu_sampler_desc){0});
    de_gpu_buffer in = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){
               .size = 256, .usage = DE_USAGE_CONSTANT, .memory = DE_MEM_UPLOAD, .name = "in"});
    de_gpu_buffer out = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){
               .size = 256, .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC, .name = "out"});
    de_gpu_buffer rb = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){
               .size = 256, .usage = DE_USAGE_COPY_DST, .memory = DE_MEM_READBACK, .name = "rb"});
    ASSERT_TRUE(tex.id && rw.id && smp.id && in.id && out.id && rb.id);

    const u8 texel[4] = {255, 128, 0, 255};
    const u8 rw_texel[4] = {0, 64, 255, 128};
    ASSERT_TRUE(de_test_mtl_upload_rgba8(d, tex, texel, err, sizeof err) == 1);
    ASSERT_TRUE(de_test_mtl_upload_rgba8(d, rw, rw_texel, err, sizeof err) == 1);
    const f32 in_values[4] = {1.5f, -2.0f, 3.25f, 42.0f};
    memcpy(de_gpu_buffer_map(d, in), in_values, sizeof in_values);

    const u32 push[5] = {
        de_gpu_texture_bindless(d, tex, 0, false),
        de_gpu_texture_bindless(d, rw, 0, true),
        de_gpu_sampler_bindless(d, smp),
        de_gpu_buffer_bindless(d, in),
        de_gpu_buffer_bindless(d, out),
    };
    u8 rw_after[4] = {0};
    const int ok =
        de_test_mtl_bindless_kernel(d, push, out, rb, 6 * 16, rw, rw_after, err, sizeof err);
    if (!ok)
        FAIL(err);
    const f32 *r = (const f32 *)de_gpu_buffer_map(d, rb);
    ASSERT_TRUE(r != NULL);
    const f32 eps = 1.0f / 255.0f;
    for (u32 c = 0; c < 4; ++c) {
        EXPECT_FLOAT_EQ(r[0 * 4 + c], texel[c] / 255.0f, eps);    /* sampled texture */
        EXPECT_FLOAT_EQ(r[1 * 4 + c], rw_texel[c] / 255.0f, eps); /* storage texture */
        EXPECT_FLOAT_EQ(r[2 * 4 + c], texel[c] / 255.0f, eps);    /* through the sampler */
        EXPECT_FLOAT_EQ(r[3 * 4 + c], in_values[c], 0.0f);        /* buffer by address */
        EXPECT_FLOAT_EQ(r[4 * 4 + c], 0.0f, 0.0f);                /* null texture: zero */
        EXPECT_FLOAT_EQ(r[5 * 4 + c], 0.0f, 0.0f);                /* null buffer: zero */
    }
    const u8 written[4] = {64, 128, 191, 255}; /* (0.25, 0.5, 0.75, 1) as UNORM8 */
    for (u32 c = 0; c < 4; ++c)
        EXPECT_LE(rw_after[c] > written[c] ? rw_after[c] - written[c] : written[c] - rw_after[c],
                  1);

    de_gpu_texture_destroy(d, tex);
    de_gpu_texture_destroy(d, rw);
    de_gpu_sampler_destroy(d, smp);
    de_gpu_buffer_destroy(d, in);
    de_gpu_buffer_destroy(d, out);
    de_gpu_buffer_destroy(d, rb);
}

/* ---------- Threads ---------- */

typedef struct mt_ctx {
    de_gpu_device *d;
    const de_test_mtl_info *info;
    u32 seed;
    u32 failures;
    bool samplers; /* the sampler table holds what 8 threads keep alive until the GPU is done */
} mt_ctx;

/* Creates, checks and destroys resources (and submits) while other threads do the same: the
 * heap list, the heap classes and the table writes are shared. */
static i32 mt_main(void *user) {
    mt_ctx *c = (mt_ctx *)user;
    for (u32 i = 0; i < 200; ++i) {
        const u32 k = c->seed + i;
        de_gpu_buffer_desc bd = buffer_desc(k);
        if (bd.memory == DE_MEM_READBACK)
            bd.usage = DE_USAGE_COPY_DST;
        de_gpu_texture_desc td = texture_desc(k, c->info);
        de_gpu_sampler_desc sd = sampler_desc(k);
        de_gpu_buffer b = de_gpu_buffer_create(c->d, &bd);
        de_gpu_texture t = de_gpu_texture_create(c->d, &td);
        de_gpu_sampler s = c->samplers ? de_gpu_sampler_create(c->d, &sd) : (de_gpu_sampler){0};
        if (!b.id || !t.id || (c->samplers && !s.id))
            ++c->failures;
        if (s.id) {
            const u32 si = de_gpu_sampler_bindless(c->d, s);
            if (table_entry(c->d, DE__MTL_BINDLESS_OFFSET_SAMPLERS, si) !=
                de_test_mtl_sampler_id(de__mtl_sampler_object(c->d, s)))
                ++c->failures;
        }
        if (b.id && bd.memory == DE_MEM_GPU_ONLY) {
            const u32 bi = de_gpu_buffer_bindless(c->d, b);
            if (table_entry(c->d, DE__MTL_BINDLESS_OFFSET_BUFFERS, bi) !=
                de_test_mtl_buffer_address(de__mtl_buffer_object(c->d, b)))
                ++c->failures;
        }
        if ((i % 16) == 0) {
            const de_gpu_queue q = (de_gpu_queue)(k % DE_QUEUE_COUNT_);
            de_gpu_cmd *cmd = de_gpu_cmd_begin(c->d, q, "mt");
            de_gpu_submit sub = {.cmds = &cmd, .cmd_count = 1};
            if (!cmd || de_gpu_queue_submit(c->d, q, &sub) != DE_OK)
                ++c->failures;
        }
        de_gpu_buffer_destroy(c->d, b);
        de_gpu_texture_destroy(c->d, t);
        de_gpu_sampler_destroy(c->d, s);
    }
    return 0;
}

TEST_F(gpu_metal, threads_create_destroy, mtl_fx) {
    enum { T = 8 };
    de_thread *threads[T];
    mt_ctx ctx[T];
    for (u32 i = 0; i < T; ++i) {
        /* Destroyed samplers wait for the next submit to free their slots; a device with a
         * small sampler table (a paravirtual one: 96) leaves them out. */
        ctx[i] = (mt_ctx){.d = f->d,
                          .info = &f->info,
                          .seed = 0x9E3779B9u * (i + 1u),
                          .samplers = de_gpu_get_caps(f->d)->max_bindless_samplers >= 1024u};
        ASSERT_EQ(
            de_thread_create(mt_main, &ctx[i], "gpu-metal-mt", DE_THREAD_NORMAL, 0, &threads[i]),
            DE_OK);
    }
    for (u32 i = 0; i < T; ++i) {
        de_thread_join(threads[i]);
        EXPECT_EQ(ctx[i].failures, 0u);
    }
    de_gpu_wait_idle(f->d);
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_BUFFER), 0u);
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_TEXTURE), 0u);
    EXPECT_EQ(de__gpu_deferred_count(f->d), 0u);
    EXPECT_EQ(de__gpu_mem_kind_used(f->d, DE_MEM_GPU_ONLY), 0u);
}
