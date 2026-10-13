/*
 * fuzz_scene_harness.c: the runtime part of the scene fuzz harness (DENG-403). See
 * fuzz_scene_harness.h.
 *
 * Each instantiation builds a type registry and a world of its own and destroys both before it
 * returns. A world registers the ECS built-ins (DeTransform, DeChildOf, ...) into its registry, and
 * registering a type again copies its layout into the registry; a world per input over one shared
 * registry would therefore grow the registry by a few KiB per input. The shared registry is only
 * for the importer (fuzz_scene_cook.c) and the cooked-output check.
 */
#include "fuzz_scene_harness.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>

#include "asset/asset_internal.h"
#include "asset/scene/scene_format.h"
#include "asset/slots_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Component types ------------------------------------------------------------------------- */

typedef struct THealth {
    f32 current;
    f32 max;
} THealth;

typedef struct TTarget {
    de_entity target;
    u32 texture; /* a de_blob handle */
    f32 speed;
} TTarget;

typedef enum TMood { TMOOD_CALM = 0, TMOOD_ANGRY = 1, TMOOD_AFRAID = -2 } TMood;
enum { TLAYER_GROUND = 1u << 0, TLAYER_WATER = 1u << 1, TLAYER_AIR = 1u << 2 };

typedef struct TItem {
    u32 id;
    i32 count;
} TItem;

typedef struct TMisc {
    de_quat rot;
    i64 i64v;
    u64 u64v;
    f64 f64v;
    de_name name;
    de_vec3 v3;
    i32 i32v;
    u32 u32v;
    i32 mood;   /* TMood */
    u32 layers; /* TLayers */
    TItem items[3];
    f32 samples[4];
    i16 i16v;
    u16 u16v;
    i8 i8v;
    u8 u8v;
    bool flag;
} TMisc;

typedef struct TMarker {
    u32 unused;
} TMarker;

static const de_enum_value k_mood_values[] = {
    {"calm", TMOOD_CALM}, {"angry", TMOOD_ANGRY}, {"afraid", TMOOD_AFRAID}};
static const de_enum_value k_layers_values[] = {
    {"ground", TLAYER_GROUND}, {"water", TLAYER_WATER}, {"air", TLAYER_AIR}};

static const de_field_info k_health_fields[] = {
    DE_FIELD(THealth, current, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(THealth, max, DE_FK_F32, DE_FIELD_DEFAULT),
};
static const de_field_info k_target_fields[] = {
    DE_FIELD(TTarget, target, DE_FK_ENTITY, DE_FIELD_DEFAULT),
    DE_FIELD_EX(TTarget, texture, DE_FK_ASSET, DE_FIELD_DEFAULT | DE_FIELD_ASSET_REF,
                {0, 0, 0, 0, "blob", 0}),
    DE_FIELD(TTarget, speed, DE_FK_F32, DE_FIELD_DEFAULT),
};
static const de_field_info k_item_fields[] = {
    DE_FIELD(TItem, id, DE_FK_U32, DE_FIELD_DEFAULT),
    DE_FIELD(TItem, count, DE_FK_I32, DE_FIELD_DEFAULT),
};
static const de_field_info k_misc_fields[] = {
    DE_FIELD(TMisc, rot, DE_FK_QUAT, DE_FIELD_DEFAULT),
    DE_FIELD(TMisc, i64v, DE_FK_I64, DE_FIELD_DEFAULT),
    DE_FIELD(TMisc, u64v, DE_FK_U64, DE_FIELD_DEFAULT),
    DE_FIELD(TMisc, f64v, DE_FK_F64, DE_FIELD_DEFAULT),
    DE_FIELD(TMisc, name, DE_FK_NAME, DE_FIELD_DEFAULT),
    DE_FIELD(TMisc, v3, DE_FK_VEC3, DE_FIELD_DEFAULT),
    DE_FIELD(TMisc, i32v, DE_FK_I32, DE_FIELD_DEFAULT),
    DE_FIELD(TMisc, u32v, DE_FK_U32, DE_FIELD_DEFAULT),
    DE_FIELD_EX(TMisc, mood, DE_FK_ENUM, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TMood", 0}),
    DE_FIELD_EX(TMisc, layers, DE_FK_FLAGS, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TLayers", 0}),
    DE_FIELD_EX(TMisc, items, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TItem", 3}),
    DE_FIELD_EX(TMisc, samples, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "f32", 4}),
    DE_FIELD(TMisc, i16v, DE_FK_I16, DE_FIELD_DEFAULT),
    DE_FIELD(TMisc, u16v, DE_FK_U16, DE_FIELD_DEFAULT),
    DE_FIELD(TMisc, i8v, DE_FK_I8, DE_FIELD_DEFAULT),
    DE_FIELD(TMisc, u8v, DE_FK_U8, DE_FIELD_DEFAULT),
    DE_FIELD(TMisc, flag, DE_FK_BOOL, DE_FIELD_DEFAULT),
};
static const de_field_info k_marker_fields[] = {
    DE_FIELD(TMarker, unused, DE_FK_U32, DE_FIELD_DEFAULT),
};

/* THealth v1 { hp } -> v2 { current = hp, max = 100 }. It reads every byte of every old field it
 * is given, so AddressSanitizer reports an old layout that points outside old_data. */
static bool health_migrate(void *obj, u32 from_version, const void *old_data,
                           const de_field_info *old_fields, u32 old_field_count) {
    THealth *h = (THealth *)obj;
    volatile u8 sink = 0;
    for (u32 i = 0; i < old_field_count; ++i) {
        const u8 *p = (const u8 *)old_data + old_fields[i].offset;
        for (u32 b = 0; b < old_fields[i].size; ++b)
            sink = (u8)(sink ^ p[b]);
        if (strcmp(old_fields[i].name, "hp") == 0 && old_fields[i].kind == DE_FK_F32 &&
            old_fields[i].size == 4) {
            f32 hp;
            memcpy(&hp, p, sizeof hp);
            h->current = hp;
            h->max = 100.0f;
        }
    }
    (void)sink;
    return from_version == 1;
}

void fz_scene_fail(const char *target, const char *what) {
    const de_error *e = de_error_last();
    fprintf(stderr, "fuzz_%s: %s (last error: %s)\n", target, what,
            e != NULL && e->result != DE_OK ? e->message : "none");
    abort();
}

/* Registers the fuzz enums and component types into `r` (THealth at `health_version`, 1 or 2). */
static de_result fz_register_types(de_type_registry *r, u32 health_version) {
    DE_TRY(de_enum_register(r, "TMood", k_mood_values, 3, false));
    DE_TRY(de_enum_register(r, "TLayers", k_layers_values, 3, true));
    de_type_desc d;
    if (health_version == 1) {
        typedef struct THealthV1 {
            f32 hp;
        } THealthV1;
        static const de_field_info k_v1[] = {DE_FIELD(THealthV1, hp, DE_FK_F32, DE_FIELD_DEFAULT)};
        d = DE_TYPE_DESC(THealthV1, 1, k_v1);
        d.name = "THealth";
    } else {
        d = DE_TYPE_DESC(THealth, 2, k_health_fields);
        d.hooks.migrate = health_migrate;
    }
    if (!DE_HANDLE_VALID(de_type_register(r, &d)) ||
        !DE_HANDLE_VALID(de_type_register(r, &DE_TYPE_DESC(TTarget, 1, k_target_fields))) ||
        !DE_HANDLE_VALID(de_type_register(r, &DE_TYPE_DESC(TItem, 1, k_item_fields))) ||
        !DE_HANDLE_VALID(de_type_register(r, &DE_TYPE_DESC(TMisc, 1, k_misc_fields))) ||
        !DE_HANDLE_VALID(de_type_register(r, &DE_TYPE_DESC(TMarker, 1, k_marker_fields))))
        return DE_ERR_INTERNAL;
    return DE_OK;
}

/* Registers the fuzz types of `r` as components of `w` (TMarker as a tag), and the built-ins. */
static void fz_register_components(de_world *w, de_type_registry *r) {
    if (!DE_HANDLE_VALID(de_builtin_components(w).transform))
        fz_scene_fail("scene", "setup: the ECS built-ins did not register");
    static const char *const names[] = {"THealth", "TTarget", "TMisc"};
    for (u32 i = 0; i < 3u; ++i)
        if (!DE_HANDLE_VALID(de_component_register(w, de_type_find(r, names[i]),
                                                   DE_COMP_SERIALIZE | DE_COMP_SNAPSHOT)))
            fz_scene_fail("scene", "setup: a fuzz component did not register");
    if (!DE_HANDLE_VALID(de_component_register(w, de_type_find(r, "TMarker"), DE_COMP_TAG)))
        fz_scene_fail("scene", "setup: the tag did not register");
}

/* ---- Shared runtime -------------------------------------------------------------------------- */

static de_type_registry *g_types;
static de_world *g_types_world; /* registered the built-ins into g_types; never used again */
static de_asset_system *g_assets;

static void fz_setup(void) {
    if (g_assets != NULL)
        return;
    /* The loader, the importer, the reflect reader and the ECS warn about hostile input; the
     * fuzzer does not need the text and it would slow it down. */
    de_log_set_level(de_log_register_category("asset", DE_LOG_INFO), DE_LOG_FATAL);
    de_log_set_level(de_log_register_category("reflect", DE_LOG_INFO), DE_LOG_FATAL);
    de_log_set_level(de_log_register_category("ecs", DE_LOG_INFO), DE_LOG_FATAL);
    if (de_type_registry_create(NULL, &g_types) != DE_OK || fz_register_types(g_types, 2) != DE_OK)
        fz_scene_fail("scene", "setup: the type registry");
    de_world_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.types = g_types;
    if (de_world_create(&wd, &g_types_world) != DE_OK)
        fz_scene_fail("scene", "setup: the world");
    fz_register_components(g_types_world, g_types);
    de_asset_system_desc ad;
    memset(&ad, 0, sizeof ad);
    ad.types = g_types;
    ad.io_threads = 1;
    ad.cache_dir = "fuzzmem:/decache";
    if (de_asset_system_create(&ad, &g_assets) != DE_OK)
        fz_scene_fail("scene", "setup: the asset system");
    /* The blob the fixtures reference: asset fields naming it map to its handle. */
    u32 h = 0;
    if (de__slot_create(g_assets, de_asset_id_from(FZ_SCENE_BLOB_ID), de_hash64_str("blob"), &h) !=
        DE_OK)
        fz_scene_fail("scene", "setup: the blob slot");
}

de_type_registry *fz_scene_types(void) {
    fz_setup();
    return g_types;
}

de_asset_system *fz_scene_assets(void) {
    fz_setup();
    return g_assets;
}

/* ---- Checks ---------------------------------------------------------------------------------- */

void fz_scene_check_view(const char *target, const de__scene_view *v) {
    if (v->kind != DE__SCENE_KIND_SCENE && v->kind != DE__SCENE_KIND_PREFAB)
        fz_scene_fail(target, "parse accepted an unknown kind");
    if (v->kind == DE__SCENE_KIND_PREFAB && v->root_count != 1u)
        fz_scene_fail(target, "parse accepted a prefab without exactly one root");
    for (u32 t = 0; t < v->type_count; ++t) {
        const u8 *p = v->types + (size_t)t * DE__SCENE_TYPE_SIZE;
        u32 off = de__scene_get_u32(p), len = de__scene_get_u32(p + 4);
        if ((u64)off + len >= v->string_bytes || len == 0)
            fz_scene_fail(target, "parse accepted a type name outside the strings");
        de__scene_type_rec tr = de__scene_type_at(v, t);
        if (tr.name[tr.name_len] != '\0' || strlen(tr.name) != tr.name_len)
            fz_scene_fail(target, "parse accepted a type name that is not NUL-terminated");
        if (t != 0 && strcmp(de__scene_type_at(v, t - 1u).name, tr.name) >= 0)
            fz_scene_fail(target, "parse accepted type names that are not sorted and unique");
    }
    u32 roots = 0, next = 0;
    const u8 *data_end = v->data + v->data_bytes;
    for (u32 i = 0; i < v->entity_count; ++i) {
        de__scene_entity_rec e = de__scene_entity_at(v, i);
        if (e.scene_id == 0 || de__scene_find_entity(v, e.scene_id) != i)
            fz_scene_fail(target, "parse accepted an entity its scene ID does not find");
        if (e.parent == DE__SCENE_NO_PARENT)
            ++roots;
        else if (e.parent >= v->entity_count || e.parent == i)
            fz_scene_fail(target, "parse accepted a parent out of range");
        if (e.first_component != next || e.component_count > v->component_count - next)
            fz_scene_fail(target, "parse accepted a component range out of order");
        next += e.component_count;
        for (u32 k = 0; k < e.component_count; ++k) {
            de__scene_comp_rec c = de__scene_comp_at(v, e.first_component + k);
            if (c.type_index >= v->type_count)
                fz_scene_fail(target, "parse accepted a component type out of range");
            if (k != 0 &&
                c.type_index <= de__scene_comp_at(v, e.first_component + k - 1u).type_index)
                fz_scene_fail(target, "parse accepted an entity with a type twice");
            if (c.data < v->data || c.size > (u64)(data_end - c.data))
                fz_scene_fail(target, "parse accepted component data outside the data section");
        }
    }
    if (roots != v->root_count || next != v->component_count)
        fz_scene_fail(target, "parse accepted wrong root or component counts");
    for (u32 i = 0; i < v->asset_count; ++i) {
        u64 id = de__scene_asset_at(v, i);
        if (id == 0 || (i != 0 && id <= de__scene_asset_at(v, i - 1u)))
            fz_scene_fail(target, "parse accepted asset references that are not sorted and unique");
    }
}

/* Reference mapping of the cooked-output check: no world and no handles, as in the importer. An
 * entity field keeps its scene ID; an asset field must name an ID of the reference table. */
static u64 chk_entity_to_scene_id(u64 bits, void *user) {
    (void)user;
    return bits;
}
static u64 chk_scene_id_to_entity(u64 scene_id, void *user) {
    (void)user;
    return scene_id;
}
static u64 chk_asset_to_id(u32 handle, const char *asset_type, void *user) {
    (void)handle;
    (void)asset_type;
    (void)user;
    return 0;
}
static u32 chk_id_to_asset(u64 id, const char *asset_type, void *user) {
    (void)asset_type;
    const de__scene_view *v = (const de__scene_view *)user;
    if (id == 0)
        return 0;
    for (u32 i = 0; i < v->asset_count; ++i)
        if (de__scene_asset_at(v, i) == id)
            return i + 1u;
    fz_scene_fail("scene", "the importer wrote an asset ID that is not in the reference table");
    return 0;
}

void fz_scene_check_cooked(const char *target, const de__scene_view *v,
                           const de_type_registry *types) {
    de_ser_context sc = {chk_entity_to_scene_id, chk_scene_id_to_entity, chk_asset_to_id,
                         chk_id_to_asset, (void *)(uintptr_t)v};
    for (u32 i = 0; i < v->component_count; ++i) {
        de__scene_comp_rec c = de__scene_comp_at(v, i);
        de_type_id id = de_type_find(types, de__scene_type_at(v, c.type_index).name);
        const de_type_desc *td = de_type_get(types, id);
        if (td == NULL)
            fz_scene_fail(target, "the importer wrote a type that is not in the registry");
        if (td->size == 0)
            continue;
        const de_allocator *a = de_heap_allocator();
        size_t align = td->align != 0 ? td->align : 1u;
        void *obj = DE_ALLOC(a, td->size, align, DE_MEM_GENERAL);
        if (obj == NULL || de_type_construct(types, id, obj) != DE_OK)
            fz_scene_fail(target, "no memory for a component object");
        de_mem_reader mr;
        de_mem_reader_init(&mr, c.data, c.size);
        de_reader rd = de_reader_from_mem(&mr);
        de_ser_options opt = {DE_SER_BINARY, DE_FIELD_SERIALIZE, &sc};
        de_result r = de_deserialize_binary(types, id, obj, &opt, NULL, &rd);
        de_type_destruct(types, id, obj);
        DE_FREE(a, obj, td->size);
        if (r != DE_OK)
            fz_scene_fail(target, "the importer wrote a component object that does not decode");
        if (mr.pos != c.size)
            fz_scene_fail(target, "the importer wrote a component object with extra bytes");
    }
}

/* ---- Instantiate ----------------------------------------------------------------------------- */

/* Walks the tree under `top` with a bound: it must hold exactly `count` entities, each child's
 * parent must be the entity it is listed under, and each child list must end. A broken link would
 * otherwise loop in the hierarchy walks (de_entity_despawn_recursive). */
static void fz_check_tree(const char *target, de_world *w, de_entity top, u32 count) {
    de_entity *stack = (de_entity *)malloc(sizeof(de_entity) * (count + 1u));
    if (stack == NULL)
        abort();
    u32 sp = 0, seen = 0;
    stack[sp++] = top;
    while (sp != 0) {
        de_entity e = stack[--sp];
        u32 listed = 0;
        for (de_entity c = de_entity_children_first(w, e); c.bits != 0;
             c = de_entity_children_next(w, c)) {
            if (++seen > count || ++listed > de_entity_children_count(w, e))
                fz_scene_fail(target, "the instantiated hierarchy has a loop or a long child list");
            if (de_entity_parent(w, c).bits != e.bits)
                fz_scene_fail(target, "an instantiated child does not name its parent");
            stack[sp++] = c;
        }
        if (listed != de_entity_children_count(w, e))
            fz_scene_fail(target, "an instantiated child count is wrong");
    }
    if (seen != count)
        fz_scene_fail(target, "the instantiated hierarchy does not hold every entity");
    free(stack);
}

de_result fz_scene_instantiate(const char *target, const u8 *data, size_t size) {
    de_asset_system *s = fz_scene_assets();
    de__scene_view v;
    if (de__scene_view_parse(data, size, &v, NULL, 0) != DE_OK)
        fz_scene_fail(target, "instantiate: the bytes do not parse");

    /* The slot, holding the bytes as the loader holds them (scene_type.c, scene_load_kind). */
    const char *type = v.kind == DE__SCENE_KIND_PREFAB ? DE__PREFAB_TYPE_NAME : DE__SCENE_TYPE_NAME;
    u32 h = 0;
    if (de__slot_create(s, de_asset_id_from("fuzz/scene"), de_hash64_str(type), &h) != DE_OK)
        fz_scene_fail(target, "instantiate: no slot");
    size_t block = sizeof(de__scene_object) + size;
    de__scene_object *o =
        (de__scene_object *)DE_ALLOC(s->alloc, block, DE_ALIGNOF(de__scene_object), DE_MEM_ASSET);
    if (o == NULL)
        fz_scene_fail(target, "instantiate: no memory");
    u8 *bytes = (u8 *)(o + 1);
    if (size != 0)
        memcpy(bytes, data, size);
    if (de__scene_view_parse(bytes, size, &o->view, NULL, 0) != DE_OK)
        fz_scene_fail(target, "instantiate: the copy does not parse");
    o->alloc = s->alloc;
    o->block_size = block;
    de__slot_set_object(de__slot_get(s, h), o);

    /* A registry and a world of this input only (see the file comment). */
    de_type_registry *reg = NULL;
    if (de_type_registry_create(NULL, &reg) != DE_OK || fz_register_types(reg, 2) != DE_OK)
        fz_scene_fail(target, "instantiate: the registry");
    de_world_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.types = reg;
    de_world *w = NULL;
    if (de_world_create(&wd, &w) != DE_OK)
        fz_scene_fail(target, "instantiate: the world");
    fz_register_components(w, reg);

    de_prefab p = {h};
    de_entity root = DE_ENTITY_NULL;
    de_result r = de_prefab_instantiate(s, w, p, DE_ENTITY_NULL, &root);
    if (r == DE_OK && v.entity_count != 0 && !de_entity_alive(w, root))
        fz_scene_fail(target, "instantiate succeeded without a live root");
    if (r != DE_OK && root.bits != 0)
        fz_scene_fail(target, "instantiate failed but returned a root");
    if (r == DE_OK && v.entity_count != 0 && de_entity_parent(w, root).bits != 0)
        fz_scene_fail(target, "instantiate without a parent gave the root a parent");

    de_entity holder = de_entity_spawn(w);
    de_entity root2 = DE_ENTITY_NULL;
    de_result r2 = de_prefab_instantiate(s, w, p, holder, &root2);
    if (r2 == DE_OK && v.entity_count != 0 && de_entity_parent(w, root2).bits != holder.bits)
        fz_scene_fail(target, "instantiate under a parent did not parent the root");
    if (r2 == DE_OK && de_entity_children_count(w, holder) != v.root_count)
        fz_scene_fail(target, "instantiate under a parent did not parent every root");
    if (r2 == DE_OK) {
        fz_check_tree(target, w, holder, v.entity_count);
        de_entity_despawn_recursive(w, holder);
    }

    de_world_destroy(w);
    de_type_registry_destroy(reg);
    /* Unloads the object through the `scene` / `prefab` type (scene_unload frees the block). */
    if (de__slot_free(s, h) != DE_OK)
        fz_scene_fail(target, "instantiate: the slot did not free");
    de_error_clear();
    return r;
}
