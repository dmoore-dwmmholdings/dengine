/* mtl_test_helpers.h — C interface of mtl_test_helpers.m (Metal backend tests, DENG-287). */
#ifndef DE_TEST_MTL_HELPERS_H
#define DE_TEST_MTL_HELPERS_H
#include <dengine/de_gpu.h>

#include <stdio.h>

/** What Metal itself says about a device, without the backend. */
typedef struct de_test_mtl_info {
    char name[128];
    u32 device_count;
    bool metal3, apple7, apple9, ab_tier2, raytracing, unified, rw_tier2, msaa4, bc;
    u64 working_set, max_samplers;
} de_test_mtl_info;

/** 1 and `out` filled for adapter 0 (the default device) or n (MTLCopyAllDevices()[n - 1]);
 * 0 when there is no such device. */
int de_test_mtl_device_info(u32 adapter, de_test_mtl_info *out);

/** gpuResourceID / gpuAddress of a Metal object (`void *` from the de__mtl_*_object accessors). */
u64 de_test_mtl_texture_id(void *texture);
u64 de_test_mtl_sampler_id(void *sampler);
u64 de_test_mtl_buffer_address(void *buffer);
/** MTLStorageMode, MTLHazardTrackingMode and MTLCPUCacheMode of a resource, and the values to
 * compare them with. */
void de_test_mtl_resource_modes(void *resource, u32 *storage, u32 *hazard, u32 *cpu_cache);
u32 de_test_mtl_storage_private(void);
u32 de_test_mtl_storage_shared(void);
u32 de_test_mtl_hazard_untracked(void);
u32 de_test_mtl_cpu_write_combined(void);
/** MTLTextureType of a texture or view, with its mip count and array length. */
u32 de_test_mtl_texture_type(void *texture, u32 *mip_levels, u32 *array_length);
u32 de_test_mtl_type_2d(void);
u32 de_test_mtl_type_2d_array(void);

/** Writes one RGBA8 texel at (0, 0) of mip 0 (blit from a staging buffer, waited for). 1 = ok;
 * 0 with the reason in `err`. */
int de_test_mtl_upload_rgba8(de_gpu_device *d, de_gpu_texture t, const u8 rgba[4], char *err,
                             size_t cap);

/**
 * Runs a compute kernel that reads the bindless table at buffer index 0 with Slang's
 * DeBindlessHeap layout. push = {sampled texture, storage texture, sampler, input buffer, output
 * buffer} indices. It writes float4s to the output buffer (bindless): [0] the sampled texel, [1]
 * the storage texel, [2] the sampled texel through the sampler, [3] input buffer element 0, [4]
 * texture slot 0, [5] buffer slot 0; then writes (0.25, 0.5, 0.75, 1) to the storage texture.
 * `out_gpu` is copied to `out_readback` (`bytes`) and the storage texel to `rw_after`.
 */
int de_test_mtl_bindless_kernel(de_gpu_device *d, const u32 push[5], de_gpu_buffer out_gpu,
                                de_gpu_buffer out_readback, u64 bytes, de_gpu_texture rw,
                                u8 rw_after[4], char *err, size_t cap);

/* ---------- Fixture for the C tests (needs de_test.h first) ---------- */
#if defined(DE_TEST_H)
#include <string.h>

/** A Metal device on a tracking allocator; skips when the machine has no Metal device. */
typedef struct mtl_fx {
    de_tracking_allocator tracker;
    de_allocator alloc;
    de_gpu_device *d;
    de_test_mtl_info info;
} mtl_fx;

static inline void mtl_fx_setup(mtl_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
    de_gpu_device_desc desc = {.backend = DE_GPU_METAL, .allocator = &f->alloc};
    de_result r = de_gpu_device_create(&desc, &f->d);
    if (r == DE_ERR_UNSUPPORTED)
        SKIP("no Metal device on this machine");
    ASSERT_EQ(r, DE_OK);
    ASSERT_TRUE(de_test_mtl_device_info(0, &f->info) == 1);
}

static inline void mtl_fx_teardown(mtl_fx *f) {
    de_gpu_device_destroy(f->d);
    de_tracking_stats st = de_tracking_allocator_stats(&f->tracker);
    EXPECT_EQ(st.blocks_live, 0u);
    EXPECT_EQ(st.bytes_live, 0u);
}

/** Skips a test that needs the Base tier (a CI runner's paravirtual device may lack it). */
static inline void mtl_require_base_tier(mtl_fx *f) {
    const de_gpu_caps *c = de_gpu_get_caps(f->d);
    static char why[200];
    if (!c->base_tier_ok) {
        snprintf(why, sizeof why, "the Metal device is below the Base tier: %s",
                 c->base_tier_reason);
        SKIP(why);
    }
}
#endif

#endif
