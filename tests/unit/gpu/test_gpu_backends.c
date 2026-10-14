/* test_gpu_backends.c — the DENG-410 RHI additions, run the same way on every backend in the build.
 * Executable test_gpu_backends, CTest names gpu_backends.backends.<test>.
 *
 * Front-end (null backend, every host):
 *   - adapters_args: de_gpu_enumerate_adapters argument checks, DE_ERR_UNSUPPORTED for a backend
 *     that is not in the build, cap 0 counting only.
 *   - adapters_null: the null backend (no adapter list of its own) reports its one device.
 *   - cmd_reserve_null: de_gpu_cmd_reserve fills a queue's pool, so de_gpu_cmd_begin of that many
 *     lists allocates nothing; reserving fewer than the device owns does nothing.
 *   - d3d12_srv_offset: bindless.slang's DE__D3D12_BUFFER_SRV_OFFSET (where D3D12 shaders find a
 *     buffer's read-only view) is the D3D12 backend's, and the heap ends with that range.
 * Backends (Metal, Vulkan, D3D12; each skips without a device, the shader importer or the feature):
 *   - adapters_<backend>: the adapter list equals what a device created on each index reports in
 *     de_gpu_caps (name, ids, memory), in de_gpu_device_desc.adapter order; a short `cap` gets
 *     the total and only `cap` entries.
 *   - cmd_reserve_<backend>: as cmd_reserve_null with the backend's command lists, then the lists
 *     run (empty) on the compute queue.
 *   - bindless_access_<backend>: data/pipelines/bindless_access.slang reads and writes buffers
 *     through de_robuffer, de_buffer, de_bytes and de_rwbytes (bindless.slang, DENG-410): UPLOAD
 *     and GPU_ONLY buffers read-only, raw buffers with every load and store width.
 * Vulkan runs on the best Base-tier device or adapter 1 forced (MoltenVK); D3D12 on the best
 * adapter (CI: the vendored WARP) with the debug layer; Metal with API validation
 * (MTL_DEBUG_LAYER=1). The device tests require zero validation errors.
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* fopen */
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv */
#endif
#include "de_test.h"

#include <dengine/de_gpu.h>

#include "gpu/gpu_backend.h"
#include "pipeline_shaders.h"

#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_agility_exports.h"
#endif
#include "gpu/d3d12/d3d12_internal.h" /* the portable part: bindless table sizes */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS ((u64)30 * 1000 * 1000 * 1000)

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

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static const char *type_name(de_gpu_adapter_type t) {
    switch (t) {
    case DE_GPU_ADAPTER_DISCRETE:
        return "discrete";
    case DE_GPU_ADAPTER_INTEGRATED:
        return "integrated";
    case DE_GPU_ADAPTER_VIRTUAL:
        return "virtual";
    case DE_GPU_ADAPTER_SOFTWARE:
        return "software";
    default:
        return "unknown";
    }
}

/* ---------- Fixture ---------- */

typedef struct b_fx {
    de_tracking_allocator tracker;
    de_allocator alloc;
    de_gpu_device *d;
    de_gpu_backend backend;
    char skip[1400];
    char err[1100];
} b_fx;

static void b_fx_setup(b_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
}

/* Validation errors of the device's layer (0 when it has none). */
static u32 validation_errors(de_gpu_device *d) {
    switch (de_gpu_get_caps(d)->backend) {
#if DE_HAS_GPU_VULKAN
    case DE_GPU_VULKAN: {
        u32 warnings = 0;
        return de__gpu_vk_validation_messages(d, &warnings);
    }
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
    case DE_GPU_D3D12:
        return de__d3d12_debug_error_count(d);
#endif
    default:
        return 0;
    }
}

static void b_fx_teardown(b_fx *f) {
    if (f->d) {
        de_gpu_wait_idle(f->d);
        EXPECT_EQ(validation_errors(f->d), 0u);
        de_gpu_device_destroy(f->d);
        f->d = NULL;
    }
    de_tracking_stats st = de_tracking_allocator_stats(&f->tracker);
    EXPECT_EQ(st.blocks_live, 0u);
    EXPECT_EQ(st.bytes_live, 0u);
}

static de__shader_target target_of(de_gpu_backend b) {
    switch (b) {
    case DE_GPU_VULKAN:
        return DE__SHADER_TARGET_SPIRV;
    case DE_GPU_D3D12:
        return DE__SHADER_TARGET_DXIL;
    case DE_GPU_METAL:
        return DE__SHADER_TARGET_METALLIB;
    default:
        return DE__SHADER_TARGET_NONE;
    }
}

/* A device of backend `b` with its debug layer, or SKIP. Vulkan falls back to adapter 1 (MoltenVK
 * is not Base-tier capable). */
static void open_backend(b_fx *f, de_gpu_backend b) {
    if (b == DE_GPU_METAL)
        set_env("MTL_DEBUG_LAYER", "1"); /* before the first Metal device of the process */
    de_gpu_device_desc desc = {.backend = b,
                               .debug_layer = true,
                               .gpu_validation = true, /* Vulkan: synchronization validation */
                               .allocator = &f->alloc,
                               .pipeline_cache_dir = ""};
    de_result r = de_gpu_device_create(&desc, &f->d);
    if (r == DE_ERR_UNSUPPORTED && b == DE_GPU_VULKAN) {
        desc.adapter = 1;
        r = de_gpu_device_create(&desc, &f->d);
    }
    if (r != DE_OK) {
        snprintf(f->skip, sizeof f->skip, "no usable %s device: %s", de_gpu_backend_name(b),
                 errchain(f->err, sizeof f->err));
        f->d = NULL;
        SKIP(f->skip);
    }
    f->backend = b;
    printf("device: \"%s\" (%s)\n", de_gpu_get_caps(f->d)->device_name, de_gpu_backend_name(b));
}

/* ---------- Adapters ---------- */

TEST(backends, adapters_args) {
    de_gpu_adapter_info info[2];
    u32 n = 77;
    EXPECT_EQ(de_gpu_enumerate_adapters(DE_GPU_NULL, info, 2, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_gpu_enumerate_adapters(DE_GPU_NULL, NULL, 2, &n), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(de_gpu_enumerate_adapters((de_gpu_backend)99, info, 2, &n), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_gpu_enumerate_adapters(DE_GPU_BACKEND_COUNT_, info, 2, &n),
              DE_ERR_INVALID_ARGUMENT);
    /* cap 0: the count only. */
    n = 0;
    EXPECT_EQ(de_gpu_enumerate_adapters(DE_GPU_NULL, NULL, 0, &n), DE_OK);
    EXPECT_EQ(n, 1u);
    /* A backend that is not in this build. */
    for (u32 b = DE_GPU_D3D12; b < DE_GPU_NULL; ++b) {
        if (de__gpu_find_backend((de_gpu_backend)b))
            continue;
        n = 5;
        de_error_clear();
        EXPECT_EQ(de_gpu_enumerate_adapters((de_gpu_backend)b, info, 2, &n), DE_ERR_UNSUPPORTED);
        EXPECT_EQ(n, 0u);
        EXPECT_TRUE(de_error_last() &&
                    strstr(de_error_last()->message, "not compiled into this build") != NULL);
    }
}

TEST(backends, adapters_null) {
    de_gpu_adapter_info info[3];
    memset(info, 0xAB, sizeof info);
    u32 n = 0;
    ASSERT_EQ(de_gpu_enumerate_adapters(DE_GPU_NULL, info, 3, &n), DE_OK);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(info[0].index, 1u);
    EXPECT_STREQ(info[0].name, "null");
    EXPECT_EQ((u32)info[0].type, (u32)DE_GPU_ADAPTER_UNKNOWN);
    /* Entries past the total are zeroed. */
    de_gpu_adapter_info zero;
    memset(&zero, 0, sizeof zero);
    EXPECT_MEMEQ(&info[1], &zero, sizeof zero);
}

/* The list against the caps of a device made on each index. */
static void check_adapters(de_gpu_backend b) {
    de_gpu_adapter_info list[16];
    u32 n = 0;
    char err[1100], skip[1300];
    de_error_clear();
    de_result r = de_gpu_enumerate_adapters(b, list, 16, &n);
    if (r == DE_ERR_UNSUPPORTED || r == DE_ERR_NOT_FOUND) {
        snprintf(skip, sizeof skip, "%s: %s", de_gpu_backend_name(b), errchain(err, sizeof err));
        SKIP(skip);
    }
    ASSERT_EQ(r, DE_OK);
    if (n == 0) {
        snprintf(skip, sizeof skip, "%s lists no adapter", de_gpu_backend_name(b));
        SKIP(skip);
    }

    /* A short cap: the total, one entry, the rest untouched. */
    de_gpu_adapter_info one[2];
    memset(one, 0xCD, sizeof one);
    u32 n1 = 0;
    ASSERT_EQ(de_gpu_enumerate_adapters(b, one, 1, &n1), DE_OK);
    EXPECT_EQ(n1, n);
    EXPECT_EQ(one[0].index, 1u);
    EXPECT_STREQ(one[0].name, list[0].name);
    u8 cd[sizeof one[1]];
    memset(cd, 0xCD, sizeof cd);
    EXPECT_MEMEQ(&one[1], cd, sizeof cd);

    u32 compared = 0;
    for (u32 i = 0; i < n && i < 16; ++i) {
        const de_gpu_adapter_info *a = &list[i];
        EXPECT_EQ(a->index, i + 1u);
        printf("adapter %u: \"%s\" %04x/%08x %s, %llu MiB dedicated, %llu MiB shared\n", a->index,
               a->name, a->vendor_id, a->device_id, type_name(a->type),
               (unsigned long long)(a->dedicated_vram >> 20),
               (unsigned long long)(a->shared_memory >> 20));
        de_gpu_device *d = NULL;
        de_gpu_device_desc desc = {.backend = b, .adapter = a->index, .pipeline_cache_dir = ""};
        de_error_clear();
        if (de_gpu_device_create(&desc, &d) != DE_OK) {
            printf("  no device on it: %s\n", errchain(err, sizeof err));
            continue;
        }
        const de_gpu_caps *c = de_gpu_get_caps(d);
        EXPECT_STREQ(a->name, c->device_name);
        EXPECT_EQ(a->vendor_id, c->vendor_id);
        EXPECT_EQ(a->device_id, c->device_id);
        EXPECT_EQ(a->dedicated_vram, c->dedicated_vram);
        EXPECT_EQ(a->shared_memory, c->shared_memory);
        if (b == DE_GPU_METAL && c->unified_memory)
            EXPECT_EQ((u32)a->type, (u32)DE_GPU_ADAPTER_INTEGRATED);
        de_gpu_device_destroy(d);
        ++compared;
    }
    if (compared == 0) {
        snprintf(skip, sizeof skip, "%s: no adapter creates a device", de_gpu_backend_name(b));
        SKIP(skip);
    }
}

TEST(backends, adapters_metal) {
    check_adapters(DE_GPU_METAL);
}
TEST(backends, adapters_vulkan) {
    check_adapters(DE_GPU_VULKAN);
}
TEST(backends, adapters_d3d12) {
    check_adapters(DE_GPU_D3D12);
}

/* ---------- D3D12 read-only views (portable) ---------- */

TEST(backends, d3d12_srv_offset) {
    FILE *fp = fopen(DE_BINDLESS_SLANG_PATH, "rb");
    ASSERT_TRUE(fp != NULL);
    static char text[65536];
    const size_t n = fread(text, 1, sizeof text - 1, fp);
    fclose(fp);
    text[n] = '\0';
    const char *key = "static const uint DE__D3D12_BUFFER_SRV_OFFSET = ";
    const char *at = strstr(text, key);
    ASSERT_TRUE(at != NULL);
    const unsigned long shader_offset = strtoul(at + strlen(key), NULL, 10);
    EXPECT_EQ((u64)shader_offset, (u64)DE__D3D12_BUFFER_SRV_OFFSET);
    /* The read-only range follows every numbered range and ends the heap. */
    const u32 buffer0 = DE__D3D12_BINDLESS_TEXTURES + DE__D3D12_BINDLESS_STORAGE_TEXTURES;
    EXPECT_EQ(buffer0 + DE__D3D12_BUFFER_SRV_OFFSET,
              buffer0 + DE__D3D12_BINDLESS_BUFFERS + DE__D3D12_BINDLESS_ACCELS);
    EXPECT_EQ(buffer0 + DE__D3D12_BUFFER_SRV_OFFSET + DE__D3D12_BINDLESS_BUFFERS,
              DE__D3D12_RESOURCE_HEAP_SIZE);
    EXPECT_LE(DE__D3D12_RESOURCE_HEAP_SIZE, 1000000u);
}

/* ---------- Command-list reserve ---------- */

/* Reserves `n` lists of `q`, then begins n: no allocation. Submits them on `q` and waits. */
static void reserve_and_use(b_fx *f, de_gpu_queue q, u32 n) {
    enum { MAX = 8 };
    ASSERT_LE(n, (u32)MAX);
    ASSERT_EQ(de_gpu_cmd_reserve(f->d, q, n), DE_OK);
    ASSERT_EQ(de_gpu_cmd_reserve(f->d, q, n / 2u), DE_OK); /* fewer than it owns: nothing */
    const u64 allocs = de_tracking_allocator_stats(&f->tracker).alloc_count;
    de_gpu_cmd *cmds[MAX];
    for (u32 i = 0; i < n; ++i) {
        cmds[i] = de_gpu_cmd_begin(f->d, q, "reserved");
        ASSERT_TRUE(cmds[i] != NULL);
    }
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracker).alloc_count, allocs);
    de_gpu_fence fence = de_gpu_fence_create(f->d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(fence));
    de_gpu_submit s = {.cmds = cmds, .cmd_count = n, .signal = fence, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(f->d, q, &s), DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(f->d, fence, 1, WAIT_NS), DE_OK);
    de_gpu_fence_destroy(f->d, fence);
}

TEST_F(backends, cmd_reserve_null, b_fx) {
    de_gpu_device_desc desc = {.backend = DE_GPU_NULL, .allocator = &f->alloc};
    ASSERT_EQ(de_gpu_device_create(&desc, &f->d), DE_OK);
    EXPECT_EQ(de_gpu_cmd_reserve(NULL, DE_QUEUE_GRAPHICS, 1), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_gpu_cmd_reserve(f->d, DE_QUEUE_COUNT_, 1), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_gpu_cmd_reserve(f->d, DE_QUEUE_GRAPHICS, 0), DE_OK);
    reserve_and_use(f, DE_QUEUE_GRAPHICS, 8);
    /* Another queue has its own pool. */
    reserve_and_use(f, DE_QUEUE_COPY, 3);
    /* After the GPU is done the lists are free again: beginning 8 still allocates nothing. */
    de_gpu_wait_idle(f->d);
    reserve_and_use(f, DE_QUEUE_GRAPHICS, 8);
}

static void cmd_reserve_backend(b_fx *f, de_gpu_backend b) {
    open_backend(f, b);
    reserve_and_use(f, DE_QUEUE_COMPUTE, 4);
    de_gpu_wait_idle(f->d);
    reserve_and_use(f, DE_QUEUE_COMPUTE, 4);
}

TEST_F(backends, cmd_reserve_metal, b_fx) {
    cmd_reserve_backend(f, DE_GPU_METAL);
}
TEST_F(backends, cmd_reserve_vulkan, b_fx) {
    cmd_reserve_backend(f, DE_GPU_VULKAN);
}
TEST_F(backends, cmd_reserve_d3d12, b_fx) {
    cmd_reserve_backend(f, DE_GPU_D3D12);
}

/* ---------- Bindless buffer accessors ---------- */

static de_gpu_buffer upload(de_gpu_device *d, const void *data, u64 size, u32 stride,
                            const char *name) {
    de_gpu_buffer b =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = size,
                                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                      .memory = DE_MEM_UPLOAD,
                                                      .stride = stride,
                                                      .name = name});
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    void *p = de_gpu_buffer_map(d, b);
    ASSERT_TRUE(p != NULL);
    memcpy(p, data, (size_t)size);
    return b;
}

static de_gpu_buffer gpu_only(de_gpu_device *d, u64 size, u32 stride, const char *name) {
    de_gpu_buffer b = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = size,
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                                 .stride = stride,
                                 .name = name});
    ASSERT_TRUE(DE_HANDLE_VALID(b));
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

static void bindless_access(b_fx *f, de_gpu_backend b) {
    char why[300];
    if (!de_pipe_test_cook(target_of(b), why, sizeof why)) {
        snprintf(f->skip, sizeof f->skip, "shared pipeline shaders not cooked: %s", why);
        SKIP(f->skip);
    }
    open_backend(f, b);
    /* As the Metal pipeline tests: below the Base tier (CI's paravirtual device has no argument
     * buffers tier 2) the bindless table does not reach the shader. */
    if (b == DE_GPU_METAL && !de_gpu_get_caps(f->d)->base_tier_ok) {
        snprintf(f->skip, sizeof f->skip, "the Metal device is below the Base tier: %s",
                 de_gpu_get_caps(f->d)->base_tier_reason);
        SKIP(f->skip);
    }
#if defined(_WIN32) && DE_HAS_GPU_D3D12
    if (b == DE_GPU_D3D12) {
        const char *no = NULL;
        if (!de__d3d12_pipelines_supported(f->d, &no)) {
            snprintf(f->skip, sizeof f->skip, "this D3D12 device has no pipelines (%s)", no);
            SKIP(f->skip);
        }
    }
#endif
    de_pipe_test_shader cs;
    if (!de_pipe_test_get(target_of(b), "bindless_access.slang", "cs_main", &cs)) {
        printf("bindless_access.slang:cs_main was not cooked\n");
        ASSERT_TRUE(false);
    }
    de_gpu_pipeline_desc pd = {
        .kind = DE_PIPE_COMPUTE,
        .cs = cs.blob,
        .push_constant_bytes = cs.push_constant_bytes,
        .threadgroup_size = {cs.threadgroup[0], cs.threadgroup[1], cs.threadgroup[2]},
        .name = "bindless access"};
    de_gpu_pipeline pipe = de_gpu_pipeline_create(f->d, &pd);
    if (!DE_HANDLE_VALID(pipe))
        printf("pipeline: %s\n", errchain(f->err, sizeof f->err));
    ASSERT_TRUE(DE_HANDLE_VALID(pipe));
    EXPECT_EQ(cs.push_constant_bytes, 24u);

    enum { N = DE_PIPE_TEST_ACCESS_COUNT };
    static u32 ro_upload[N], ro_gpu[N], raw_src[N], raw_old[2 * N];
    for (u32 i = 0; i < N; ++i) {
        ro_upload[i] = DE_PIPE_TEST_ACCESS_RO_UPLOAD(i);
        ro_gpu[i] = DE_PIPE_TEST_ACCESS_RO_GPU(i);
        raw_src[i] = DE_PIPE_TEST_ACCESS_RAW_SRC(i);
    }
    for (u32 w = 0; w < 2 * N; ++w)
        raw_old[w] = DE_PIPE_TEST_ACCESS_RAW_OLD(w);

    de_gpu_buffer b_ro_upload = upload(f->d, ro_upload, sizeof ro_upload, 4, "ro upload");
    de_gpu_buffer b_raw_upload = upload(f->d, raw_src, sizeof raw_src, 0, "raw upload");
    de_gpu_buffer stage_gpu = upload(f->d, ro_gpu, sizeof ro_gpu, 4, "ro gpu staging");
    de_gpu_buffer stage_raw = upload(f->d, raw_old, sizeof raw_old, 0, "raw rw staging");
    de_gpu_buffer b_ro_gpu = gpu_only(f->d, sizeof ro_gpu, 4, "ro gpu");
    de_gpu_buffer b_raw_rw = gpu_only(f->d, sizeof raw_old, 0, "raw rw");
    de_gpu_buffer b_dst = gpu_only(f->d, sizeof ro_upload, 4, "dst");
    de_gpu_buffer rb =
        de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = sizeof ro_upload + sizeof raw_old,
                                                         .usage = DE_USAGE_COPY_DST,
                                                         .memory = DE_MEM_READBACK,
                                                         .name = "readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(rb));
    u32 *res = (u32 *)de_gpu_buffer_map(f->d, rb);
    ASSERT_TRUE(res != NULL);
    memset(res, 0, sizeof ro_upload + sizeof raw_old);

    de_error_clear();
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_COMPUTE, "bindless access");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier bars[3] = {
        buf_barrier(b_ro_gpu, DE_ACCESS_NONE, DE_ACCESS_COPY_DST),
        buf_barrier(b_raw_rw, DE_ACCESS_NONE, DE_ACCESS_COPY_DST),
        buf_barrier(b_dst, DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE),
    };
    de_gpu_cmd_barriers(c, bars, 3);
    de_gpu_cmd_copy_buffer(c, b_ro_gpu, 0, stage_gpu, 0, sizeof ro_gpu);
    de_gpu_cmd_copy_buffer(c, b_raw_rw, 0, stage_raw, 0, sizeof raw_old);
    bars[0] = buf_barrier(b_ro_gpu, DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_READ);
    bars[1] = buf_barrier(b_raw_rw, DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_WRITE);
    de_gpu_cmd_barriers(c, bars, 2);
    de_gpu_cmd_set_pipeline(c, pipe);
    const u32 params[6] = {
        de_gpu_buffer_bindless(f->d, b_ro_upload),  de_gpu_buffer_bindless(f->d, b_ro_gpu),
        de_gpu_buffer_bindless(f->d, b_raw_upload), de_gpu_buffer_bindless(f->d, b_raw_rw),
        de_gpu_buffer_bindless(f->d, b_dst),        N};
    for (u32 i = 0; i < 5; ++i)
        EXPECT_NE(params[i], DE_GPU_BINDLESS_INVALID);
    de_gpu_cmd_push_constants(c, params, sizeof params);
    de_gpu_cmd_dispatch(c, (N + 63u) / 64u, 1, 1);
    bars[0] = buf_barrier(b_dst, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
    bars[1] = buf_barrier(b_raw_rw, DE_ACCESS_SHADER_WRITE, DE_ACCESS_COPY_SRC);
    de_gpu_cmd_barriers(c, bars, 2);
    de_gpu_cmd_copy_buffer(c, rb, 0, b_dst, 0, sizeof ro_upload);
    de_gpu_cmd_copy_buffer(c, rb, sizeof ro_upload, b_raw_rw, 0, sizeof raw_old);
    if (de_error_last())
        printf("recording: %s\n", errchain(f->err, sizeof f->err));
    EXPECT_TRUE(de_error_last() == NULL);
    de_gpu_fence fence = de_gpu_fence_create(f->d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(fence));
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_COMPUTE, &s), DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(f->d, fence, 1, WAIT_NS), DE_OK);
    de_gpu_fence_destroy(f->d, fence);

    u32 bad = 0;
    for (u32 i = 0; i < N; ++i)
        if (res[i] != DE_PIPE_TEST_ACCESS_DST(i) && bad++ < 4)
            printf("dst[%u]: 0x%08x, expected 0x%08x\n", i, res[i], DE_PIPE_TEST_ACCESS_DST(i));
    const u32 *raw = res + N;
    for (u32 w = 0; w < 2 * N; ++w)
        if (raw[w] != DE_PIPE_TEST_ACCESS_RAW_OUT(w) && bad++ < 8)
            printf("raw_rw[%u]: 0x%08x, expected 0x%08x\n", w, raw[w],
                   DE_PIPE_TEST_ACCESS_RAW_OUT(w));
    EXPECT_EQ(bad, 0u);

    de_gpu_buffer_destroy(f->d, b_ro_upload);
    de_gpu_buffer_destroy(f->d, b_raw_upload);
    de_gpu_buffer_destroy(f->d, stage_gpu);
    de_gpu_buffer_destroy(f->d, stage_raw);
    de_gpu_buffer_destroy(f->d, b_ro_gpu);
    de_gpu_buffer_destroy(f->d, b_raw_rw);
    de_gpu_buffer_destroy(f->d, b_dst);
    de_gpu_buffer_destroy(f->d, rb);
    de_gpu_pipeline_destroy(f->d, pipe);
}

TEST_F(backends, bindless_access_metal, b_fx) {
    bindless_access(f, DE_GPU_METAL);
}
TEST_F(backends, bindless_access_vulkan, b_fx) {
    bindless_access(f, DE_GPU_VULKAN);
}
TEST_F(backends, bindless_access_d3d12, b_fx) {
    bindless_access(f, DE_GPU_D3D12);
}
