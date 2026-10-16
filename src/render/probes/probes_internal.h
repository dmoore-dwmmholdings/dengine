/* Shared radiance capture/storage/persistence contract (DENG-340, DENG-341, DDGI S16).
 * Main thread: create/destroy, request, prepare. Render thread: capture/blend/build.
 * Pool slots are generational; no caller may retain a raw slot after an eviction.
 * Canonical storage is RGBA16F cube-array; strips mirror it for the RHI's 2D bindless table.
 */
#ifndef DE_RENDER_PROBES_INTERNAL_H
#define DE_RENDER_PROBES_INTERNAL_H
#include <dengine/de_render.h>
#include <dengine/de_memory.h>
#include "render/world/render_world.h"
#include "render/passes/vis/vis_stages.h"
DE_EXTERN_C_BEGIN
#define DE__PROBE_MAX          64u
#define DE__PROBE_NONE         0xffffffffu
#define DE__PROBE_BLOB_HEADER  48u
#define DE__PROBE_BLOB_VERSION 1u
/** Immutable Extract snapshot; position is absolute, rotation maps probe local to world. */

typedef struct de__probe_slot {
    u64 owner, used;
    u32 generation, resolution, mips, mip_base;
    bool ready;
    de_gpu_texture strip;
} de__probe_slot;
typedef struct de__probe_handle {
    u32 slot, generation;
} de__probe_handle;
typedef struct de__probe_store {
    de_gpu_device *dev;
    const de_allocator *alloc;
    u32 resolution, mips, capacity, cube_access;
    de_gpu_texture cube;
    de__probe_slot slots[DE__PROBE_MAX];
} de__probe_store;
/** Generic cube-array pool. Smaller allocations start at a mip offset, retaining one N-slot budget.
 */
de_result de__probe_store_init(de__probe_store *s, de_gpu_device *dev, const de_allocator *a,
                               u32 resolution, u32 capacity);
void de__probe_store_destroy(de__probe_store *s);
de__probe_handle de__probe_store_alloc(de__probe_store *s, u64 owner, u64 frame);
/** Mixed-resolution allocation: smaller cubes start at log2(pool_resolution/resolution). */
de__probe_handle de__probe_store_alloc_resolution(de__probe_store *s, u64 owner, u64 frame,
                                                  u32 resolution);
bool de__probe_store_valid(const de__probe_store *s, de__probe_handle handle);
void de__probe_store_free(de__probe_store *s, u32 slot);
u64 de__probe_payload_size(u32 resolution);
/** Little-endian versioned blob, payload mip-major, then +X,-X,+Y,-Y,+Z,-Z, packed half RGBA.
 * No struct padding, timestamps or pointers. Hash validates settings and payload corruption. */
de_result de__probe_blob_write(void *dst, u64 size, u64 settings_hash, u32 resolution,
                               const void *payload);
de_result de__probe_blob_read(const void *src, u64 size, u64 settings_hash, u32 *resolution,
                              const void **payload);
/** Shared 48-byte envelope. DEPR v1 callers retain their exact existing bytes. Payloads
 * define their own bounded layout; readers validate semantic contents after this checksum. */
de_result de__probe_envelope_write(void *dst, u64 size, const char magic[4], u32 tag, u64 settings,
                                   const void *payload, u64 bytes);
de_result de__probe_envelope_read(const void *src, u64 size, const char magic[4], u64 settings,
                                  u32 *tag, const void **payload, u64 *bytes);
u64 de__probe_settings_hash(const de__probe_snapshot *p);
/** Cube camera directions match de_cooked_env_face_direction, no temporal history. */
void de__probe_face_camera(de_dvec3 position, de_dvec3 origin, u32 face, de__rw_view *out);
f32 de__probe_weight(const DeReflectionProbe *p, de_vec3 local);
de_vec3 de__probe_parallax(const DeReflectionProbe *p, de_vec3 local, de_vec3 direction);
/** Generic reusable capture. Six real RTT views are scheduled on the main thread; complete
 * runs after those views. All transfers stay on graphics queue; storage publishes only after
 * all six faces, with per-resource barriers. No GPU readback in ordinary frames. */
typedef struct de__probe_capture {
    de_renderer *renderer;
    u32 resolution, mips, scheduled;
    de_view views[6];
    u32 expected_executed[6];
    de_gpu_texture targets[6], raw, filtered;
    de_gpu_buffer transfer;
    u64 transfer_bytes;
    de_gpu_sampler sampler;
    de__vis_kernel copy, downsample, prefilter;
    bool initialized;
    u32 raw_access, filtered_access, transfer_access;
} de__probe_capture;
de_result de__probe_capture_init(de__probe_capture *c, de_renderer *r, u32 resolution);
void de__probe_capture_destroy(de__probe_capture *c);
void de__probe_capture_at_position(de__probe_capture *c, de_dvec3 position, de_dvec3 origin,
                                   u32 face_mask);
/** With filter=false, s/handle are ignored, including NULL/invalid. Raw mip 0 contains the
 * scheduled faces in GENERAL (SHADER_READ|SHADER_WRITE), accessed by per-mip storage descriptors.
 * This supports independent low-resolution radiance integration (including 32 square captures).
 * The caller owns subsequent GPU barriers, completion/fence lifetime and capture destruction. */
de_result de__probe_capture_complete(de__probe_capture *c, de__probe_store *s,
                                     de__probe_handle handle, bool filter);
/** Upload/read canonical packed bytes; bake calls wait for GPU, ordinary upload queues copies. */
de_result de__probe_store_upload(de__probe_capture *c, de__probe_store *s, de__probe_handle h,
                                 const void *payload);
de_result de__probe_store_readback(de__probe_capture *c, de__probe_store *s, de__probe_handle h,
                                   void *payload);
/** Warm per-view bounds ring. Failed growth preserves the old allocation for a retry. */
typedef struct de__probe_bounds_ring {
    de_gpu_buffer bounds;
    f32 *map;
    u32 capacity;
} de__probe_bounds_ring;
de_result de__probe_bounds_reserve(de_gpu_device *dev, de__probe_bounds_ring *ring, u32 count);
struct de__probes;
de_result de__probes_create(de_renderer *r, struct de__probes **out);
void de__probes_destroy(de_renderer *r, struct de__probes *p);
void de__probes_prepare(de_renderer *r, const de__render_world_frame *world);
void de__probes_after_views(de_renderer *r, de_arena *arena, u64 frame);
typedef struct de__probe_info {
    u32 resolution, faces, capture_faces;
    bool ready, baked;
} de__probe_info;
bool de__probes_info(const de_renderer *r, u64 entity, de__probe_info *out);
de_result de__probes_readback(de_renderer *r, u64 entity, void *payload, u64 size);
bool de__probes_capture_view(const de_renderer *r, de_view view);
DE_EXTERN_C_END
#endif
