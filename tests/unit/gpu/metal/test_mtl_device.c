/* test_mtl_device.c — the Metal backend's device, caps, queues and heaps (DENG-287).
 *
 * Runs on the host's GPU. Metal API validation is on for the whole process (MTL_DEBUG_LAYER=1,
 * set below before the first device unless the environment already sets it): a validation error
 * aborts the test. Tests skip when the machine has no Metal device, and the ones that need the
 * Base tier skip below it (a CI runner's paravirtual device).
 *
 * This file is C and includes mtl_internal.h: that the backend's internal header compiles as C11
 * with -Wpedantic is part of `no_objc_in_headers`.
 */
#include "de_test.h"

#include <dengine/de_gpu.h>

#include "gpu/metal/mtl_internal.h"
#include "mtl_test_helpers.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Before main and before any Metal call: switch the API validation layer on. */
__attribute__((constructor)) static void enable_metal_validation(void) {
    setenv("MTL_DEBUG_LAYER", "1", 0);
}

/* True when the last error or one it wraps mentions `text`. */
static bool error_mentions(const char *text) {
    for (const de_error *e = de_error_last(); e; e = e->cause)
        if (strstr(e->message, text))
            return true;
    return false;
}

/* ---------- Device and caps ---------- */

TEST_F(gpu_metal, device_caps, mtl_fx) {
    const de_gpu_caps *c = de_gpu_get_caps(f->d);
    const de_test_mtl_info *i = &f->info;
    EXPECT_EQ(c->backend, DE_GPU_METAL);
    EXPECT_STREQ(c->device_name, i->name);
    EXPECT_EQ(c->vendor_id, 0x106Bu);

    /* Base tier (SPEC §3.3): Metal 3, Apple7, argument buffers tier 2. */
    const bool base = i->metal3 && i->apple7 && i->ab_tier2;
    EXPECT_EQ(c->base_tier_ok, base);
    if (base)
        EXPECT_STREQ(c->base_tier_reason, "");
    else
        EXPECT_TRUE(c->base_tier_reason[0] != '\0');

    /* RT: hardware on Apple9 (M3+), API-only below, none without the API. */
    const de_rt_support rt =
        !i->raytracing ? DE_RT_NONE : (i->apple9 ? DE_RT_HARDWARE : DE_RT_API_ONLY);
    EXPECT_EQ(c->raytracing, rt);
    EXPECT_EQ(c->ray_query, i->raytracing);
    EXPECT_FALSE(c->rt_pipelines);

    EXPECT_EQ(c->mesh_shaders, i->metal3);
    EXPECT_EQ(c->mesh_max_output_vertices, i->metal3 ? 256u : 0u);
    EXPECT_EQ(c->mesh_max_output_primitives, i->metal3 ? 512u : 0u);
    EXPECT_EQ(c->wave_size_min, 32u);
    EXPECT_EQ(c->wave_size_max, 32u);
    EXPECT_TRUE(c->async_compute);
    EXPECT_FALSE(c->enhanced_barriers);

    EXPECT_EQ(c->unified_memory, i->unified);
    EXPECT_EQ(c->dedicated_vram, i->working_set);
    EXPECT_GT(c->dedicated_vram, 0u);

    /* Bindless sizes are the DeBindlessHeap arrays of bindless.slang. */
    EXPECT_EQ(c->max_bindless_textures, 65536u);
    EXPECT_EQ(c->max_bindless_storage_textures, 16384u);
    EXPECT_EQ(c->max_bindless_buffers, 65536u);
    EXPECT_EQ(c->max_bindless_samplers, i->max_samplers < 2048u ? (u32)i->max_samplers : 2048u);
    EXPECT_EQ(c->max_bindless_accels, 0u);
    EXPECT_EQ(c->max_push_constant_bytes, DE_GPU_MAX_PUSH_CONSTANT_BYTES);
    EXPECT_GT(c->timestamp_frequency, 0u);

    printf(
        "metal device \"%s\": base tier %s%s%s, rt %d, mesh %d, unified %d, working set %llu MiB, "
        "timestamps %llu Hz\n",
        c->device_name, c->base_tier_ok ? "ok" : "NO", c->base_tier_ok ? "" : ": ",
        c->base_tier_reason, (int)c->raytracing, (int)c->mesh_shaders, (int)c->unified_memory,
        (unsigned long long)(c->dedicated_vram >> 20), (unsigned long long)c->timestamp_frequency);
}

TEST(gpu_metal, default_backend_is_metal) {
    EXPECT_EQ(de_gpu_platform_default_backend(), DE_GPU_METAL);
    EXPECT_TRUE(de__gpu_find_backend(DE_GPU_DEFAULT) == &de__gpu_metal_vtable);
    de_gpu_device *d = NULL;
    de_result r = de_gpu_device_create(&(de_gpu_device_desc){0}, &d);
    if (r == DE_ERR_UNSUPPORTED)
        SKIP("no Metal device on this machine");
    ASSERT_EQ(r, DE_OK);
    EXPECT_EQ(de_gpu_get_caps(d)->backend, DE_GPU_METAL);
    EXPECT_TRUE(de__mtl_device_object(d) != NULL);
    de_gpu_device_destroy(d);
}

TEST(gpu_metal, adapter_selection) {
    de_test_mtl_info info;
    if (!de_test_mtl_device_info(0, &info))
        SKIP("no Metal device on this machine");
    /* adapter n is MTLCopyAllDevices()[n - 1]. */
    de_test_mtl_info first;
    ASSERT_TRUE(de_test_mtl_device_info(1, &first) == 1);
    de_gpu_device *d = NULL;
    ASSERT_EQ(
        de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_METAL, .adapter = 1}, &d),
        DE_OK);
    EXPECT_STREQ(de_gpu_get_caps(d)->device_name, first.name);
    de_gpu_device_destroy(d);

    /* One past the last adapter fails and says how many there are. */
    d = NULL;
    const u32 past = info.device_count + 1;
    EXPECT_EQ(
        de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_METAL, .adapter = past}, &d),
        DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(d == NULL);
    EXPECT_TRUE(error_mentions("adapter"));
}

/* ---------- Queues ---------- */

TEST_F(gpu_metal, queue_timelines, mtl_fx) {
    /* Each queue's internal timeline is an MTLSharedEvent the submission signals. Commands are
     * DENG-288; an empty command list still makes a submission. */
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        ASSERT_TRUE(de__mtl_queue_object(f->d, (de_gpu_queue)q) != NULL);
        for (u32 n = 0; n < 3; ++n) {
            de_gpu_cmd *c = de_gpu_cmd_begin(f->d, (de_gpu_queue)q, "empty");
            ASSERT_TRUE(c != NULL);
            de_gpu_submit s = {.cmds = &c, .cmd_count = 1};
            ASSERT_EQ(de_gpu_queue_submit(f->d, (de_gpu_queue)q, &s), DE_OK);
        }
    }
    /* A destroy waits for the values submitted before it. */
    de_gpu_buffer b = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = 256, .usage = DE_USAGE_STORAGE, .name = "deferred"});
    ASSERT_NE(b.id, 0u);
    de_gpu_buffer_destroy(f->d, b);
    de_gpu_wait_idle(f->d);
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        const u64 submitted = de_atomic_load_u64(&f->d->submitted[q], DE_ATOMIC_ACQUIRE);
        EXPECT_EQ(submitted, 3u);
        EXPECT_GE(f->d->vt->queue_completed(f->d, (de_gpu_queue)q), submitted);
    }
    EXPECT_EQ(de__gpu_deferred_count(f->d), 0u);
    EXPECT_EQ(de__gpu_live_count(f->d, DE__GPU_BUFFER), 0u);
}

/* ---------- Heaps and placement ---------- */

static bool in_heap_list(de_gpu_device *d, void *heap) {
    void *heaps[64];
    const u32 n = de__mtl_heap_list(d, heaps, 64);
    for (u32 i = 0; i < n && i < 64; ++i)
        if (heaps[i] == heap)
            return true;
    return false;
}

/* The buffer sits in a class heap at the offset the front-end's TLSF chose. */
static void check_buffer_placement(de_gpu_device *d, de_gpu_buffer h, u32 storage, u32 cache) {
    de__gpu_buffer *b = de__gpu_get_buffer(d, h);
    ASSERT_TRUE(b != NULL);
    const mtl_buffer *mb = DE__GPU_PAYLOAD(mtl_buffer, b);
    ASSERT_TRUE(mb->alloc.heap != NULL);
    EXPECT_EQ(mb->alloc.mem_class, (u32)b->memory);
    void *heap = NULL;
    u64 offset = ~(u64)0;
    ASSERT_TRUE(de__mtl_buffer_placement(d, h, &heap, &offset));
    EXPECT_TRUE(heap == DE__GPU_PAYLOAD(mtl_heap, mb->alloc.heap)->heap);
    EXPECT_EQ(offset, mb->alloc.offset);
    EXPECT_TRUE(in_heap_list(d, heap));
    u32 st, hz, cm;
    de_test_mtl_resource_modes(mb->buffer, &st, &hz, &cm);
    EXPECT_EQ(st, storage);
    EXPECT_EQ(hz, de_test_mtl_hazard_untracked());
    if (cache != ~0u)
        EXPECT_EQ(cm, cache);
    EXPECT_EQ(mb->gpu_address, de_test_mtl_buffer_address(mb->buffer));
}

TEST_F(gpu_metal, heap_placement, mtl_fx) {
    de_gpu_device *d = f->d;
    /* The residency list starts with the backend's heap of null descriptors. */
    void *first = NULL;
    EXPECT_EQ(de__mtl_heap_list(d, &first, 1), 1u);
    EXPECT_TRUE(first == de__mtl_dev(d)->internal.heap);

    de_gpu_buffer gpu = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 1000, .usage = DE_USAGE_STORAGE, .name = "gpu"});
    de_gpu_buffer up = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){
               .size = 4096, .usage = DE_USAGE_CONSTANT, .memory = DE_MEM_UPLOAD, .name = "up"});
    de_gpu_buffer rb = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){
               .size = 4096, .usage = DE_USAGE_COPY_DST, .memory = DE_MEM_READBACK, .name = "rb"});
    de_gpu_texture tex = de_gpu_texture_create(
        d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                  .width = 256,
                                  .height = 256,
                                  .mip_count = 9,
                                  .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                  .name = "tex"});
    ASSERT_NE(gpu.id, 0u);
    ASSERT_NE(up.id, 0u);
    ASSERT_NE(rb.id, 0u);
    ASSERT_NE(tex.id, 0u);

    check_buffer_placement(d, gpu, de_test_mtl_storage_private(), ~0u);
    const bool shared_heaps = de__mtl_dev(d)->shared_heaps;
    if (shared_heaps) {
        check_buffer_placement(d, up, de_test_mtl_storage_shared(),
                               de_test_mtl_cpu_write_combined());
        check_buffer_placement(d, rb, de_test_mtl_storage_shared(), ~0u);
    } else {
        /* Mac2-family / paravirtual devices: standalone shared buffers (below the Base tier). */
        printf("note: no shared-storage heaps on \"%s\"\n", f->info.name);
        EXPECT_FALSE(de__mtl_buffer_placement(d, up, NULL, NULL));
        EXPECT_FALSE(de__mtl_buffer_placement(d, rb, NULL, NULL));
    }

    de__gpu_texture *t = de__gpu_get_texture(d, tex);
    const mtl_texture *mt = DE__GPU_PAYLOAD(mtl_texture, t);
    void *heap = NULL;
    u64 offset = 0;
    ASSERT_TRUE(de__mtl_texture_placement(d, tex, &heap, &offset));
    EXPECT_TRUE(heap == DE__GPU_PAYLOAD(mtl_heap, mt->alloc.heap)->heap);
    EXPECT_EQ(offset, mt->alloc.offset);
    EXPECT_EQ(mt->alloc.mem_class, (u32)DE__GPU_MEM_CLASS_GPU_ONLY);
    de__gpu_mem_req req;
    ASSERT_EQ(de__gpu_texture_requirements(
                  d,
                  &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                         .width = 256,
                                         .height = 256,
                                         .depth_or_layers = 1,
                                         .mip_count = 9,
                                         .sample_count = 1,
                                         .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST},
                  &req),
              DE_OK);
    EXPECT_EQ(offset % req.align, 0u);
    EXPECT_GE(mt->alloc.size, req.size);
    u32 st, hz, cm;
    de_test_mtl_resource_modes(mt->texture, &st, &hz, &cm);
    EXPECT_EQ(st, de_test_mtl_storage_private());
    EXPECT_EQ(hz, de_test_mtl_hazard_untracked());

    /* Shared buffers: `contents` is the persistent mapping and is writable; GPU_ONLY has none. */
    u8 *pu = (u8 *)de_gpu_buffer_map(d, up);
    u8 *pr = (u8 *)de_gpu_buffer_map(d, rb);
    ASSERT_TRUE(pu != NULL);
    ASSERT_TRUE(pr != NULL);
    for (u32 i = 0; i < 4096; ++i) {
        pu[i] = (u8)(i * 7u);
        pr[i] = (u8)(i * 13u);
    }
    for (u32 i = 0; i < 4096; ++i) {
        ASSERT_EQ(pu[i], (u8)(i * 7u));
        ASSERT_EQ(pr[i], (u8)(i * 13u));
    }
    EXPECT_TRUE(de_gpu_buffer_map(d, gpu) == NULL);

    /* Memory is accounted per class and comes back after the GPU is done. */
    EXPECT_GT(de__gpu_mem_kind_used(d, DE_MEM_GPU_ONLY), 0u);
    EXPECT_EQ(de__gpu_mem_kind_used(d, DE_MEM_UPLOAD) > 0, shared_heaps);
    EXPECT_EQ(de__gpu_mem_kind_used(d, DE_MEM_READBACK) > 0, shared_heaps);
    de_gpu_buffer_destroy(d, gpu);
    de_gpu_buffer_destroy(d, up);
    de_gpu_buffer_destroy(d, rb);
    de_gpu_texture_destroy(d, tex);
    de_gpu_wait_idle(d);
    EXPECT_EQ(de__gpu_mem_kind_used(d, DE_MEM_GPU_ONLY), 0u);
    EXPECT_EQ(de__gpu_mem_kind_used(d, DE_MEM_UPLOAD), 0u);
    EXPECT_EQ(de__gpu_mem_kind_used(d, DE_MEM_READBACK), 0u);
    /* Each class keeps its empty block heap as the spare, still on the residency list. */
    EXPECT_EQ(de__mtl_heap_list(d, NULL, 0), shared_heaps ? 4u : 2u);
}

TEST_F(gpu_metal, placed_transients, mtl_fx) {
    de_gpu_device *d = f->d;
    de__gpu_transient_region *region = NULL;
    ASSERT_EQ(de__gpu_transient_region_create(d, DE__GPU_MEM_CLASS_GPU_ONLY, 8u << 20, &region),
              DE_OK);
    const de__gpu_alloc *ra = de__gpu_transient_region_alloc(region);
    void *region_heap = DE__GPU_PAYLOAD(mtl_heap, ra->heap)->heap;

    /* Two render targets aliasing the same memory, and a buffer after them. */
    const de_gpu_texture_desc rt = {.format = DE_FMT_RGBA16_FLOAT,
                                    .width = 512,
                                    .height = 512,
                                    .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED,
                                    .name = "rt"};
    de_gpu_texture a = de__gpu_place_transient_texture(region, 0, &rt);
    de_gpu_texture b = de__gpu_place_transient_texture(region, 0, &rt);
    de_gpu_buffer buf = de__gpu_place_transient_buffer(
        region, 4u << 20,
        &(de_gpu_buffer_desc){.size = 65536, .usage = DE_USAGE_STORAGE, .name = "scratch"});
    ASSERT_NE(a.id, 0u);
    ASSERT_NE(b.id, 0u);
    ASSERT_NE(buf.id, 0u);
    void *heap = NULL;
    u64 offset = 0;
    ASSERT_TRUE(de__mtl_texture_placement(d, a, &heap, &offset));
    EXPECT_TRUE(heap == region_heap);
    EXPECT_EQ(offset, ra->offset);
    ASSERT_TRUE(de__mtl_texture_placement(d, b, &heap, &offset));
    EXPECT_EQ(offset, ra->offset);
    ASSERT_TRUE(de__mtl_buffer_placement(d, buf, &heap, &offset));
    EXPECT_TRUE(heap == region_heap);
    EXPECT_EQ(offset, ra->offset + (4u << 20));
    /* The region owns the memory: the placed resources hold no allocation of their own. */
    EXPECT_TRUE(DE__GPU_PAYLOAD(mtl_buffer, de__gpu_get_buffer(d, buf))->alloc.heap == NULL);
    EXPECT_EQ(de__gpu_transient_region_placed(region), 3u);

    de_gpu_texture_destroy(d, a);
    de_gpu_texture_destroy(d, b);
    de_gpu_buffer_destroy(d, buf);
    de__gpu_transient_region_destroy(region);
    de_gpu_wait_idle(d);
    EXPECT_EQ(de__gpu_mem_kind_used(d, DE_MEM_GPU_ONLY), 0u);

    /* An UPLOAD region gives mappable placed buffers (where shared heaps exist). */
    if (!de__mtl_dev(d)->shared_heaps) {
        EXPECT_EQ(de__gpu_transient_region_create(d, DE__GPU_MEM_CLASS_UPLOAD, 1u << 20, &region),
                  DE_ERR_UNSUPPORTED);
        return;
    }
    ASSERT_EQ(de__gpu_transient_region_create(d, DE__GPU_MEM_CLASS_UPLOAD, 1u << 20, &region),
              DE_OK);
    de_gpu_buffer ub = de__gpu_place_transient_buffer(
        region, 0,
        &(de_gpu_buffer_desc){
            .size = 4096, .usage = DE_USAGE_CONSTANT, .memory = DE_MEM_UPLOAD, .name = "ring"});
    ASSERT_NE(ub.id, 0u);
    u8 *p = (u8 *)de_gpu_buffer_map(d, ub);
    ASSERT_TRUE(p != NULL);
    memset(p, 0xAB, 4096);
    EXPECT_EQ(p[4095], 0xABu);
    de_gpu_buffer_destroy(d, ub);
    de__gpu_transient_region_destroy(region);
    de_gpu_wait_idle(d);
}

/* The fallbacks of Mac2-family and paravirtual devices (below the Base tier), forced on this one:
 * without shared-storage heaps, UPLOAD and READBACK buffers become standalone shared MTLBuffers,
 * still mapped and in the bindless table; without border colors, DE_ADDR_BORDER is ClampToZero. */
TEST_F(gpu_metal, fallbacks_below_base_tier, mtl_fx) {
    de_gpu_device *d = f->d;
    de__mtl_dev(d)->shared_heaps = false; /* before any shared heap exists */
    de__mtl_dev(d)->border_color = false;
    de_gpu_sampler smp =
        de_gpu_sampler_create(d, &(de_gpu_sampler_desc){.u = DE_ADDR_BORDER, .v = DE_ADDR_BORDER});
    EXPECT_NE(smp.id, 0u);
    de_gpu_sampler_destroy(d, smp);
    de_gpu_buffer up = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){
               .size = 4096, .usage = DE_USAGE_CONSTANT, .memory = DE_MEM_UPLOAD, .name = "up"});
    de_gpu_buffer rb = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){
               .size = 4096, .usage = DE_USAGE_COPY_DST, .memory = DE_MEM_READBACK, .name = "rb"});
    ASSERT_NE(up.id, 0u);
    ASSERT_NE(rb.id, 0u);
    EXPECT_FALSE(de__mtl_buffer_placement(d, up, NULL, NULL));
    u8 *p = (u8 *)de_gpu_buffer_map(d, up);
    ASSERT_TRUE(p != NULL);
    memset(p, 0x5A, 4096);
    EXPECT_EQ(p[100], 0x5Au);
    ASSERT_TRUE(de_gpu_buffer_map(d, rb) != NULL);
    u32 st, hz, cm;
    de_test_mtl_resource_modes(de__mtl_buffer_object(d, up), &st, &hz, &cm);
    EXPECT_EQ(st, de_test_mtl_storage_shared());
    EXPECT_EQ(hz, de_test_mtl_hazard_untracked());
    const u8 *cpu = NULL;
    de__mtl_bindless_buffer(d, &cpu);
    u64 entry;
    memcpy(&entry,
           cpu + DE__MTL_BINDLESS_OFFSET_BUFFERS +
               (size_t)de_gpu_buffer_bindless(d, up) * DE__MTL_BINDLESS_ENTRY_BYTES,
           sizeof entry);
    EXPECT_EQ(entry, de_test_mtl_buffer_address(de__mtl_buffer_object(d, up)));
    EXPECT_EQ(de__gpu_mem_kind_used(d, DE_MEM_UPLOAD), 0u); /* not from the heap manager */
    /* Shared heaps (transient regions) are refused. */
    de__gpu_transient_region *region = NULL;
    EXPECT_EQ(de__gpu_transient_region_create(d, DE__GPU_MEM_CLASS_UPLOAD, 1u << 20, &region),
              DE_ERR_UNSUPPORTED);
    de_gpu_buffer_destroy(d, up);
    de_gpu_buffer_destroy(d, rb);
    de_gpu_wait_idle(d);
}

/* ---------- No Objective-C outside src/gpu/metal/ ---------- */

/* Strips comments and string/char literals, so prose that names Metal types does not count. */
static void strip_comments(char *s) {
    char *w = s;
    for (char *r = s; *r;) {
        if (r[0] == '/' && r[1] == '*') {
            r += 2;
            while (*r && !(r[0] == '*' && r[1] == '/'))
                ++r;
            r += *r ? 2 : 0;
            *w++ = ' ';
        } else if (r[0] == '/' && r[1] == '/') {
            while (*r && *r != '\n')
                ++r;
        } else if (*r == '"' || *r == '\'') {
            const char q = *r++;
            while (*r && *r != q) {
                if (*r == '\\' && r[1])
                    ++r;
                ++r;
            }
            r += *r ? 1 : 0;
            *w++ = q;
            *w++ = q;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* An Objective-C construct or Apple framework type in C code, or NULL. */
static const char *objc_token(const char *code) {
    static const char *const k_tokens[] = {
        "#import",   "@interface",       "@protocol", "@class",       "@end",
        "@property", "@autoreleasepool", "id<",       "__bridge",     "NSObject",
        "NSString",  "NSArray",          "NSError",   "CAMetalLayer", "dispatch_data_t",
    };
    for (size_t i = 0; i < sizeof k_tokens / sizeof k_tokens[0]; ++i)
        if (strstr(code, k_tokens[i]))
            return k_tokens[i];
    /* Metal types: "MTL" followed by an upper-case letter (MTLDevice, MTLResourceID, ...). */
    for (const char *p = strstr(code, "MTL"); p; p = strstr(p + 1, "MTL"))
        if (p[3] >= 'A' && p[3] <= 'Z' &&
            (p == code ||
             !(p[-1] == '_' || (p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= 'a' && p[-1] <= 'z'))))
            return "MTL<Type>";
    return NULL;
}

static u32 g_headers_scanned;

static void scan_dir(const char *dir) {
    DIR *dh = opendir(dir);
    if (!dh)
        return;
    struct dirent *e;
    while ((e = readdir(dh)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat stbuf;
        if (stat(path, &stbuf) != 0)
            continue;
        if (S_ISDIR(stbuf.st_mode)) {
            scan_dir(path);
            continue;
        }
        const size_t n = strlen(e->d_name);
        if (n < 3 || strcmp(e->d_name + n - 2, ".h") != 0)
            continue;
        FILE *fp = fopen(path, "rb");
        ASSERT_TRUE(fp != NULL);
        fseek(fp, 0, SEEK_END);
        const long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        char *text = (char *)malloc((size_t)size + 1);
        ASSERT_TRUE(text != NULL);
        const size_t got = fread(text, 1, (size_t)size, fp);
        fclose(fp);
        text[got] = '\0';
        strip_comments(text);
        const char *tok = objc_token(text);
        if (tok) {
            static char msg[1200];
            snprintf(msg, sizeof msg, "%s uses Objective-C or a Metal type (%s)", path, tok);
            free(text);
            FAIL(msg);
        }
        free(text);
        ++g_headers_scanned;
    }
    closedir(dh);
}

TEST(gpu_metal, no_objc_in_headers) {
    /* Every header of the engine, the tools and the tests, the Metal backend's own
     * mtl_internal.h included: ObjC stays in .m files. (Including src/gpu/metal/ from another
     * module is the layering check's job: no allow edge leads to gpu/metal.) */
    static const char *const k_dirs[] = {"include", "src", "tools", "tests/unit"};
    g_headers_scanned = 0;
    for (size_t i = 0; i < sizeof k_dirs / sizeof k_dirs[0]; ++i) {
        char dir[512];
        snprintf(dir, sizeof dir, "%s/%s", DE_TEST_ROOT_DIR, k_dirs[i]);
        scan_dir(dir);
    }
    EXPECT_GT(g_headers_scanned, 100u);

    /* The scanner itself: it finds what it should and ignores comments. */
    char sample[] = "/* id<MTLDevice> in a comment */ void *x; // NSString\n";
    strip_comments(sample);
    EXPECT_TRUE(objc_token(sample) == NULL);
    char bad[] = "typedef struct { id<MTLBuffer> b; } t;";
    strip_comments(bad);
    EXPECT_TRUE(objc_token(bad) != NULL);
    char bad2[] = "MTLResourceID rid;";
    EXPECT_TRUE(objc_token(bad2) != NULL);
    char ok[] = "u32 DE__MTL_BINDLESS_TEXTURES; de__mtl_heap_list();";
    EXPECT_TRUE(objc_token(ok) == NULL);
}
