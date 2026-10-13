/*
 * fbx_import.c: the FBX front end of the model importer (DENG-102, SPEC §11.6, §5.5, §22).
 *
 * ufbx parses the file (binary or ASCII) from memory; this file turns the ufbx scene into the
 * model IR (model_ir.h) that the glTF front end also fills, so both cook to the same formats
 * through model_cook.c. In order:
 *   1. load with ufbx (FBX forced, our allocator with a memory limit, units to metres with
 *      UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY, geometry transforms baked, missing normals
 *      generated, Blender's PBR encoding read), forward ufbx's warnings;
 *   2. the source axes: the file's GlobalSettings, or the `axis_up` / `axis_front` overrides;
 *   3. materials and their textures (fbx_material.c);
 *   4. meshes: each material part becomes one submesh; its faces are triangulated with
 *      ufbx_triangulate_face and every corner's attributes (position, normal, tangent, two UV
 *      sets, one colour set, 4 skin influences) go through an insertion-ordered dedup table, so
 *      vertex order is first use and never hash order; tangents are generated with MikkTSpace for
 *      normal-mapped submeshes when the file has none;
 *   5. nodes (ufbx's local transforms, the root skipped), skins (geometry_to_bone as inverse
 *      binds) and animation stacks (ufbx_bake_anim: linear keys, rotations resampled at the
 *      file's frame rate, starting at 0);
 *   6. the basis change from the source axes to engine axes, applied to all of it.
 *
 * Why the axes are not ufbx's target_axes: ufbx applies an axis change as a rotation of the
 * top-level nodes (adjust_pre_rotation) and leaves the geometry in file axes. The glTF path
 * rotates everything (geometry too, C·M·C⁻¹ for transforms), so the same model would cook to
 * different meshes from the two formats. Doing the basis change here keeps FBX and glTF output
 * the same: a Z-up mesh in engine space, identity root transforms.
 */
#include "fbx_import.h"
#include "fbx_internal.h"

#include "model_internal.h"
#include "model_ir.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "render/formats/de_cooked_material.h"

#include <ufbx.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Limits that keep a hostile file from asking for absurd amounts of memory or time. */
#define FBX_MEMORY_LIMIT ((size_t)1u << 30) /* bytes per ufbx allocator (load, bake) */
#define FBX_MAX_INDICES  (1u << 28)         /* triangle corners per mesh */
#define FBX_MAX_JOINTS   65535u             /* clusters per skin (u16 joint indices) */

/* ---- Allocator ------------------------------------------------------------------------------- */

/* ufbx frees with the size it allocated; a zero size is allocated as one byte. */
static size_t nz(size_t s) {
    return s != 0 ? s : 1u;
}

static void *ufbx_alloc_cb(void *user, size_t size) {
    const de_allocator *a = (const de_allocator *)user;
    return DE_ALLOC(a, nz(size), 16, DE_MEM_ASSET);
}

static void *ufbx_realloc_cb(void *user, void *old, size_t old_size, size_t new_size) {
    const de_allocator *a = (const de_allocator *)user;
    if (old == NULL)
        return DE_ALLOC(a, nz(new_size), 16, DE_MEM_ASSET);
    return DE_REALLOC(a, old, nz(old_size), nz(new_size), 16, DE_MEM_ASSET);
}

static void ufbx_free_cb(void *user, void *ptr, size_t size) {
    const de_allocator *a = (const de_allocator *)user;
    if (ptr != NULL)
        DE_FREE(a, ptr, nz(size));
}

static ufbx_allocator_opts allocator_opts(const de_allocator *a) {
    ufbx_allocator_opts o;
    memset(&o, 0, sizeof o);
    o.allocator.alloc_fn = ufbx_alloc_cb;
    o.allocator.realloc_fn = ufbx_realloc_cb;
    o.allocator.free_fn = ufbx_free_cb;
    o.allocator.user = (void *)(uintptr_t)a;
    o.memory_limit = FBX_MEMORY_LIMIT;
    return o;
}

static de_result ufbx_fail(const de_model_source *src, const ufbx_error *e, const char *stage) {
    de_result r = e->type == UFBX_ERROR_OUT_OF_MEMORY ? DE_ERR_OUT_OF_MEMORY : DE_ERR_PARSE;
    const char *what = e->type == UFBX_ERROR_MEMORY_LIMIT || e->type == UFBX_ERROR_ALLOCATION_LIMIT
                           ? "the file needs more memory than the importer allows"
                           : e->description.data;
    return DE_FAIL(r, "%s: FBX %s: %s%s%.*s", src->path, stage, what, e->info_length ? ": " : "",
                   (int)e->info_length, e->info);
}

/* ---- Source detection ------------------------------------------------------------------------ */

bool de_model_source_is_fbx(const de_model_source *src) {
    static const char magic[] = "Kaydara FBX Binary";
    if (src->data != NULL && src->size >= sizeof magic - 1u &&
        memcmp(src->data, magic, sizeof magic - 1u) == 0)
        return true;
    const char *p = src->path;
    if (p == NULL)
        return false;
    size_t n = strlen(p);
    if (n < 4 || p[n - 4] != '.')
        return false;
    const char *e = p + n - 3;
    return (e[0] == 'f' || e[0] == 'F') && (e[1] == 'b' || e[1] == 'B') &&
           (e[2] == 'x' || e[2] == 'X');
}

/*
 * ufbx reads an ASCII file up to where it ends, so a truncated one loads as part of a scene.
 * A cut inside a node leaves its braces open: count them outside strings and `;` comments.
 * (A cut exactly between two top-level nodes cannot be told from a complete file.)
 */
static bool ascii_truncated(const u8 *p, u64 n) {
    i64 depth = 0;
    bool str = false, comment = false;
    for (u64 i = 0; i < n; ++i) {
        u8 c = p[i];
        if (comment) {
            comment = c != '\n';
        } else if (str) {
            str = c != '"';
        } else if (c == '"') {
            str = true;
        } else if (c == ';') {
            comment = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
        }
    }
    return depth != 0 || str;
}

/* ---- Axes ------------------------------------------------------------------------------------ */

static u32 ax_index(ufbx_coordinate_axis a) {
    return (u32)a / 2u;
}

static i32 ax_sign(ufbx_coordinate_axis a) {
    return ((u32)a & 1u) ? -1 : 1;
}

static ufbx_coordinate_axis ax_make(u32 index, i32 sign) {
    return (ufbx_coordinate_axis)(index * 2u + (sign < 0 ? 1u : 0u));
}

/* "x", "+y", "-Z", ... → an axis; false for anything else. */
static bool parse_axis(const char *s, ufbx_coordinate_axis *out) {
    i32 sign = 1;
    if (*s == '+' || *s == '-')
        sign = *s++ == '-' ? -1 : 1;
    char c = *s;
    if (c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');
    if ((c != 'x' && c != 'y' && c != 'z') || s[1] != '\0')
        return false;
    *out = ax_make((u32)(c - 'x'), sign);
    return true;
}

static bool is_auto(const char *s) {
    return s == NULL || s[0] == '\0' || strcmp(s, "auto") == 0;
}

/* a × b of two signed unit axes (perpendicular). */
static ufbx_coordinate_axis ax_cross(ufbx_coordinate_axis a, ufbx_coordinate_axis b) {
    i32 va[3] = {0, 0, 0}, vb[3] = {0, 0, 0};
    va[ax_index(a)] = ax_sign(a);
    vb[ax_index(b)] = ax_sign(b);
    i32 c[3] = {va[1] * vb[2] - va[2] * vb[1], va[2] * vb[0] - va[0] * vb[2],
                va[0] * vb[1] - va[1] * vb[0]};
    for (u32 i = 0; i < 3; ++i)
        if (c[i] != 0)
            return ax_make(i, c[i]);
    return UFBX_COORDINATE_AXIS_UNKNOWN;
}

/* The usual front of a file with this up axis: +Z for X- and Y-up (Maya), -Y for Z-up (Max). */
static ufbx_coordinate_axis default_front(ufbx_coordinate_axis up) {
    return ax_index(up) == 2u ? UFBX_COORDINATE_AXIS_NEGATIVE_Y : UFBX_COORDINATE_AXIS_POSITIVE_Z;
}

static bool axes_basis(const ufbx_coordinate_axes *a) {
    if (!ufbx_coordinate_axes_valid(*a))
        return false;
    u32 r = ax_index(a->right), u = ax_index(a->up), f = ax_index(a->front);
    return r != u && u != f && r != f;
}

/* The source axes: the file's, or the overrides. DE_ERR_INVALID_ARGUMENT for a bad override. */
static de_result source_axes(de__fbx_ctx *f, const de_model_fbx_options *opt,
                             ufbx_coordinate_axes *out) {
    ufbx_coordinate_axes file = f->scene->settings.axes;
    if (!axes_basis(&file)) {
        de_model_ir_warn(f->ir, "the file has no valid axis settings; assuming Y-up (right +X, "
                                "up +Y, front +Z)");
        file.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
        file.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
        file.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;
    }
    const char *up_s = opt != NULL ? opt->axis_up : NULL;
    const char *front_s = opt != NULL ? opt->axis_front : NULL;
    if (is_auto(up_s) && is_auto(front_s)) {
        *out = file;
        return DE_OK;
    }
    ufbx_coordinate_axis up = file.up, front = file.front;
    if (!is_auto(up_s) && !parse_axis(up_s, &up))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "%s: setting `axis_up` must be \"auto\", \"x\", \"y\" or \"z\" (optionally "
                       "signed), got \"%s\"",
                       f->src->path, up_s);
    if (!is_auto(front_s)) {
        if (!parse_axis(front_s, &front))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "%s: setting `axis_front` must be \"auto\" or a signed axis such as "
                           "\"+y\" or \"-z\", got \"%s\"",
                           f->src->path, front_s);
        if (ax_index(front) == ax_index(up))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "%s: setting `axis_front` (\"%s\") is parallel to the up axis",
                           f->src->path, front_s);
    } else if (ax_index(front) == ax_index(up)) {
        front = default_front(up);
    }
    out->up = up;
    out->front = front;
    out->right = ax_cross(up, front); /* right-handed: right = up × front */
    return DE_OK;
}

/*
 * C (row-major, c[row][col]) maps the source axes onto engine axes: right → +X, up → +Z,
 * front → -Y (ufbx's "front" is the side a model shows the viewer; the DENG-2 rule maps glTF's
 * +Z front to -Y). C is a signed permutation; det(C) = -1 for a left-handed source.
 */
static void axes_matrix(const ufbx_coordinate_axes *src, f64 c[3][3]) {
    memset(c, 0, sizeof(f64) * 9);
    const ufbx_coordinate_axis from[3] = {src->right, src->up, src->front};
    const ufbx_coordinate_axis to[3] = {UFBX_COORDINATE_AXIS_POSITIVE_X,
                                        UFBX_COORDINATE_AXIS_POSITIVE_Z,
                                        UFBX_COORDINATE_AXIS_NEGATIVE_Y};
    for (u32 k = 0; k < 3; ++k)
        c[ax_index(to[k])][ax_index(from[k])] = (f64)(ax_sign(to[k]) * ax_sign(from[k]));
}

static f64 det3(f64 c[3][3]) {
    return c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) -
           c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0]) +
           c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0]);
}

/* ---- Basis change ---------------------------------------------------------------------------- */

/* v' = s · C · v. C is a signed permutation, so this only moves and negates components. */
static void xf_vec(f64 c[3][3], f64 s, f32 *v) {
    f64 in[3] = {v[0], v[1], v[2]};
    for (u32 i = 0; i < 3; ++i)
        v[i] = (f32)(s * (c[i][0] * in[0] + c[i][1] * in[1] + c[i][2] * in[2]));
}

static void xf_scale(f64 c[3][3], f32 *s) {
    f32 in[3] = {s[0], s[1], s[2]};
    for (u32 i = 0; i < 3; ++i)
        for (u32 j = 0; j < 3; ++j)
            if (c[i][j] != 0.0)
                s[i] = in[j];
}

/* M' = C·M·Cᵀ for a column-major affine 4x4. */
static void xf_mat4(f64 c[3][3], f32 m[16]) {
    f64 a[3][3], r[3][3];
    for (u32 col = 0; col < 3; ++col)
        for (u32 row = 0; row < 3; ++row)
            a[row][col] = m[4u * col + row];
    for (u32 i = 0; i < 3; ++i)
        for (u32 j = 0; j < 3; ++j) {
            f64 acc = 0.0;
            for (u32 k = 0; k < 3; ++k)
                for (u32 l = 0; l < 3; ++l)
                    acc += c[i][k] * a[k][l] * c[j][l];
            r[i][j] = acc;
        }
    for (u32 col = 0; col < 3; ++col)
        for (u32 row = 0; row < 3; ++row)
            m[4u * col + row] = (f32)r[row][col];
    xf_vec(c, 1.0, m + 12);
}

/*
 * Applies the basis change to the whole IR. For a rotation C (det +1) this is what
 * de_model_ir_convert_axes does for glTF. A mirror (det -1, left-handed source) also flips each
 * triangle's winding and the tangents' w, and a rotation's axis turns as a pseudovector
 * (q.xyz → det·C·q.xyz).
 */
static void convert_basis(de_model_ir *ir, f64 c[3][3]) {
    f64 det = det3(c);
    for (u32 m = 0; m < ir->mesh_count; ++m) {
        de_model_mesh *mesh = &ir->meshes[m];
        for (u32 v = 0; v < mesh->vertex_count; ++v) {
            xf_vec(c, 1.0, mesh->position + 3u * v);
            if (mesh->normal != NULL)
                xf_vec(c, 1.0, mesh->normal + 3u * v);
            if (mesh->tangent != NULL) {
                xf_vec(c, 1.0, mesh->tangent + 4u * v);
                mesh->tangent[4u * v + 3u] *= (f32)det;
            }
        }
        if (det < 0.0)
            for (u32 i = 0; i + 2u < mesh->index_count; i += 3u) {
                u32 t = mesh->indices[i + 1u];
                mesh->indices[i + 1u] = mesh->indices[i + 2u];
                mesh->indices[i + 2u] = t;
            }
    }
    for (u32 n = 0; n < ir->node_count; ++n) {
        de_model_node *node = &ir->nodes[n];
        xf_vec(c, 1.0, node->translation);
        xf_vec(c, det, node->rotation);
        xf_scale(c, node->scale);
    }
    for (u32 k = 0; k < ir->skin_count; ++k)
        for (u32 j = 0; j < ir->skins[k].joint_count; ++j)
            xf_mat4(c, ir->skins[k].inverse_bind + 16u * j);
    for (u32 a = 0; a < ir->anim_count; ++a)
        for (u32 ch = 0; ch < ir->anims[a].channel_count; ++ch) {
            de_model_anim_channel *chan = &ir->anims[a].channels[ch];
            for (u32 k = 0; k < chan->key_count; ++k) {
                f32 *x = chan->values + (u64)k * chan->components;
                if (chan->path == DE_MODEL_ANIM_TRANSLATION)
                    xf_vec(c, 1.0, x);
                else if (chan->path == DE_MODEL_ANIM_ROTATION)
                    xf_vec(c, det, x);
                else if (chan->path == DE_MODEL_ANIM_SCALE)
                    xf_scale(c, x);
            }
        }
}

/* ---- Meshes ---------------------------------------------------------------------------------- */

/* One corner's attributes, the dedup key: compared and hashed as bytes (no padding). */
typedef struct fbx_vertex {
    f32 pos[3], nrm[3], tan[4], uv[2][2], col[4], wgt[4];
    u16 jnt[4];
} fbx_vertex;

DE_STATIC_ASSERT(sizeof(fbx_vertex) == 22u * 4u + 4u * 2u, "fbx_vertex must have no padding");

typedef struct mesh_build {
    const ufbx_mesh *src;
    u32 attr_mask, uv_sets;
    const ufbx_skin_deformer *skin;
    bool warned_weights;
    fbx_vertex *verts; /* unique vertices of the mesh, in first-use order */
    u32 vert_count;
    u32 *table; /* dedup slots: vertex index + 1, 0 = empty */
    u32 table_cap;
} mesh_build;

static void put3(f32 *d, ufbx_vec3 v) {
    d[0] = (f32)v.x;
    d[1] = (f32)v.y;
    d[2] = (f32)v.z;
}

static void corner(de__fbx_ctx *f, mesh_build *b, u32 ix, fbx_vertex *v) {
    const ufbx_mesh *m = b->src;
    memset(v, 0, sizeof *v);
    put3(v->pos, ufbx_get_vertex_vec3(&m->vertex_position, ix));
    if (b->attr_mask & DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_NORMAL))
        put3(v->nrm, ufbx_get_vertex_vec3(&m->vertex_normal, ix));
    for (u32 s = 0; s < b->uv_sets; ++s) {
        ufbx_vec2 uv = ufbx_get_vertex_vec2(&m->uv_sets.data[s].vertex_uv, ix);
        v->uv[s][0] = (f32)uv.x;
        v->uv[s][1] = (f32)(1.0 - uv.y); /* FBX: origin bottom-left; IR (glTF): top-left */
    }
    if (b->attr_mask & DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_TANGENT)) {
        const ufbx_uv_set *set = &m->uv_sets.data[0];
        ufbx_vec3 t = ufbx_get_vertex_vec3(&set->vertex_tangent, ix);
        ufbx_vec3 bt = ufbx_get_vertex_vec3(&set->vertex_bitangent, ix);
        f64 n[3] = {v->nrm[0], v->nrm[1], v->nrm[2]};
        f64 cx = n[1] * t.z - n[2] * t.y, cy = n[2] * t.x - n[0] * t.z,
            cz = n[0] * t.y - n[1] * t.x;
        put3(v->tan, t);
        /* The bitangent follows +v; flipping v flips it, hence the minus. */
        v->tan[3] = cx * bt.x + cy * bt.y + cz * bt.z < 0.0 ? 1.0f : -1.0f;
    }
    if (b->attr_mask & DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_COLOR0)) {
        ufbx_vec4 c = ufbx_get_vertex_vec4(&m->color_sets.data[0].vertex_color, ix);
        v->col[0] = (f32)c.x;
        v->col[1] = (f32)c.y;
        v->col[2] = (f32)c.z;
        v->col[3] = (f32)c.w;
    }
    if (b->skin != NULL) {
        u32 cp = m->vertex_indices.data[ix];
        f64 sum = 0.0;
        u32 n = 0;
        if (cp < b->skin->vertices.count) {
            const ufbx_skin_vertex *sv = &b->skin->vertices.data[cp];
            /* ufbx sorts a vertex's weights by decreasing weight: the first 4 are the largest. */
            for (u32 k = 0; k < sv->num_weights && n < 4u; ++k) {
                const ufbx_skin_weight *w = &b->skin->weights.data[sv->weight_begin + k];
                if (!(w->weight > 0.0) || !isfinite(w->weight))
                    continue;
                v->jnt[n] = (u16)w->cluster_index;
                v->wgt[n] = (f32)w->weight;
                sum += w->weight;
                ++n;
            }
        }
        if (n == 0 || !(sum > 0.0)) {
            if (!b->warned_weights)
                de_model_ir_warn(f->ir,
                                 "mesh \"%s\": vertices without skin weights; bound to joint 0",
                                 m->name.data);
            b->warned_weights = true;
            memset(v->jnt, 0, sizeof v->jnt);
            memset(v->wgt, 0, sizeof v->wgt);
            v->wgt[0] = 1.0f;
        } else {
            for (u32 k = 0; k < n; ++k)
                v->wgt[k] = (f32)((f64)v->wgt[k] / sum);
        }
    }
}

/* The index of `v` among the vertices of the current submesh, added on first use. */
static u32 dedup(mesh_build *b, const fbx_vertex *v) {
    u64 h = de_hash64(v, sizeof *v, 0);
    u32 mask = b->table_cap - 1u;
    for (u32 i = (u32)h & mask;; i = (i + 1u) & mask) {
        u32 s = b->table[i];
        if (s == 0) {
            b->verts[b->vert_count] = *v;
            b->table[i] = ++b->vert_count;
            return b->vert_count - 1u;
        }
        if (memcmp(&b->verts[s - 1u], v, sizeof *v) == 0)
            return s - 1u;
    }
}

static bool material_has_normal_map(const de_model_ir *ir, i32 material) {
    return material >= 0 && ir->materials[material].tex[DE_COOKED_TEX_NORMAL].texture >= 0;
}

/* Copies the unique vertices into the IR mesh's streams. */
static de_result emit_streams(de_model_ir *ir, const mesh_build *b, de_model_mesh *out) {
    u64 n = b->vert_count;
    u32 mask = out->attr_mask;
    out->position = (f32 *)de_model_ir_array(ir, n * 3u + 1u, sizeof(f32));
    if (out->position == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    if (mask & DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_NORMAL) &&
        (out->normal = (f32 *)de_model_ir_array(ir, n * 3u + 1u, sizeof(f32))) == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    if (mask & DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_TANGENT) &&
        (out->tangent = (f32 *)de_model_ir_array(ir, n * 4u + 1u, sizeof(f32))) == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 s = 0; s < 2; ++s)
        if (mask & DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_UV0 + s) &&
            (out->uv[s] = (f32 *)de_model_ir_array(ir, n * 2u + 1u, sizeof(f32))) == NULL)
            return DE_ERR_OUT_OF_MEMORY;
    if (mask & DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_COLOR0) &&
        (out->color0 = (f32 *)de_model_ir_array(ir, n * 4u + 1u, sizeof(f32))) == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    if (mask & DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_JOINTS0)) {
        out->joints0 = (u16 *)de_model_ir_array(ir, n * 4u + 1u, sizeof(u16));
        out->weights0 = (f32 *)de_model_ir_array(ir, n * 4u + 1u, sizeof(f32));
        if (out->joints0 == NULL || out->weights0 == NULL)
            return DE_ERR_OUT_OF_MEMORY;
    }
    for (u64 i = 0; i < n; ++i) {
        const fbx_vertex *v = &b->verts[i];
        memcpy(out->position + 3u * i, v->pos, sizeof v->pos);
        if (out->normal != NULL)
            memcpy(out->normal + 3u * i, v->nrm, sizeof v->nrm);
        if (out->tangent != NULL)
            memcpy(out->tangent + 4u * i, v->tan, sizeof v->tan);
        for (u32 s = 0; s < 2; ++s)
            if (out->uv[s] != NULL)
                memcpy(out->uv[s] + 2u * i, v->uv[s], sizeof v->uv[s]);
        if (out->color0 != NULL)
            memcpy(out->color0 + 4u * i, v->col, sizeof v->col);
        if (out->joints0 != NULL) {
            memcpy(out->joints0 + 4u * i, v->jnt, sizeof v->jnt);
            memcpy(out->weights0 + 4u * i, v->wgt, sizeof v->wgt);
        }
    }
    return DE_OK;
}

/* MikkTSpace tangents for the normal-mapped submeshes, when the file gave none. */
static de_result generate_tangents(de_model_ir *ir, de_model_mesh *out) {
    if (out->tangent != NULL || out->normal == NULL || out->uv[0] == NULL)
        return DE_OK;
    bool any = false;
    for (u32 s = 0; s < out->submesh_count; ++s)
        any = any || material_has_normal_map(ir, out->submeshes[s].material);
    if (!any)
        return DE_OK;
    out->tangent = (f32 *)de_model_ir_array(ir, (u64)out->vertex_count * 4u + 1u, sizeof(f32));
    if (out->tangent == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 i = 0; i < out->vertex_count; ++i) { /* (1, 0, 0, +1) where nothing is generated */
        out->tangent[4u * i] = 1.0f;
        out->tangent[4u * i + 3u] = 1.0f;
    }
    out->attr_mask |= DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_TANGENT);
    for (u32 s = 0; s < out->submesh_count; ++s) {
        de_model_submesh *sm = &out->submeshes[s];
        if (!material_has_normal_map(ir, sm->material))
            continue;
        if (!de_model_generate_tangents(out->position, out->normal, out->uv[0], out->tangent,
                                        out->indices + sm->index_first, sm->index_count))
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory generating tangents");
        sm->attr_mask |= DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_TANGENT);
    }
    return DE_OK;
}

static de_result import_mesh(de__fbx_ctx *f, const ufbx_mesh *m, de_model_mesh *out) {
    de_model_ir *ir = f->ir;
    const de_allocator *a = ir->alloc;
    out->name = de_model_ir_strdup(ir, m->name.data);
    if (out->name == NULL)
        return DE_ERR_OUT_OF_MEMORY;

    mesh_build b;
    memset(&b, 0, sizeof b);
    b.src = m;
    b.attr_mask = DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_POSITION);
    if (m->vertex_normal.exists)
        b.attr_mask |= DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_NORMAL);
    b.uv_sets = m->uv_sets.count < 2u ? (u32)m->uv_sets.count : 2u;
    if (m->uv_sets.count > 2u)
        de_model_ir_warn(ir, "mesh \"%s\": %u UV sets; only the first 2 are imported", out->name,
                         (unsigned)m->uv_sets.count);
    for (u32 s = 0; s < b.uv_sets; ++s)
        b.attr_mask |= DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_UV0 + s);
    if (b.uv_sets > 0 && m->vertex_normal.exists && m->uv_sets.data[0].vertex_tangent.exists &&
        m->uv_sets.data[0].vertex_bitangent.exists)
        b.attr_mask |= DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_TANGENT);
    if (m->color_sets.count > 0) {
        b.attr_mask |= DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_COLOR0);
        if (m->color_sets.count > 1)
            de_model_ir_warn(ir, "mesh \"%s\": %u colour sets; only the first is imported",
                             out->name, (unsigned)m->color_sets.count);
    }
    if (m->skin_deformers.count > 0) {
        b.skin = m->skin_deformers.data[0];
        b.attr_mask |=
            DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_JOINTS0) | DE_MODEL_ATTR_BIT(DE_MODEL_ATTR_WEIGHTS0);
        if (m->skin_deformers.count > 1)
            de_model_ir_warn(ir, "mesh \"%s\": %u skin deformers; only the first is imported",
                             out->name, (unsigned)m->skin_deformers.count);
    }
    if (m->blend_deformers.count > 0) {
        for (size_t i = 0; i < m->blend_deformers.count; ++i)
            out->morph_target_count += (u32)m->blend_deformers.data[i]->channels.count;
        de_model_ir_warn(ir, "mesh \"%s\": blend shapes are not imported", out->name);
    }

    /* Corners per part and in total; the largest part sizes the dedup table. */
    u64 corners = 0, part_max = 0;
    u32 parts = 0;
    for (size_t p = 0; p < m->material_parts.count; ++p) {
        u64 c = (u64)m->material_parts.data[p].num_triangles * 3u;
        corners += c;
        if (c > part_max)
            part_max = c;
        if (c != 0)
            ++parts;
    }
    if (corners > FBX_MAX_INDICES)
        return DE_FAIL(DE_ERR_PARSE, "%s: mesh \"%s\" is too large (%llu triangle corners)",
                       f->src->path, out->name, (unsigned long long)corners);
    if (parts == 0) {
        de_model_ir_warn(ir, "mesh \"%s\" has no triangles", out->name);
        return DE_OK;
    }
    out->attr_mask = b.attr_mask;
    out->submesh_count = parts;
    out->index_count = (u32)corners;
    out->submeshes = (de_model_submesh *)de_model_ir_array(ir, parts, sizeof(de_model_submesh));
    out->indices = (u32 *)de_model_ir_array(ir, corners, sizeof(u32));
    if (out->submeshes == NULL || out->indices == NULL)
        return DE_ERR_OUT_OF_MEMORY;

    /* Scratch: the unique vertices (at most one per corner), the dedup table, one face's
     * triangles. Freed below whatever happens. */
    b.table_cap = 16u;
    while ((u64)b.table_cap < part_max * 2u)
        b.table_cap *= 2u;
    size_t tri_cap = m->max_face_triangles * 3u + 3u;
    size_t verts_bytes = (size_t)corners * sizeof(fbx_vertex);
    size_t table_bytes = (size_t)b.table_cap * sizeof(u32);
    size_t tri_bytes = tri_cap * sizeof(u32);
    b.verts = (fbx_vertex *)DE_ALLOC(a, verts_bytes, 16, DE_MEM_ASSET);
    b.table = (u32 *)DE_ALLOC(a, table_bytes, 16, DE_MEM_ASSET);
    u32 *tri = (u32 *)DE_ALLOC(a, tri_bytes, 16, DE_MEM_ASSET);
    de_result r = DE_OK;
    if (b.verts == NULL || b.table == NULL || tri == NULL)
        r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for mesh \"%s\"", out->name);
    if (r == DE_OK && b.skin != NULL && b.skin->clusters.count > FBX_MAX_JOINTS)
        r = DE_FAIL(DE_ERR_PARSE, "%s: mesh \"%s\": its skin has %u joints (at most %u)",
                    f->src->path, out->name, (unsigned)b.skin->clusters.count, FBX_MAX_JOINTS);

    u32 sub = 0, idx = 0;
    for (size_t p = 0; p < m->material_parts.count && r == DE_OK; ++p) {
        const ufbx_mesh_part *part = &m->material_parts.data[p];
        if (part->num_triangles == 0)
            continue;
        memset(b.table, 0, table_bytes);
        de_model_submesh *sm = &out->submeshes[sub++];
        sm->index_first = idx;
        sm->vertex_first = b.vert_count;
        sm->material =
            part->index < m->materials.count ? (i32)m->materials.data[part->index]->typed_id : -1;
        sm->attr_mask = b.attr_mask;
        for (size_t fi = 0; fi < part->face_indices.count; ++fi) {
            ufbx_face face = m->faces.data[part->face_indices.data[fi]];
            u32 nt = ufbx_triangulate_face(tri, tri_cap, m, face);
            for (u32 k = 0; k < nt * 3u && idx < corners; ++k) {
                fbx_vertex v;
                corner(f, &b, tri[k], &v);
                out->indices[idx++] = dedup(&b, &v);
            }
        }
        sm->index_count = idx - sm->index_first;
        sm->vertex_count = b.vert_count - sm->vertex_first;
    }
    out->index_count = idx;
    out->vertex_count = b.vert_count;
    if (r == DE_OK)
        r = emit_streams(ir, &b, out);
    if (b.verts != NULL)
        DE_FREE(a, b.verts, verts_bytes);
    if (b.table != NULL)
        DE_FREE(a, b.table, table_bytes);
    if (tri != NULL)
        DE_FREE(a, tri, tri_bytes);
    DE_TRY(r);
    return generate_tangents(ir, out);
}

/* ---- Nodes, skins, animations ---------------------------------------------------------------- */

static de_result import_nodes(de__fbx_ctx *f, const u32 *node_map) {
    const ufbx_scene *sc = f->scene;
    de_model_ir *ir = f->ir;
    ir->roots = (u32 *)de_model_ir_array(ir, ir->node_count + 1u, sizeof(u32));
    if (ir->roots == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < sc->nodes.count; ++i) {
        const ufbx_node *s = sc->nodes.data[i];
        if (s->is_root)
            continue;
        de_model_node *n = &ir->nodes[node_map[i]];
        n->name = de_model_ir_strdup(ir, s->name.data);
        if (n->name == NULL)
            return DE_ERR_OUT_OF_MEMORY;
        n->parent =
            s->parent != NULL && !s->parent->is_root ? (i32)node_map[s->parent->typed_id] : -1;
        n->mesh = s->mesh != NULL ? (i32)s->mesh->typed_id : -1;
        n->skin = s->mesh != NULL && s->mesh->skin_deformers.count > 0
                      ? (i32)s->mesh->skin_deformers.data[0]->typed_id
                      : -1;
        const ufbx_transform *t = &s->local_transform;
        n->translation[0] = (f32)t->translation.x;
        n->translation[1] = (f32)t->translation.y;
        n->translation[2] = (f32)t->translation.z;
        n->rotation[0] = (f32)t->rotation.x;
        n->rotation[1] = (f32)t->rotation.y;
        n->rotation[2] = (f32)t->rotation.z;
        n->rotation[3] = (f32)t->rotation.w;
        n->scale[0] = (f32)t->scale.x;
        n->scale[1] = (f32)t->scale.y;
        n->scale[2] = (f32)t->scale.z;
        n->in_scene = true; /* ufbx parents every node under the root */
        if (n->parent < 0)
            ir->roots[ir->root_count++] = node_map[i];
    }
    return DE_OK;
}

static de_result import_skins(de__fbx_ctx *f, const u32 *node_map) {
    const ufbx_scene *sc = f->scene;
    de_model_ir *ir = f->ir;
    for (u32 i = 0; i < ir->skin_count; ++i) {
        const ufbx_skin_deformer *s = sc->skin_deformers.data[i];
        de_model_skin *k = &ir->skins[i];
        k->name = de_model_ir_strdup(ir, s->name.data);
        k->joint_count = (u32)s->clusters.count;
        k->joints = (u32 *)de_model_ir_array(ir, k->joint_count + 1u, sizeof(u32));
        k->inverse_bind = (f32 *)de_model_ir_array(ir, (u64)k->joint_count * 16u + 1u, 4);
        if (k->name == NULL || k->joints == NULL || k->inverse_bind == NULL)
            return DE_ERR_OUT_OF_MEMORY;
        k->skeleton_root = -1;
        for (u32 j = 0; j < k->joint_count; ++j) {
            const ufbx_skin_cluster *c = s->clusters.data[j];
            if (c->bone_node == NULL || c->bone_node->is_root)
                return DE_FAIL(DE_ERR_PARSE, "%s: skin \"%s\": cluster %u has no bone",
                               f->src->path, k->name, j);
            k->joints[j] = node_map[c->bone_node->typed_id];
            /* geometry_to_bone: mesh geometry → bone space at bind time, the inverse bind
             * matrix (column-major 3x4 in ufbx). */
            f32 *ib = k->inverse_bind + 16u * j;
            for (u32 col = 0; col < 4; ++col) {
                ib[4u * col + 0u] = (f32)c->geometry_to_bone.cols[col].x;
                ib[4u * col + 1u] = (f32)c->geometry_to_bone.cols[col].y;
                ib[4u * col + 2u] = (f32)c->geometry_to_bone.cols[col].z;
            }
            ib[15] = 1.0f;
        }
    }
    return DE_OK;
}

static de_result channel(de_model_ir *ir, de_model_anim *an, u32 node, de_model_anim_path path,
                         const void *keys, size_t count) {
    if (count == 0)
        return DE_OK;
    de_model_anim_channel *ch = &an->channels[an->channel_count++];
    ch->node = node;
    ch->path = path;
    ch->interp = DE_MODEL_INTERP_LINEAR;
    ch->key_count = (u32)count;
    ch->components = path == DE_MODEL_ANIM_ROTATION ? 4u : 3u;
    ch->times = (f32 *)de_model_ir_array(ir, count, sizeof(f32));
    ch->values = (f32 *)de_model_ir_array(ir, (u64)count * ch->components, sizeof(f32));
    if (ch->times == NULL || ch->values == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    for (size_t k = 0; k < count; ++k) {
        f32 *v = ch->values + k * ch->components;
        f64 t;
        if (path == DE_MODEL_ANIM_ROTATION) {
            const ufbx_baked_quat *q = (const ufbx_baked_quat *)keys + k;
            t = q->time;
            v[0] = (f32)q->value.x;
            v[1] = (f32)q->value.y;
            v[2] = (f32)q->value.z;
            v[3] = (f32)q->value.w;
        } else {
            const ufbx_baked_vec3 *p = (const ufbx_baked_vec3 *)keys + k;
            t = p->time;
            put3(v, p->value);
        }
        ch->times[k] = (f32)t;
        if (ch->times[k] > an->duration)
            an->duration = ch->times[k];
    }
    return DE_OK;
}

static de_result import_anims(de__fbx_ctx *f, const u32 *node_map) {
    const ufbx_scene *sc = f->scene;
    de_model_ir *ir = f->ir;
    f64 fps = sc->settings.frames_per_second;
    if (!(fps >= 1.0) || !(fps <= 1000.0))
        fps = 30.0;
    for (u32 i = 0; i < ir->anim_count; ++i) {
        const ufbx_anim_stack *st = sc->anim_stacks.data[i];
        de_model_anim *an = &ir->anims[i];
        an->name = de_model_ir_strdup(ir, st->name.data);
        if (an->name == NULL)
            return DE_ERR_OUT_OF_MEMORY;
        ufbx_bake_opts bo;
        memset(&bo, 0, sizeof bo);
        bo.temp_allocator = allocator_opts(ir->alloc);
        bo.result_allocator = allocator_opts(ir->alloc);
        bo.trim_start_time = true;
        bo.resample_rate = fps;
        ufbx_error err;
        ufbx_baked_anim *baked = ufbx_bake_anim(sc, st->anim, &bo, &err);
        if (baked == NULL) {
            char stage[96];
            snprintf(stage, sizeof stage, "animation \"%.64s\"", an->name);
            return ufbx_fail(f->src, &err, stage);
        }
        de_result r = DE_OK;
        an->channels = (de_model_anim_channel *)de_model_ir_array(
            ir, (u64)baked->nodes.count * 3u + 1u, sizeof(de_model_anim_channel));
        if (an->channels == NULL)
            r = DE_ERR_OUT_OF_MEMORY;
        for (size_t k = 0; k < baked->nodes.count && r == DE_OK; ++k) {
            const ufbx_baked_node *bn = &baked->nodes.data[k];
            if (bn->typed_id >= sc->nodes.count || sc->nodes.data[bn->typed_id]->is_root)
                continue;
            u32 node = node_map[bn->typed_id];
            r = channel(ir, an, node, DE_MODEL_ANIM_TRANSLATION, bn->translation_keys.data,
                        bn->translation_keys.count);
            if (r == DE_OK)
                r = channel(ir, an, node, DE_MODEL_ANIM_ROTATION, bn->rotation_keys.data,
                            bn->rotation_keys.count);
            if (r == DE_OK)
                r = channel(ir, an, node, DE_MODEL_ANIM_SCALE, bn->scale_keys.data,
                            bn->scale_keys.count);
        }
        ufbx_free_baked_anim(baked);
        DE_TRY(r);
    }
    return DE_OK;
}

/* ---- Entry ----------------------------------------------------------------------------------- */

static de_result load(de__fbx_ctx *f, const de_model_fbx_options *opt) {
    const ufbx_scene *sc = f->scene;
    de_model_ir *ir = f->ir;
    for (size_t i = 0; i < sc->metadata.warnings.count; ++i) {
        const ufbx_warning *w = &sc->metadata.warnings.data[i];
        if (w->count > 1)
            de_model_ir_warn(ir, "FBX: %s (%u times)", w->description.data, (unsigned)w->count);
        else
            de_model_ir_warn(ir, "FBX: %s", w->description.data);
    }
    ufbx_coordinate_axes axes = {UFBX_COORDINATE_AXIS_POSITIVE_X, UFBX_COORDINATE_AXIS_POSITIVE_Y,
                                 UFBX_COORDINATE_AXIS_POSITIVE_Z};
    DE_TRY(source_axes(f, opt, &axes));

    /* ufbx node typed_id → IR node index (the root is not an IR node). */
    u32 *node_map = (u32 *)de_model_ir_array(ir, sc->nodes.count + 1u, sizeof(u32));
    if (node_map == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    u32 nodes = 0;
    for (size_t i = 0; i < sc->nodes.count; ++i)
        node_map[i] = sc->nodes.data[i]->is_root ? UINT32_MAX : nodes++;

    ir->node_count = nodes;
    ir->mesh_count = (u32)sc->meshes.count;
    ir->material_count = (u32)sc->materials.count;
    ir->skin_count = (u32)sc->skin_deformers.count;
    ir->anim_count = (u32)sc->anim_stacks.count;
    f->texture_cap = ((u32)sc->texture_files.count + (u32)sc->textures.count) * 3u + 1u;
    ir->nodes = (de_model_node *)de_model_ir_array(ir, nodes + 1u, sizeof(de_model_node));
    ir->meshes = (de_model_mesh *)de_model_ir_array(ir, ir->mesh_count + 1u, sizeof(de_model_mesh));
    ir->materials = (de_model_material *)de_model_ir_array(ir, ir->material_count + 1u,
                                                           sizeof(de_model_material));
    ir->textures =
        (de_model_texture *)de_model_ir_array(ir, f->texture_cap, sizeof(de_model_texture));
    ir->skins = (de_model_skin *)de_model_ir_array(ir, ir->skin_count + 1u, sizeof(de_model_skin));
    ir->anims = (de_model_anim *)de_model_ir_array(ir, ir->anim_count + 1u, sizeof(de_model_anim));
    if (ir->nodes == NULL || ir->meshes == NULL || ir->materials == NULL || ir->textures == NULL ||
        ir->skins == NULL || ir->anims == NULL)
        return DE_ERR_OUT_OF_MEMORY;

    for (u32 i = 0; i < ir->material_count; ++i)
        DE_TRY(de__fbx_material(f, sc->materials.data[i], &ir->materials[i]));
    for (u32 i = 0; i < ir->mesh_count; ++i)
        DE_TRY(import_mesh(f, sc->meshes.data[i], &ir->meshes[i]));
    DE_TRY(import_nodes(f, node_map));
    DE_TRY(import_skins(f, node_map));
    DE_TRY(import_anims(f, node_map));

    f64 c[3][3];
    axes_matrix(&axes, c);
    if (det3(c) < 0.0)
        de_model_ir_warn(ir, "the source axes are left-handed; the model is mirrored into the "
                             "engine's right-handed axes");
    convert_basis(ir, c);
    return DE_OK;
}

de_result de_model_fbx_load(const de_model_source *src, const de_model_fbx_options *opt,
                            de_model_ir *ir) {
    if (src == NULL || ir == NULL || (src->data == NULL && src->size != 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_model_fbx_load: NULL argument");
    const char *path = src->path != NULL ? src->path : "";
    de_model_source s = *src;
    s.path = path;
    if (src->size == 0)
        return DE_FAIL(DE_ERR_PARSE, "%s: FBX: the file is empty", path);
    /* Validate the overrides before parsing, so a bad setting fails fast. */
    ufbx_coordinate_axis tmp = UFBX_COORDINATE_AXIS_UNKNOWN;
    if (opt != NULL && !is_auto(opt->axis_up) && !parse_axis(opt->axis_up, &tmp))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "%s: setting `axis_up` must be \"auto\", \"x\", \"y\" or \"z\" "
                       "(optionally signed), got \"%s\"",
                       path, opt->axis_up);
    if (opt != NULL && !is_auto(opt->axis_front) && !parse_axis(opt->axis_front, &tmp))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "%s: setting `axis_front` must be \"auto\" or a signed axis such as "
                       "\"+y\" or \"-z\", got \"%s\"",
                       path, opt->axis_front);

    static const char magic[] = "Kaydara FBX Binary";
    bool binary =
        src->size >= sizeof magic - 1u && memcmp(src->data, magic, sizeof magic - 1u) == 0;
    if (!binary && ascii_truncated((const u8 *)src->data, src->size))
        return DE_FAIL(DE_ERR_PARSE, "%s: FBX load: the file is truncated (unbalanced braces)",
                       path);

    ufbx_load_opts lo;
    memset(&lo, 0, sizeof lo);
    lo.temp_allocator = allocator_opts(ir->alloc);
    lo.result_allocator = allocator_opts(ir->alloc);
    lo.file_format = UFBX_FILE_FORMAT_FBX;
    lo.no_format_from_extension = true;
    lo.target_unit_meters = 1.0;
    lo.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    lo.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    lo.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
    lo.geometry_transform_helper_name.data = "GeometryTransform";
    lo.geometry_transform_helper_name.length = strlen("GeometryTransform");
    lo.scale_helper_name.data = "ScaleHelper";
    lo.scale_helper_name.length = strlen("ScaleHelper");
    lo.generate_missing_normals = true;
    lo.normalize_normals = true;
    lo.normalize_tangents = true;
    lo.clean_skin_weights = true;
    lo.use_blender_pbr_material = true;
    lo.node_depth_limit = 1024;
    ufbx_error err;
    ufbx_scene *scene = ufbx_load_memory(src->data, (size_t)src->size, &lo, &err);
    if (scene == NULL)
        return ufbx_fail(&s, &err, "load");
    de__fbx_ctx f;
    memset(&f, 0, sizeof f);
    f.src = &s;
    f.ir = ir;
    f.scene = scene;
    de_result r = load(&f, opt);
    ufbx_free_scene(scene);
    return r;
}
