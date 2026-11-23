/*
 * DENG-766: physics authoring assets. Parsers for the DIDE formats (`*.physmat.toml`,
 * `*.ragdoll.toml`) and a per-world registry of named materials and ragdolls (extension slot
 * DE__PHYS_EXT_ASSETS). Parsing reads text only; the registry is looked up linearly (a few
 * dozen assets at most) when bodies and ragdolls are created.
 */
#include "physics/physics_internal.h"

#include "core/de_toml_internal.h"

#include <dengine/de_memory.h>

#include <string.h>

/* ================================ TOML helpers ========================================== */

/* A number key into *out (int or float). Missing: true, *out untouched. Wrong type: false. */
static bool tm_num(toml_datum_t t, const char *key, f32 *out) {
    toml_datum_t d = toml_get(t, key);
    if (d.type == TOML_UNKNOWN)
        return true;
    if (d.type == TOML_FP64) {
        *out = (f32)d.u.fp64;
        return true;
    }
    if (d.type == TOML_INT64) {
        *out = (f32)d.u.int64;
        return true;
    }
    return false;
}

static bool tm_bool(toml_datum_t t, const char *key, bool *out) {
    toml_datum_t d = toml_get(t, key);
    if (d.type == TOML_UNKNOWN)
        return true;
    if (d.type != TOML_BOOLEAN)
        return false;
    *out = d.u.boolean;
    return true;
}

/* A number array of exactly n entries. Missing: true, untouched. */
static bool tm_vec(toml_datum_t t, const char *key, f32 *out, int n) {
    toml_datum_t d = toml_get(t, key);
    if (d.type == TOML_UNKNOWN)
        return true;
    if (d.type != TOML_ARRAY || d.u.arr.size != n)
        return false;
    for (int i = 0; i < n; ++i) {
        toml_datum_t e = d.u.arr.elem[i];
        if (e.type == TOML_FP64)
            out[i] = (f32)e.u.fp64;
        else if (e.type == TOML_INT64)
            out[i] = (f32)e.u.int64;
        else
            return false;
    }
    return true;
}

/* The string key, or NULL when missing; *bad when present but not a string. */
static const char *tm_str(toml_datum_t t, const char *key, bool *bad) {
    toml_datum_t d = toml_get(t, key);
    if (d.type == TOML_UNKNOWN)
        return NULL;
    if (d.type != TOML_STRING) {
        *bad = true;
        return NULL;
    }
    return d.u.s;
}

/* {position = [x, y, z], rotation = [x, y, z, w]} into a transform with scale 1. */
static bool tm_pose(toml_datum_t t, const char *key, de_transform *out) {
    memset(out, 0, sizeof *out);
    out->rotation.w = 1.0f;
    out->scale = (de_vec3){1.0f, 1.0f, 1.0f};
    toml_datum_t d = toml_get(t, key);
    if (d.type == TOML_UNKNOWN)
        return true;
    if (d.type != TOML_TABLE)
        return false;
    f32 pos[3] = {0.0f, 0.0f, 0.0f};
    f32 rot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (!tm_vec(d, "position", pos, 3) || !tm_vec(d, "rotation", rot, 4))
        return false;
    out->position = (de_dvec3){pos[0], pos[1], pos[2]};
    out->rotation = (de_quat){rot[0], rot[1], rot[2], rot[3]};
    return true;
}

/* ================================ Materials ============================================= */

static bool parse_combine(const char *s, de_combine_mode *out) {
    static const struct {
        const char *name;
        de_combine_mode mode;
    } k[] = {
        {"average", DE_COMBINE_AVERAGE},   {"minimum", DE_COMBINE_MINIMUM},
        {"maximum", DE_COMBINE_MAXIMUM},   {"multiply", DE_COMBINE_MULTIPLY},
        {"geometric", DE_COMBINE_GEOMETRIC},
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; ++i)
        if (strcmp(s, k[i].name) == 0) {
            *out = k[i].mode;
            return true;
        }
    return false;
}

DE_API de_result de_physics_material_parse(const char *text, u32 len, de_material_phys *out) {
    if (text == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics_material_parse: NULL argument");
    toml_result_t res = de__toml_parse(NULL, text, (int)len);
    if (!res.ok) {
        de_result r = DE_FAIL(DE_ERR_PARSE, "physmat: %s", res.errmsg);
        de__toml_free(res);
        return r;
    }
    de_material_phys m;
    memset(&m, 0, sizeof m);
    m.friction = 0.5f;
    bool bad = false;
    toml_datum_t t = res.toptab;
    if (!tm_num(t, "friction", &m.friction) || !tm_num(t, "restitution", &m.restitution) ||
        !tm_num(t, "density", &m.density))
        bad = true;
    const char *fc = tm_str(t, "friction_combine", &bad);
    const char *rc = tm_str(t, "restitution_combine", &bad);
    m.friction_combine = DE_COMBINE_GEOMETRIC; /* DIDE's defaults when the key is missing */
    m.restitution_combine = DE_COMBINE_MAXIMUM;
    if (fc != NULL && !parse_combine(fc, &m.friction_combine))
        bad = true;
    if (rc != NULL && !parse_combine(rc, &m.restitution_combine))
        bad = true;
    de__toml_free(res);
    if (bad)
        return DE_FAIL(DE_ERR_PARSE, "physmat: a key has the wrong type or an unknown mode");
    if (!de__f32_finite(m.friction) || !de__f32_finite(m.restitution) ||
        !de__f32_finite(m.density) || m.friction < 0.0f || m.restitution < 0.0f ||
        m.density < 0.0f)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "physmat: negative or non-finite value");
    *out = m;
    return DE_OK;
}

/* ================================ Ragdolls ============================================== */

static bool parse_bone(toml_datum_t b, u32 index, de_ragdoll_bone *o) {
    memset(o, 0, sizeof *o);
    o->parent = DE_RAGDOLL_NO_PARENT;
    toml_datum_t par = toml_get(b, "parent");
    if (par.type == TOML_INT64) {
        if (par.u.int64 < 0 || (u64)par.u.int64 >= (u64)index)
            return false;
        o->parent = (u32)par.u.int64;
    } else if (par.type != TOML_UNKNOWN) {
        return false;
    }
    if ((index == 0u) != (o->parent == DE_RAGDOLL_NO_PARENT))
        return false;
    bool bad = false;
    const char *shape = tm_str(b, "shape", &bad);
    const char *joint = tm_str(b, "joint", &bad);
    if (bad)
        return false;
    o->shape = DE_RAGDOLL_SHAPE_CAPSULE;
    if (shape != NULL) {
        if (strcmp(shape, "capsule") == 0)
            o->shape = DE_RAGDOLL_SHAPE_CAPSULE;
        else if (strcmp(shape, "box") == 0)
            o->shape = DE_RAGDOLL_SHAPE_BOX;
        else if (strcmp(shape, "sphere") == 0)
            o->shape = DE_RAGDOLL_SHAPE_SPHERE;
        else
            return false;
    }
    o->joint = DE_RAGDOLL_JOINT_CONE_TWIST;
    if (joint != NULL) {
        if (strcmp(joint, "cone_twist") == 0)
            o->joint = DE_RAGDOLL_JOINT_CONE_TWIST;
        else if (strcmp(joint, "hinge") == 0)
            o->joint = DE_RAGDOLL_JOINT_HINGE;
        else
            return false;
    }
    f32 he[3] = {0.0f, 0.0f, 0.0f};
    f32 jr[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (!tm_pose(b, "bind", &o->bind) || !tm_pose(b, "shape_offset", &o->shape_offset) ||
        !tm_num(b, "radius", &o->radius) || !tm_num(b, "half_height", &o->half_height) ||
        !tm_vec(b, "half_extents", he, 3) || !tm_num(b, "mass", &o->mass) ||
        !tm_vec(b, "joint_rotation", jr, 4) || !tm_num(b, "limit_min", &o->limit_min) ||
        !tm_num(b, "limit_max", &o->limit_max) || !tm_num(b, "swing_y", &o->swing_y) ||
        !tm_num(b, "swing_z", &o->swing_z))
        return false;
    o->half_extents = (de_vec3){he[0], he[1], he[2]};
    o->joint_rotation = (de_quat){jr[0], jr[1], jr[2], jr[3]};
    return true;
}

DE_API de_result de_ragdoll_asset_parse(const char *text, u32 len, de_ragdoll_asset *out) {
    if (text == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_ragdoll_asset_parse: NULL argument");
    toml_result_t res = de__toml_parse(NULL, text, (int)len);
    if (!res.ok) {
        de_result r = DE_FAIL(DE_ERR_PARSE, "ragdoll: %s", res.errmsg);
        de__toml_free(res);
        return r;
    }
    memset(out, 0, sizeof *out);
    toml_datum_t t = res.toptab;
    bool bad = false;
    const char *skel = tm_str(t, "skeleton", &bad);
    if (skel != NULL) {
        size_t n = strlen(skel);
        if (n >= sizeof out->skeleton)
            bad = true;
        else
            memcpy(out->skeleton, skel, n + 1u);
    }
    if (!tm_bool(t, "self_collision", &out->self_collision))
        bad = true;
    toml_datum_t bones = toml_get(t, "bones");
    if (bones.type != TOML_ARRAY || bones.u.arr.size < 1 ||
        bones.u.arr.size > (int)DE_RAGDOLL_MAX_BONES)
        bad = true;
    if (!bad) {
        out->bone_count = (u32)bones.u.arr.size;
        for (u32 i = 0; i < out->bone_count && !bad; ++i) {
            toml_datum_t b = bones.u.arr.elem[i];
            if (b.type != TOML_TABLE || !parse_bone(b, i, &out->bones[i]))
                bad = true;
        }
    }
    de__toml_free(res);
    if (bad)
        return DE_FAIL(DE_ERR_PARSE, "ragdoll: bad bones, parent order or key type");
    return DE_OK;
}

/* ================================ Registry ============================================== */

typedef struct mat_entry {
    de_name name;
    de_material_phys m;
} mat_entry;

typedef struct rag_entry {
    de_name name;
    u32 stamp;
    de_ragdoll_asset *a;
} rag_entry;

typedef struct de__phys_assets {
    const de_allocator *alloc;
    mat_entry *mats;
    u32 mat_count, mat_cap;
    rag_entry *rags;
    u32 rag_count, rag_cap;
    u32 stamp;
} de__phys_assets;

static void assets_destroy(de_physics_world *p, void *state) {
    (void)p;
    de__phys_assets *s = (de__phys_assets *)state;
    const de_allocator *a = s->alloc;
    for (u32 i = 0; i < s->rag_count; ++i)
        DE_FREE(a, s->rags[i].a, sizeof(de_ragdoll_asset));
    if (s->mats != NULL)
        DE_FREE(a, s->mats, sizeof(mat_entry) * (size_t)s->mat_cap);
    if (s->rags != NULL)
        DE_FREE(a, s->rags, sizeof(rag_entry) * (size_t)s->rag_cap);
    DE_FREE(a, s, sizeof *s);
}

static const de__phys_ext_vtbl k_assets_vtbl = {assets_destroy, NULL, NULL};

static de__phys_assets *assets_get(const de_physics_world *p) {
    return (de__phys_assets *)de__phys_ext_get(p, DE__PHYS_EXT_ASSETS);
}

static de__phys_assets *assets_make(de_physics_world *p) {
    de__phys_assets *s = assets_get(p);
    if (s != NULL)
        return s;
    s = (de__phys_assets *)DE_ALLOC(p->alloc, sizeof *s, DE_ALIGNOF(de__phys_assets),
                                    DE_MEM_PHYSICS);
    if (s == NULL)
        return NULL;
    memset(s, 0, sizeof *s);
    s->alloc = p->alloc;
    de__phys_ext_set(p, DE__PHYS_EXT_ASSETS, s, &k_assets_vtbl);
    return s;
}

/* Grows *arr (elem bytes each) to hold one more; false when out of memory. */
static bool grow(const de_allocator *a, void **arr, u32 count, u32 *cap, size_t elem,
                 size_t align) {
    if (count < *cap)
        return true;
    u32 nc = *cap == 0u ? 8u : *cap * 2u;
    void *n = DE_ALLOC(a, elem * (size_t)nc, align, DE_MEM_PHYSICS);
    if (n == NULL)
        return false;
    if (*arr != NULL) {
        memcpy(n, *arr, elem * (size_t)count);
        DE_FREE(a, *arr, elem * (size_t)*cap);
    }
    *arr = n;
    *cap = nc;
    return true;
}

DE_API de_result de_physics_material_register(de_physics_world *p, de_name name,
                                              const de_material_phys *m) {
    if (p == NULL || m == NULL || name.hash == 0u)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics_material_register: bad argument");
    if ((u32)m->friction_combine > (u32)DE_COMBINE_MAXIMUM ||
        (u32)m->restitution_combine > (u32)DE_COMBINE_MAXIMUM)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics_material_register: unknown combine");
    de__phys_assets *s = assets_make(p);
    if (s == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_physics_material_register");
    for (u32 i = 0; i < s->mat_count; ++i)
        if (s->mats[i].name.hash == name.hash) {
            s->mats[i].m = *m;
            return DE_OK;
        }
    if (!grow(s->alloc, (void **)&s->mats, s->mat_count, &s->mat_cap, sizeof(mat_entry),
              DE_ALIGNOF(mat_entry)))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_physics_material_register");
    s->mats[s->mat_count].name = name;
    s->mats[s->mat_count].m = *m;
    ++s->mat_count;
    return DE_OK;
}

DE_API bool de_physics_material_find(const de_physics_world *p, de_name name,
                                     de_material_phys *out) {
    de__phys_assets *s = p != NULL ? assets_get(p) : NULL;
    if (s == NULL || name.hash == 0u)
        return false;
    for (u32 i = 0; i < s->mat_count; ++i)
        if (s->mats[i].name.hash == name.hash) {
            if (out != NULL)
                *out = s->mats[i].m;
            return true;
        }
    return false;
}

DE_API de_result de_physics_ragdoll_asset_register(de_physics_world *p, de_name name,
                                                   const de_ragdoll_asset *a) {
    if (p == NULL || a == NULL || name.hash == 0u || a->bone_count == 0u ||
        a->bone_count > DE_RAGDOLL_MAX_BONES)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics_ragdoll_asset_register");
    de__phys_assets *s = assets_make(p);
    if (s == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_physics_ragdoll_asset_register");
    for (u32 i = 0; i < s->rag_count; ++i)
        if (s->rags[i].name.hash == name.hash) {
            memcpy(s->rags[i].a, a, sizeof *a);
            s->rags[i].stamp = ++s->stamp;
            return DE_OK;
        }
    if (!grow(s->alloc, (void **)&s->rags, s->rag_count, &s->rag_cap, sizeof(rag_entry),
              DE_ALIGNOF(rag_entry)))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_physics_ragdoll_asset_register");
    de_ragdoll_asset *copy = (de_ragdoll_asset *)DE_ALLOC(
        s->alloc, sizeof(de_ragdoll_asset), DE_ALIGNOF(de_ragdoll_asset), DE_MEM_PHYSICS);
    if (copy == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_physics_ragdoll_asset_register");
    memcpy(copy, a, sizeof *a);
    rag_entry *e = &s->rags[s->rag_count++];
    e->name = name;
    e->a = copy;
    e->stamp = ++s->stamp;
    return DE_OK;
}

DE_API const de_ragdoll_asset *de_physics_ragdoll_asset_find(const de_physics_world *p,
                                                             de_name name) {
    return de__phys_ragdoll_asset(p, name, NULL);
}

const de_ragdoll_asset *de__phys_ragdoll_asset(const de_physics_world *p, de_name name,
                                               u32 *stamp) {
    de__phys_assets *s = p != NULL ? assets_get(p) : NULL;
    if (s == NULL || name.hash == 0u)
        return NULL;
    for (u32 i = 0; i < s->rag_count; ++i)
        if (s->rags[i].name.hash == name.hash) {
            if (stamp != NULL)
                *stamp = s->rags[i].stamp;
            return s->rags[i].a;
        }
    return NULL;
}
