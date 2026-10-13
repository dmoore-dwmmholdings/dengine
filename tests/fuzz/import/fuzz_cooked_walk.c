/*
 * fuzz_cooked_walk.c: the cooked-format walkers of the fuzz targets (DENG-106). See
 * fuzz_cooked_walk.h.
 */
#include "fuzz_cooked_walk.h"

#include <dengine/de_base.h>

#include "anim/formats/de_cooked_clip.h"
#include "anim/formats/de_cooked_skeleton.h"
#include "render/formats/de_cooked_env.h"
#include "render/formats/de_cooked_material.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/formats/de_cooked_model.h"
#include "render/formats/de_cooked_texture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Most clip times a walk samples (spread over the table), so a large hostile clip stays fast. */
#define FZ_CLIP_MAX_SAMPLES 48u

/* A broken parser promise: report it and crash, so the fuzzer keeps the input. */
#define FZ_PROMISE(cond, what)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "fuzz_cooked_walk: %s: %s\n", (what), #cond);                          \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

/* Reads values so the compiler cannot drop the loads (and ASan sees them). */
static volatile u32 g_sink;

static void touch_f32(const f32 *v, u32 n) {
    u32 h = 0;
    for (u32 i = 0; i < n; ++i) {
        u32 b;
        memcpy(&b, &v[i], sizeof b);
        h = h * 31u + b;
    }
    g_sink += h;
}

static void touch_bytes(const u8 *p, u64 n) {
    u32 h = 0;
    for (u64 i = 0; i < n; ++i)
        h = h * 31u + p[i];
    g_sink += h;
}

void fz_magic(const void *data, u64 size, char out[5]) {
    memset(out, 0, 5);
    if (data != NULL && size >= 4)
        for (u32 i = 0; i < 4; ++i) {
            char c = ((const char *)data)[i];
            out[i] = c >= 0x20 && c < 0x7F ? c : '?';
        }
}

/* ---- DMSH ------------------------------------------------------------------------------------ */

bool fz_walk_mesh(const void *data, u64 size, const char **why) {
    de_cooked_mesh_view v;
    memset(&v, 0, sizeof v);
    if (!de_cooked_mesh_parse(data, size, &v, why))
        return false;
    FZ_PROMISE(v.vertex_count >= 1u && v.submesh_count >= 1u, "DMSH counts");
    FZ_PROMISE(v.lod_count >= 1u && v.lod_count <= DE_COOKED_MESH_MAX_LODS, "DMSH LOD count");
    FZ_PROMISE(v.index_count % 3u == 0, "DMSH index count");
    FZ_PROMISE(v.streams[DE_COOKED_STREAM_POSITION] != NULL, "DMSH position stream");
    /* Every vertex of every stream. */
    for (u32 i = 0; i < v.vertex_count; ++i) {
        f32 f[4];
        u16 j[4];
        u8 w[4];
        de_cooked_mesh_position(&v, i, f);
        touch_f32(f, 3);
        if (de_cooked_mesh_normal(&v, i, f))
            touch_f32(f, 3);
        if (de_cooked_mesh_tangent(&v, i, f))
            touch_f32(f, 4);
        for (u32 set = 0; set < 2; ++set)
            if (de_cooked_mesh_uv(&v, set, i, f))
                touch_f32(f, 2);
        if (de_cooked_mesh_color(&v, i, f))
            touch_f32(f, 4);
        if (de_cooked_mesh_joints(&v, i, j))
            g_sink += j[0] + j[1] + j[2] + j[3];
        if (de_cooked_mesh_weights(&v, i, w))
            g_sink += (u32)w[0] + w[1] + w[2] + w[3];
    }
    for (u32 s = 0; s < v.submesh_count; ++s) {
        de_cooked_submesh sm = de_cooked_mesh_submesh(&v, s);
        touch_f32(sm.bounds_min, 3);
        touch_f32(sm.bounds_max, 3);
    }
    for (u32 i = 0; i < v.index_count; ++i)
        FZ_PROMISE(de_cooked_mesh_index(&v, i) < v.vertex_count, "DMSH index < V");
    /* LODs, the submesh ranges inside them, and their meshlets. */
    for (u32 l = 0; l < v.lod_count; ++l) {
        de_cooked_mesh_lod lod = de_cooked_mesh_get_lod(&v, l);
        FZ_PROMISE((u64)lod.index_first + lod.index_count <= v.index_count, "DMSH LOD indices");
        FZ_PROMISE((u64)lod.meshlet_first + lod.meshlet_count <= v.meshlet_count,
                   "DMSH LOD meshlets");
        FZ_PROMISE(lod.vertex_count <= v.vertex_count, "DMSH LOD vertex prefix");
        for (u32 s = 0; s < v.submesh_count; ++s) {
            de_cooked_mesh_range r = de_cooked_mesh_lod_submesh(&v, l, s);
            FZ_PROMISE(r.index_first >= lod.index_first &&
                           (u64)r.index_first + r.index_count <=
                               (u64)lod.index_first + lod.index_count,
                       "DMSH LOD-submesh indices");
            FZ_PROMISE(r.meshlet_first >= lod.meshlet_first &&
                           (u64)r.meshlet_first + r.meshlet_count <=
                               (u64)lod.meshlet_first + lod.meshlet_count,
                       "DMSH LOD-submesh meshlets");
        }
    }
    for (u32 m = 0; m < v.meshlet_count; ++m) {
        de_cooked_meshlet ml = de_cooked_mesh_meshlet(&v, m);
        FZ_PROMISE(ml.vertex_count <= DE_COOKED_MESHLET_MAX_VERTICES &&
                       ml.triangle_count <= DE_COOKED_MESHLET_MAX_TRIANGLES,
                   "DMSH meshlet limits");
        FZ_PROMISE(ml.submesh < v.submesh_count, "DMSH meshlet submesh");
        FZ_PROMISE((u64)ml.vertex_offset + ml.vertex_count <= v.meshlet_vertex_count,
                   "DMSH meshlet vertices");
        FZ_PROMISE((u64)ml.triangle_offset + 3u * (u64)ml.triangle_count <=
                       v.meshlet_triangle_bytes,
                   "DMSH meshlet triangles");
        for (u32 k = 0; k < ml.vertex_count; ++k)
            FZ_PROMISE(de_cooked_mesh_meshlet_vertex(&v, ml.vertex_offset + k) < v.vertex_count,
                       "DMSH meshlet vertex < V");
        for (u32 k = 0; k < 3u * ml.triangle_count; ++k)
            FZ_PROMISE(v.meshlet_triangles[ml.triangle_offset + k] < ml.vertex_count,
                       "DMSH meshlet local index");
        touch_f32(ml.center, 3);
        touch_f32(ml.cone_axis, 3);
    }
    return true;
}

/* ---- DMAT ------------------------------------------------------------------------------------ */

bool fz_walk_material(const void *data, u64 size, const char **why) {
    de_cooked_material_view v;
    memset(&v, 0, sizeof v);
    if (!de_cooked_material_parse(data, size, &v, why))
        return false;
    FZ_PROMISE(v.texture_count <= DE_COOKED_TEX_COUNT, "DMAT texture count");
    for (u32 i = 0; i < v.texture_count; ++i) {
        FZ_PROMISE(v.textures[i].slot < DE_COOKED_TEX_COUNT, "DMAT slot");
        FZ_PROMISE(de_cooked_material_texture(&v, v.textures[i].slot) == &v.textures[i],
                   "DMAT slots unique");
    }
    touch_bytes((const u8 *)&v.params, sizeof v.params);
    return true;
}

/* ---- DMDL ------------------------------------------------------------------------------------ */

bool fz_walk_model(const void *data, u64 size, const char **why) {
    de_cooked_model_view v;
    memset(&v, 0, sizeof v);
    if (!de_cooked_model_parse(data, size, &v, why))
        return false;
    for (u32 i = 0; i < v.node_count; ++i) {
        de_cooked_model_node n = de_cooked_model_get_node(&v, i);
        g_sink += (u32)strlen(n.name);
        FZ_PROMISE(n.parent == DE_COOKED_NONE || n.parent < v.node_count, "DMDL parent");
        FZ_PROMISE(n.mesh == DE_COOKED_NONE || n.mesh < v.mesh_count, "DMDL mesh");
        FZ_PROMISE(n.skin == DE_COOKED_NONE || n.skin < v.skeleton_count, "DMDL skin");
        /* No cycle: the parent chain ends within node_count steps. */
        u32 p = n.parent, steps = 0;
        while (p != DE_COOKED_NONE) {
            FZ_PROMISE(++steps <= v.node_count, "DMDL parent cycle");
            p = de_cooked_model_get_node(&v, p).parent;
        }
        touch_f32(n.translation, 3);
        touch_f32(n.rotation, 4);
        touch_f32(n.scale, 3);
    }
    for (u32 i = 0; i < v.root_count; ++i)
        FZ_PROMISE(de_cooked_model_root(&v, i) < v.node_count, "DMDL root");
    u64 h = 0;
    for (u32 i = 0; i < v.mesh_count; ++i)
        h += de_cooked_model_mesh_id(&v, i);
    for (u32 i = 0; i < v.material_count; ++i)
        h += de_cooked_model_material_id(&v, i);
    for (u32 i = 0; i < v.texture_count; ++i)
        h += de_cooked_model_texture_id(&v, i);
    for (u32 i = 0; i < v.skeleton_count; ++i)
        h += de_cooked_model_skeleton_id(&v, i);
    for (u32 i = 0; i < v.clip_count; ++i)
        h += de_cooked_model_clip_id(&v, i);
    g_sink += (u32)h;
    return true;
}

/* ---- DTEX ------------------------------------------------------------------------------------ */

bool fz_walk_texture(const void *data, u64 size, const char **why) {
    de_cooked_texture_view v;
    memset(&v, 0, sizeof v);
    if (!de_cooked_texture_parse(data, size, &v, why))
        return false;
    FZ_PROMISE(v.mip_count >= 1u && v.mip_count <= DE_COOKED_TEXTURE_MAX_MIPS, "DTEX mips");
    FZ_PROMISE((u64)v.depth_or_layers * v.mip_count == v.subresource_count,
               "DTEX subresource count");
    u64 end = 0;
    for (u32 i = 0; i < v.subresource_count; ++i) {
        de_cooked_texture_subresource s;
        FZ_PROMISE(de_cooked_texture_subresource_at(&v, i, &s), "DTEX subresource");
        FZ_PROMISE(s.offset % 512u == 0 && s.row_pitch % 256u == 0, "DTEX alignment");
        FZ_PROMISE(s.size == (u64)s.row_pitch * s.row_count, "DTEX size");
        FZ_PROMISE(s.offset >= end && s.offset <= size && s.size <= size - s.offset,
                   "DTEX subresource range");
        FZ_PROMISE(de_cooked_texture_row_bytes(&v.block, s.width) <= s.row_pitch, "DTEX row pitch");
        end = s.offset + s.size;
        /* First and last row of the subresource: what an upload reads. */
        touch_bytes(v.blob + s.offset, s.row_pitch);
        if (s.row_count > 1u)
            touch_bytes(v.blob + s.offset + s.size - s.row_pitch, s.row_pitch);
    }
    return true;
}

/* ---- Environment (DTEX + ENVS) --------------------------------------------------------------- */

bool fz_walk_env(const void *data, u64 size, const char **why) {
    de_cooked_env_view v;
    memset(&v, 0, sizeof v);
    if (!de_cooked_env_parse(data, size, &v, why))
        return false;
    FZ_PROMISE(v.dtex_size <= size && size - v.dtex_size == DE_COOKED_ENV_TRAILER_SIZE,
               "ENVS position");
    FZ_PROMISE(v.texture.dim == (u32)DE_TEX_CUBE && v.texture.depth_or_layers == 6u, "ENVS cube");
    FZ_PROMISE(v.face_size == v.texture.width && v.mip_count == v.texture.mip_count,
               "ENVS face size and mips");
    /* The DTEX part on its own is a valid DTEX blob. */
    const char *tw = NULL;
    FZ_PROMISE(fz_walk_texture(data, v.dtex_size, &tw), "ENVS DTEX part");
    /* What a shader does with the trailer: the roughness mapping and the irradiance, along every
     * face's centre and corners, through the face table both ways. */
    for (u32 face = 0; face < 6u; ++face)
        for (u32 k = 0; k < 5u; ++k) {
            static const f32 sc[5] = {0.0f, -1.0f, 1.0f, -1.0f, 1.0f};
            static const f32 tc[5] = {0.0f, -1.0f, -1.0f, 1.0f, 1.0f};
            f32 d[3], rgb[3], s = 0.0f, t = 0.0f;
            de_cooked_env_face_direction(face, sc[k], tc[k], d);
            u32 back = de_cooked_env_direction_face(d, &s, &t);
            FZ_PROMISE(back < 6u, "ENVS face table");
            de_cooked_env_irradiance(&v, d[0], d[1], d[2], rgb);
            touch_f32(rgb, 3);
        }
    f32 mips[3] = {de_cooked_env_roughness_to_mip(&v, -1.0f),
                   de_cooked_env_roughness_to_mip(&v, 0.5f),
                   de_cooked_env_roughness_to_mip(&v, 2.0f)};
    touch_f32(mips, 3);
    return true;
}

/* ---- DSKL ------------------------------------------------------------------------------------ */

bool fz_walk_skeleton(const void *data, u64 size, const char **why) {
    de_cooked_skeleton_view v;
    memset(&v, 0, sizeof v);
    if (!de_cooked_skeleton_parse(data, size, &v, why))
        return false;
    for (u32 i = 0; i < v.joint_count; ++i) {
        u16 p = de_cooked_skeleton_parent(&v, i);
        FZ_PROMISE(p == DE_COOKED_SKEL_NO_PARENT || p < i, "DSKL parent before child");
        g_sink += (u32)de_cooked_skeleton_name_hash(&v, i);
        const char *n = de_cooked_skeleton_name(&v, i);
        if (n != NULL)
            g_sink += (u32)strlen(n);
        de_cooked_joint_pose pose = de_cooked_skeleton_rest(&v, i);
        touch_f32(pose.t, 3);
        f32 m[12];
        de_cooked_skeleton_inverse_bind(&v, i, m);
        touch_f32(m, 12);
    }
    for (u32 i = 0; i < v.mesh_joint_count; ++i)
        FZ_PROMISE(de_cooked_skeleton_remap(&v, i) < v.joint_count, "DSKL remap");
    FZ_PROMISE(v.joint_count == 0 || v.root_joint < v.joint_count, "DSKL root joint");
    f32 m[12];
    de_cooked_skeleton_mesh_from_model(&v, m);
    touch_f32(m, 12);
    return true;
}

/* ---- DCLP ------------------------------------------------------------------------------------ */

bool fz_walk_clip(const void *data, u64 size, const void *skel, u64 skel_size, const char **why) {
    de_cooked_clip_view v;
    memset(&v, 0, sizeof v);
    if (!de_cooked_clip_parse(data, size, &v, why))
        return false;
    FZ_PROMISE(v.time_count >= 1u, "DCLP time count");
    for (u32 i = 0; i < v.track_count; ++i) {
        de_cooked_clip_track t = de_cooked_clip_get_track(&v, i);
        FZ_PROMISE(t.joint < v.joint_count, "DCLP track joint");
        FZ_PROMISE(t.components >= 1u && t.components <= 4u, "DCLP components");
        if (t.encoding != DE_COOKED_CLIP_ENC_CONST) {
            FZ_PROMISE(t.key_count >= 1u, "DCLP key count");
            for (u32 k = 0; k < t.key_count; ++k) {
                FZ_PROMISE(de_canim_get_u16(t.times + 2u * k) < v.time_count, "DCLP key time");
                f32 out[4] = {0, 0, 0, 0};
                de_cooked_clip_key_value(&t, k, out);
                touch_f32(out, 4);
            }
        }
    }
    de_cooked_skeleton_view sv;
    memset(&sv, 0, sizeof sv);
    bool have_skel = skel != NULL && de_cooked_skeleton_parse(skel, skel_size, &sv, NULL);
    de_cooked_joint_pose pose[256];
    bool sample_all = have_skel && sv.joint_count == v.joint_count &&
                      sv.joint_count <= (u32)(sizeof pose / sizeof pose[0]);
    u32 step = v.time_count > FZ_CLIP_MAX_SAMPLES ? v.time_count / FZ_CLIP_MAX_SAMPLES : 1u;
    for (u32 ti = 0; ti < v.time_count; ti += step) {
        f32 t0 = de_cooked_clip_time(&v, ti);
        f32 t1 = ti + 1u < v.time_count ? de_cooked_clip_time(&v, ti + 1u) : t0 + 0.5f;
        f32 times[2] = {t0, t0 + (t1 - t0) * 0.5f};
        for (u32 k = 0; k < 2; ++k) {
            for (u32 i = 0; i < v.track_count; ++i) {
                de_cooked_clip_track t = de_cooked_clip_get_track(&v, i);
                f32 out[4] = {0, 0, 0, 0};
                de_cooked_clip_track_sample(&v, &t, times[k], out);
                touch_f32(out, 4);
            }
            if (sample_all) {
                FZ_PROMISE(de__anim_clip_sample(&v, &sv, times[k], pose), "DCLP reference sampler");
                for (u32 j = 0; j < sv.joint_count; ++j)
                    touch_f32(pose[j].r, 4);
            }
        }
    }
    return true;
}
