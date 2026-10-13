/*
 * de_cooked_skeleton.h: the cooked skeleton format DSKL, version 1 (SPEC §11.6, §21.5; DENG-104).
 * Internal; include as "anim/formats/de_cooked_skeleton.h".
 *
 * The bytes of a `skeleton` asset, a sub-asset `<model>#skeleton:<name>` of a model. The model
 * importer (tools/cook/importers/model/anim_skeleton.c) writes one per skin of the source. The
 * animation module (S19 playback, S27 animator) reads it. Everything is in engine coordinates
 * (Z-up, right-handed, metres, SPEC §5.5), with the import `scale` applied.
 *
 * Joints: the skin's joints and their ancestor nodes up to the frame they share with the skinned
 * mesh node (the importer's rule, anim_skeleton.c). Model space is the space of that frame. They
 * are sorted parent-before-child: parent[i] < i, or DE_COOKED_SKEL_NO_PARENT for a root. The sort
 * is stable in source order (skin joint order; an ancestor comes just before the first joint that
 * needs it). Ancestors that are not skin joints have no mesh joint index.
 *
 * Skinning: the skin matrix of mesh joint i, with j = remap[i], is
 *     mesh_from_model x model[j] x inverse_bind[j]
 * where model[j] is the joint's model-space transform. It maps the mesh's bind-pose vertices to
 * the posed vertices in the mesh node's own space, and it is the identity in the rest pose.
 * mesh_from_model is the identity unless the skinned mesh node has its own transform below the
 * shared frame.
 *
 * Nothing here is render-facing: the renderer never includes the anim module (S19 checks this).
 *
 * Every field is little-endian and read through the helpers below, never a struct copy, so the
 * bytes are the same on every host. Floats are IEEE-754 bit patterns. Layout:
 *
 *   header      DE_COOKED_SKEL_HEADER_SIZE (112) bytes:
 *                 0  u8[4] magic "DSKL"
 *                 4  u32   format version (DE_COOKED_SKEL_VERSION)
 *                 8  u32   flags (DE_COOKED_SKEL_HAS_NAMES)
 *                12  u32   joint count J (<= DE_COOKED_SKEL_MAX_JOINTS)
 *                16  u32   mesh joint count M (the skin's joint list)
 *                20  u32   root joint: the top joint of the skin, which carries root motion
 *                          (S27); DE_COOKED_NONE when J = 0
 *                24  u32   parents offset          (J x u16)
 *                28  u32   name hash offset        (J x u64, 8-byte aligned)
 *                32  u32   rest pose offset        (J x 10 f32: translation 3, rotation x y z w,
 *                                                   scale 3; 4-byte aligned)
 *                36  u32   inverse bind offset     (J x 12 f32: de_affine, column-major 3x4)
 *                40  u32   mesh joint remap offset (M x u16: skeleton joint of mesh joint i)
 *                44  u32   name table offset       (J x u32 string offsets, then the strings;
 *                                                   0 without DE_COOKED_SKEL_HAS_NAMES)
 *                48  u32   name table bytes
 *                52  u32   total size of the blob
 *                56  f32[12] mesh_from_model (de_affine, column-major 3x4)
 *               104  u64   reserved, 0
 *   sections    in the order of the offsets above, each 8-byte aligned.
 *
 * Name hashes are de_name values (de_hash64_str of the joint's name). The joint's name is the
 * unique sub-asset name of its node (`<NodeName>`, `~N` for duplicates), the same as the node
 * names of the model (DMDL). The name strings are a Dev aid (importer setting
 * `anim_joint_names`), not needed to play a clip.
 *
 * Bump DE_COOKED_SKEL_VERSION on any change.
 */
#ifndef DE_ANIM_FORMATS_COOKED_SKELETON_H
#define DE_ANIM_FORMATS_COOKED_SKELETON_H

#include <dengine/de_base.h>

#include <string.h>

DE_EXTERN_C_BEGIN

/* ---- Little-endian fields (shared with de_cooked_clip.h) ------------------------------------- */

static inline u16 de_canim_get_u16(const u8 *p) {
    return (u16)((u16)p[0] | (u16)((u16)p[1] << 8));
}

static inline u32 de_canim_get_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline u64 de_canim_get_u64(const u8 *p) {
    return (u64)de_canim_get_u32(p) | ((u64)de_canim_get_u32(p + 4) << 32);
}

static inline f32 de_canim_get_f32(const u8 *p) {
    u32 b = de_canim_get_u32(p);
    f32 f;
    memcpy(&f, &b, 4);
    return f;
}

/** True if [off, off + len) lies inside a blob of `size` bytes (no overflow). */
static inline bool de_canim_range_ok(u64 size, u64 off, u64 len) {
    return off <= size && len <= size - off;
}

/* ---- Format ---------------------------------------------------------------------------------- */

#define DE_COOKED_SKEL_MAGIC       "DSKL"
#define DE_COOKED_SKEL_VERSION     1u
#define DE_COOKED_SKEL_HEADER_SIZE 112u
/** The parent of a root joint. */
#define DE_COOKED_SKEL_NO_PARENT 0xFFFFu
/** Joint indices are u16 and 0xFFFF means "none". */
#define DE_COOKED_SKEL_MAX_JOINTS 0xFFFEu
/** Header flag: the blob has the joint name strings. */
#define DE_COOKED_SKEL_HAS_NAMES 1u
#ifndef DE_COOKED_NONE
/** "None" in a u32 index field (same value as the render formats). */
#define DE_COOKED_NONE 0xFFFFFFFFu
#endif

/** One joint's local transform: translation, rotation (unit quaternion x, y, z, w), scale. */
typedef struct de_cooked_joint_pose {
    f32 t[3];
    f32 r[4];
    f32 s[3];
} de_cooked_joint_pose;

/** A checked view of a DSKL blob (pointers into it). */
typedef struct de_cooked_skeleton_view {
    const u8 *base;
    u64 size;
    u32 flags;
    u32 joint_count, mesh_joint_count, root_joint;
    const u8 *parents, *name_hashes, *rest, *inverse_bind, *remap, *names;
    u32 names_size;
} de_cooked_skeleton_view;

/**
 * Checks a DSKL blob and fills `out`: magic, version, sizes, offsets, parent-before-child order,
 * the remap and root joint in range, the name strings inside the table and NUL-terminated.
 * Allocation-free. False on any error, with a short reason in `*why` (nullable).
 */
static inline bool de_cooked_skeleton_parse(const void *data, u64 size,
                                            de_cooked_skeleton_view *out, const char **why) {
    const char *w = NULL;
    const u8 *p = (const u8 *)data;
    memset(out, 0, sizeof *out);
    if (p == NULL || size < DE_COOKED_SKEL_HEADER_SIZE) {
        w = "shorter than the header";
        goto fail;
    }
    if (memcmp(p, DE_COOKED_SKEL_MAGIC, 4) != 0) {
        w = "not a cooked skeleton (magic)";
        goto fail;
    }
    if (de_canim_get_u32(p + 4) != DE_COOKED_SKEL_VERSION) {
        w = "unsupported format version";
        goto fail;
    }
    out->base = p;
    out->size = size;
    out->flags = de_canim_get_u32(p + 8);
    out->joint_count = de_canim_get_u32(p + 12);
    out->mesh_joint_count = de_canim_get_u32(p + 16);
    out->root_joint = de_canim_get_u32(p + 20);
    u32 par = de_canim_get_u32(p + 24), hash = de_canim_get_u32(p + 28);
    u32 rest = de_canim_get_u32(p + 32), ibm = de_canim_get_u32(p + 36);
    u32 remap = de_canim_get_u32(p + 40), names = de_canim_get_u32(p + 44);
    out->names_size = de_canim_get_u32(p + 48);
    u32 total = de_canim_get_u32(p + 52);
    u64 j = out->joint_count;
    bool has_names = (out->flags & DE_COOKED_SKEL_HAS_NAMES) != 0;
    if (total != size || j > DE_COOKED_SKEL_MAX_JOINTS ||
        out->mesh_joint_count > DE_COOKED_SKEL_MAX_JOINTS ||
        (j == 0 ? out->root_joint != DE_COOKED_NONE : out->root_joint >= j) ||
        !de_canim_range_ok(size, par, j * 2u) || !de_canim_range_ok(size, hash, j * 8u) ||
        !de_canim_range_ok(size, rest, j * 40u) || !de_canim_range_ok(size, ibm, j * 48u) ||
        !de_canim_range_ok(size, remap, (u64)out->mesh_joint_count * 2u) ||
        (has_names &&
         (!de_canim_range_ok(size, names, out->names_size) || (u64)out->names_size < j * 4u)) ||
        par < DE_COOKED_SKEL_HEADER_SIZE || (hash & 7u) != 0 || (rest & 3u) != 0 ||
        (ibm & 3u) != 0) {
        w = "bad sizes or offsets";
        goto fail;
    }
    out->parents = p + par;
    out->name_hashes = p + hash;
    out->rest = p + rest;
    out->inverse_bind = p + ibm;
    out->remap = p + remap;
    out->names = has_names ? p + names : NULL;
    for (u32 i = 0; i < out->joint_count; ++i) {
        u16 pa = de_canim_get_u16(out->parents + 2u * i);
        if (pa != DE_COOKED_SKEL_NO_PARENT && pa >= i) {
            w = "a joint comes before its parent";
            goto fail;
        }
        if (has_names) {
            u32 o = de_canim_get_u32(out->names + 4u * i);
            u64 lo = j * 4u;
            if (o < lo || o >= out->names_size ||
                memchr(out->names + o, 0, out->names_size - o) == NULL) {
                w = "bad joint name";
                goto fail;
            }
        }
    }
    for (u32 i = 0; i < out->mesh_joint_count; ++i)
        if (de_canim_get_u16(out->remap + 2u * i) >= j) {
            w = "mesh joint remap out of range";
            goto fail;
        }
    return true;
fail:
    memset(out, 0, sizeof *out);
    if (why != NULL)
        *why = w;
    return false;
}

/** Parent of joint `i` (< joint_count), DE_COOKED_SKEL_NO_PARENT for a root. */
static inline u16 de_cooked_skeleton_parent(const de_cooked_skeleton_view *v, u32 i) {
    return de_canim_get_u16(v->parents + 2u * i);
}

/** de_name hash of joint `i`. */
static inline u64 de_cooked_skeleton_name_hash(const de_cooked_skeleton_view *v, u32 i) {
    return de_canim_get_u64(v->name_hashes + 8u * (u64)i);
}

/** Name string of joint `i`, or NULL when the blob has no names. */
static inline const char *de_cooked_skeleton_name(const de_cooked_skeleton_view *v, u32 i) {
    if (v->names == NULL)
        return NULL;
    return (const char *)(v->names + de_canim_get_u32(v->names + 4u * i));
}

/** Rest (bind-time) local pose of joint `i`. */
static inline de_cooked_joint_pose de_cooked_skeleton_rest(const de_cooked_skeleton_view *v,
                                                           u32 i) {
    de_cooked_joint_pose pose;
    const u8 *r = v->rest + 40u * (u64)i;
    for (u32 k = 0; k < 3; ++k) {
        pose.t[k] = de_canim_get_f32(r + 4u * k);
        pose.s[k] = de_canim_get_f32(r + 28u + 4u * k);
    }
    for (u32 k = 0; k < 4; ++k)
        pose.r[k] = de_canim_get_f32(r + 12u + 4u * k);
    return pose;
}

/** Inverse bind matrix of joint `i`: 12 floats, de_affine layout (column-major 3x4). For a joint
 * that is not a skin joint: the inverse of mesh_from_model x its rest model transform. */
static inline void de_cooked_skeleton_inverse_bind(const de_cooked_skeleton_view *v, u32 i,
                                                   f32 m[12]) {
    for (u32 k = 0; k < 12; ++k)
        m[k] = de_canim_get_f32(v->inverse_bind + 48u * (u64)i + 4u * k);
}

/** The mesh_from_model transform: 12 floats, de_affine layout. */
static inline void de_cooked_skeleton_mesh_from_model(const de_cooked_skeleton_view *v, f32 m[12]) {
    for (u32 k = 0; k < 12; ++k)
        m[k] = de_canim_get_f32(v->base + 56u + 4u * k);
}

/** Skeleton joint of mesh joint `i` (< mesh_joint_count; the JOINTS0 values of the mesh). */
static inline u16 de_cooked_skeleton_remap(const de_cooked_skeleton_view *v, u32 i) {
    return de_canim_get_u16(v->remap + 2u * i);
}

DE_EXTERN_C_END

#endif /* DE_ANIM_FORMATS_COOKED_SKELETON_H */
