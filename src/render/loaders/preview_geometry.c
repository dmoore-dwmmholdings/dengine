#include "render/loaders/preview_geometry.h"
#include "cook/importers/model/model_internal.h"
#include <dengine/de_math.h>
#include <math.h>
#include <string.h>

#define SLICES 32u
#define BANDS  16u

static void vertex(de_model_mesh *mesh, u32 at, de_vec3 p, de_vec3 n, de_vec3 tangent, f32 sign,
                   f32 u, f32 v) {
    memcpy(mesh->position + at * 3, &p, sizeof p);
    memcpy(mesh->normal + at * 3, &n, sizeof n);
    memcpy(mesh->tangent + at * 4, &tangent, sizeof tangent);
    mesh->tangent[at * 4 + 3] = sign;
    mesh->uv[0][at * 2] = u;
    mesh->uv[0][at * 2 + 1] = v;
}
static de_result cook_geometry(de_model_ir *ir, de__render_preview_shape shape, de_model_buf *out) {
    if (!ir || !out || (u32)shape >= DE__RENDER_PREVIEW_SHAPES)
        return DE_ERR_INVALID_ARGUMENT;
    de_model_mesh mesh = {.name = "editor material preview", .submesh_count = 1};
    mesh.vertex_count = shape == DE__RENDER_PREVIEW_SPHERE ? (SLICES + 1) * (BANDS + 1)
                        : shape == DE__RENDER_PREVIEW_BOX  ? 24
                                                           : 4;
    mesh.index_count = shape == DE__RENDER_PREVIEW_SPHERE ? SLICES * (BANDS - 1) * 6
                       : shape == DE__RENDER_PREVIEW_BOX  ? 36
                                                          : 6;
    mesh.attr_mask =
        DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_POSITION) | DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_NORMAL) |
        DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_TANGENT) | DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_UV0);
    mesh.position = de_model_ir_array(ir, mesh.vertex_count, 3 * sizeof(f32));
    mesh.normal = de_model_ir_array(ir, mesh.vertex_count, 3 * sizeof(f32));
    mesh.tangent = de_model_ir_array(ir, mesh.vertex_count, 4 * sizeof(f32));
    mesh.uv[0] = de_model_ir_array(ir, mesh.vertex_count, 2 * sizeof(f32));
    mesh.indices = de_model_ir_array(ir, mesh.index_count, sizeof(u32));
    mesh.submeshes = de_model_ir_array(ir, 1, sizeof *mesh.submeshes);
    if (!mesh.position || !mesh.normal || !mesh.tangent || !mesh.uv[0] || !mesh.indices ||
        !mesh.submeshes)
        return ir->over_limit ? DE_ERR_UNSUPPORTED : DE_ERR_OUT_OF_MEMORY;
    u32 index = 0;
    if (shape == DE__RENDER_PREVIEW_SPHERE) {
        for (u32 row = 0; row <= BANDS; ++row)
            for (u32 col = 0; col <= SLICES; ++col) {
                const f32 u = (f32)col / SLICES, v = (f32)row / BANDS;
                const f32 theta = u * 2 * DE_PI, phi = v * DE_PI;
                const de_vec3 p = {sinf(phi) * cosf(theta), sinf(phi) * sinf(theta), cosf(phi)};
                vertex(&mesh, row * (SLICES + 1) + col, p, p,
                       (de_vec3){-sinf(theta), cosf(theta), 0}, -1, u, v);
                if (row == BANDS || col == SLICES)
                    continue;
                const u32 a = row * (SLICES + 1) + col, b = a + SLICES + 1;
                if (row) {
                    mesh.indices[index++] = a;
                    mesh.indices[index++] = b;
                    mesh.indices[index++] = a + 1;
                }
                if (row + 1 < BANDS) {
                    mesh.indices[index++] = a + 1;
                    mesh.indices[index++] = b;
                    mesh.indices[index++] = b + 1;
                }
            }
    } else {
        const de_vec3 normals[] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                   {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        const de_vec3 us[] = {{0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}};
        const de_vec3 vs[] = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 1, 0}, {0, -1, 0}};
        const f32 corners[][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const u32 tris[] = {0, 1, 2, 0, 2, 3};
        for (u32 face = 0; face < (shape == DE__RENDER_PREVIEW_BOX ? 6u : 1u); ++face) {
            const u32 axis = shape == DE__RENDER_PREVIEW_BOX ? face : 4;
            const f32 extent = shape == DE__RENDER_PREVIEW_BOX ? .75f : 1;
            const de_vec3 center =
                de_v3_scale(normals[axis], shape == DE__RENDER_PREVIEW_BOX ? extent : 0);
            for (u32 k = 0; k < 4; ++k) {
                de_vec3 p = de_v3_add(
                    center, de_v3_add(de_v3_scale(us[axis], (corners[k][0] * 2 - 1) * extent),
                                      de_v3_scale(vs[axis], (corners[k][1] * 2 - 1) * extent)));
                vertex(&mesh, face * 4 + k, p, normals[axis], us[axis], 1, corners[k][0],
                       corners[k][1]);
            }
            for (u32 k = 0; k < 6; ++k)
                mesh.indices[index++] = face * 4 + tris[k];
        }
    }
    if (index != mesh.index_count)
        return DE_ERR_INTERNAL;
    mesh.submeshes[0] = (de_model_submesh){.index_count = mesh.index_count,
                                           .vertex_count = mesh.vertex_count,
                                           .material = -1,
                                           .attr_mask = mesh.attr_mask};
    /* Bounds belong to the same generated streams, never independent approximations. */
    for (u32 axis = 0; axis < 3; ++axis) {
        f32 lo = INFINITY, hi = -INFINITY;
        for (u32 v = 0; v < mesh.vertex_count; ++v) {
            lo = fminf(lo, mesh.position[v * 3 + axis]);
            hi = fmaxf(hi, mesh.position[v * 3 + axis]);
        }
        mesh.bounds_min[axis] = mesh.submeshes[0].bounds_min[axis] = lo;
        mesh.bounds_max[axis] = mesh.submeshes[0].bounds_max[axis] = hi;
    }
    de_model_buf_init(out, ir);
    return de_model_mesh_cook(ir, &mesh,
                              &(de_model_mesh_settings){.meshlets = true,
                                                        .lod_error = .01f,
                                                        .max_position_error = .00001f},
                              NULL, out);
}

de_result de__render_preview_geometry(const de_allocator *allocator, de__render_preview_shape shape,
                                      de__render_preview_blob *out) {
    if (!out || (u32)shape >= DE__RENDER_PREVIEW_SHAPES)
        return DE_ERR_INVALID_ARGUMENT;
    const de_allocator *a = allocator ? allocator : de_heap_allocator();
    de_model_ir ir;
    de_model_ir_init(&ir, a, "editor preview primitive");
    ir.byte_limit = 4u << 20;
    de_model_buf cooked;
    de_result result = cook_geometry(&ir, shape, &cooked);
    de__render_preview_blob blob = {0};
    if (result == DE_OK) {
        if (cooked.failed || cooked.size > DE__RENDER_PREVIEW_BYTES_MAX)
            result = DE_ERR_UNSUPPORTED;
        else {
            blob.size = cooked.size;
            blob.data = DE_ALLOC(a, (size_t)blob.size, 16, DE_MEM_RENDER);
            if (!blob.data)
                result = DE_ERR_OUT_OF_MEMORY;
            else
                memcpy(blob.data, cooked.data, (size_t)blob.size);
        }
    }
    de_model_ir_free(&ir);
    if (result == DE_OK)
        *out = blob;
    return result;
}
