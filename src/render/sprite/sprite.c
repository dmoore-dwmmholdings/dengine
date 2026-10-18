#include "sprite_internal.h"
#include "render/renderer_internal.h"
#include <string.h>

void de_render_set_2d_plane(de_renderer *r, de_plane_2d plane) {
    if (r == NULL || (plane != DE_PLANE_SIDE_XZ && plane != DE_PLANE_TOP_XY))
        return;
    de_mutex_lock(&r->lock);
    r->plane_2d = plane;
    de_mutex_unlock(&r->lock);
}

de_result de__sprite_quad_create(de_gpu_device *dev, de_render_mesh *mesh) {
    /* One immutable shared unit quad. A genuine DMSH meshlet lets both raster paths,
     * all cull stages, material resolve, shadows and picking use their ordinary path. */
    u32 data[64] = {0};
    const f32 positions[12] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const f32 uv[8] = {0, 1, 1, 1, 1, 0, 0, 0};
    memcpy(data, positions, sizeof positions);
    memcpy(data + 12, uv, sizeof uv);
    /* OCT16 normals +Z and tangents +X, positive handedness. */
    for (u32 i = 0; i < 4; ++i)
        data[24 + i] = 0x00007fffu;
    const u32 record = 32;
    data[record + 2] = 4u | (2u << 8);
    const f32 sphere[4] = {.5f, .5f, 0, .7071068f};
    memcpy(data + record + 4, sphere, sizeof sphere);
    const f32 cutoff = 1;
    memcpy(data + record + 11, &cutoff, sizeof cutoff);
    for (u32 i = 0; i < 4; ++i)
        data[48 + i] = i;
    const u8 triangles[8] = {0, 1, 2, 0, 2, 3, 0, 0};
    memcpy(data + 52, triangles, sizeof triangles);
    memset(mesh, 0, sizeof *mesh);
    const u32 indices[6] = {0, 1, 2, 0, 2, 3};
    const void *parts[5] = {data, data + 32, data + 48, data + 52, indices};
    const u64 sizes[5] = {128, 64, 16, 8, sizeof indices};
    for (u32 i = 0; i < DE_RENDER_MESH_BUFFER_COUNT; ++i) {
        de_gpu_buffer b = de_gpu_buffer_create(
            dev, &(de_gpu_buffer_desc){.size = sizes[i],
                                       .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                       .memory = DE_MEM_UPLOAD,
                                       .name = "sprite shared quad"});
        if (!DE_HANDLE_VALID(b))
            goto fail;
        mesh->buffers[i] = b;
        void *mapped = de_gpu_buffer_map(dev, b);
        if (mapped == NULL)
            goto fail;
        memcpy(mapped, parts[i], (size_t)sizes[i]);
        mesh->bindless[i] = de_gpu_buffer_bindless(dev, b);
        mesh->buffer_bytes[i] = sizes[i];
    }
    mesh->vertex_count = 4;
    mesh->index_count = 6;
    mesh->meshlet_count = 1;
    mesh->submesh_count = 1;
    mesh->lod_count = 1;
    mesh->bounds_max[0] = mesh->bounds_max[1] = 1;
    memcpy(mesh->sphere, sphere, sizeof sphere);
    mesh->stream_offset[DE_COOKED_STREAM_POSITION] = 0;
    mesh->stream_format[DE_COOKED_STREAM_POSITION] = DE_COOKED_VF_F32X3;
    mesh->stream_offset[DE_COOKED_STREAM_UV0] = 48;
    mesh->stream_format[DE_COOKED_STREAM_UV0] = DE_COOKED_VF_F32X2;
    mesh->stream_offset[DE_COOKED_STREAM_NORMAL] = 80;
    mesh->stream_format[DE_COOKED_STREAM_NORMAL] = DE_COOKED_VF_OCT16;
    mesh->stream_offset[DE_COOKED_STREAM_TANGENT] = 96;
    mesh->stream_format[DE_COOKED_STREAM_TANGENT] = DE_COOKED_VF_OCT16_SIGN;
    /* The stream offsets are local; meshlet accessors add these section offsets. */
    mesh->lods[0] = (de_cooked_mesh_lod){
        .index_count = 6, .meshlet_count = 1, .triangle_count = 2, .vertex_count = 4};
    return DE_OK;
fail:
    for (u32 i = 0; i < DE_RENDER_MESH_BUFFER_COUNT; ++i)
        if (DE_HANDLE_VALID(mesh->buffers[i]))
            de_gpu_buffer_destroy(dev, mesh->buffers[i]);
    memset(mesh, 0, sizeof *mesh);
    return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "sprite shared quad");
}

void de__sprite_pack_material(const DeSprite *s, de__gs_material *o,
                              u32 (*texture)(void *, de_texture), void *user) {
    memset(o, 0, sizeof *o);
    o->model = DE__GS_MATERIAL_MODEL_SPRITE;
    o->flags = DE__MATERIAL_FLAG_READY | DE__MATERIAL_FLAG_DOUBLE_SIDED;
    o->alpha_mode = s->translucent ? 2u : 1u;
    o->features = DE__MATERIAL_FEATURE_DOUBLE_SIDED |
                  (s->translucent ? DE__MATERIAL_FEATURE_FORWARD : DE__MATERIAL_FEATURE_ALPHA_MASK);
    memcpy(o->base_color.v, &s->color, sizeof s->color);
    o->pbr.v[0] = DE_HANDLE_VALID(s->rough_metal_tex) ? 1 : 0;
    o->pbr.v[1] = 1;
    o->pbr.v[2] = 1;
    o->pbr.v[3] = .5f;
    for (u32 i = 0; i < 4; ++i)
        o->specular.v[i] = 1;
    o->clearcoat.v[2] = 1.5f;
    if (DE_HANDLE_VALID(s->emissive_tex))
        for (u32 i = 0; i < 4; ++i)
            o->emissive.v[i] = 1;
    const de_texture textures[5] = {s->texture, s->rough_metal_tex, s->normal_tex, s->emissive_tex,
                                    s->height_tex};
    const u32 slots[5] = {0, 1, 2, 4, 18};
    for (u32 i = 0; i < 5; ++i)
        if (texture && DE_HANDLE_VALID(textures[i]))
            o->textures[slots[i] / 4].v[slots[i] % 4] = texture(user, textures[i]);
    /* Sprite-only storage: uv offset and signed scale; extra.w is height scale. */
    o->subsurface.v[0] = s->uv_rect.x + (s->flip_x ? s->uv_rect.z : 0);
    o->subsurface.v[1] = s->uv_rect.y + (s->flip_y ? s->uv_rect.w : 0);
    o->subsurface.v[2] = s->uv_rect.z * (s->flip_x ? -1 : 1);
    o->subsurface.v[3] = s->uv_rect.w * (s->flip_y ? -1 : 1);
    o->extra.v[3] = s->thickness;
}
