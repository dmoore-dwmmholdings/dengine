/*
 * scene_instantiate.c: de_prefab_instantiate and de_asset_scene (SPEC §9.2, §11.1; DENG-215).
 *
 * Immediate mode, on the main thread, outside running systems (the ECS asserts that for its
 * structural calls): spawning from inside a system needs a command-buffer form (later task).
 *
 * Steps: map each type of the scene's type table to a component of the world (by name), spawn
 * every entity with its components in one row (de_entity_spawn_with), read each component's
 * binary object into its row (entity fields map scene IDs to the spawned entities, asset fields
 * become typed handles), drop the components that `migrate` refused, then link parents with
 * de_entity_set_parent in scene-ID order. The scene-ID -> entity map exists only during the call:
 * the entities array, indexed like the scene's entity records (sorted by scene ID, so a lookup is
 * a binary search). Temporaries live in thread scratch, so instantiating allocates nothing from
 * the heap except what the ECS itself needs.
 */

#include "asset/scene/scene_format.h"

#include "asset/asset_internal.h"
#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>

#include "ecs/ecs_internal.h" /* ecs_component_get: a component's type and whether it is a tag */

#include <string.h>

de_prefab de_asset_scene(de_asset_system *s, de_asset_id id) {
    return (de_prefab){de__asset_handle_of(s, id, DE__SCENE_TYPE_NAME)};
}

typedef struct inst_ctx {
    de_asset_system *s;
    const de__scene_view *v;
    const de_entity *ents;
} inst_ctx;

static u64 inst_scene_id_to_entity(u64 scene_id, void *user) {
    const inst_ctx *c = (const inst_ctx *)user;
    if (scene_id == 0)
        return 0;
    u32 i = de__scene_find_entity(c->v, scene_id);
    return i != DE__SCENE_NO_PARENT ? c->ents[i].bits : 0u;
}
static u64 inst_entity_to_scene_id(u64 bits, void *user) {
    (void)user;
    return bits;
}
static u32 inst_id_to_asset(u64 id, const char *asset_type, void *user) {
    const inst_ctx *c = (const inst_ctx *)user;
    if (id == 0)
        return 0;
    u32 h = de__asset_handle_of(c->s, (de_asset_id){id}, asset_type);
    if (h == 0)
        DE_WARN(c->s->log, "prefab: asset reference %016llx (%s) has no handle; it stays 0",
                (unsigned long long)id, asset_type != NULL ? asset_type : "any type");
    return h;
}
static u64 inst_asset_to_id(u32 handle, const char *asset_type, void *user) {
    (void)handle;
    (void)asset_type;
    (void)user;
    return 0;
}

typedef struct inst_type {
    de_component comp; /* invalid: skipped */
    de_type_id type;
    bool tag;
} inst_type;

typedef struct inst_drop {
    u32 entity;
    de_component comp;
} inst_drop;

static void inst_rollback(de_world *w, const de_entity *ents, u32 n) {
    for (u32 i = n; i-- > 0;)
        if (ents[i].bits != 0)
            de_entity_despawn(w, ents[i]);
}

de_result de_prefab_instantiate(de_asset_system *s, de_world *w, de_prefab p, de_entity parent,
                                de_entity *out_root) {
    if (out_root != NULL)
        *out_root = DE_ENTITY_NULL;
    if (s == NULL || w == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_prefab_instantiate: NULL system or world");
    const de__slot *sl = de__slot_get(s, p.id);
    if (sl == NULL || (sl->type_hash != de_hash64_str(DE__SCENE_TYPE_NAME) &&
                       sl->type_hash != de_hash64_str(DE__PREFAB_TYPE_NAME)))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_prefab_instantiate: handle 0x%x is not a prefab or scene",
                       (unsigned)p.id);
    const de__scene_object *o = (const de__scene_object *)de__slot_object(sl);
    if (o == NULL)
        return DE_FAIL(DE_ERR_NOT_FOUND, "de_prefab_instantiate: asset %016llx is not loaded",
                       (unsigned long long)sl->id.hash);
    if (parent.bits != 0 && !de_entity_alive(w, parent))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_prefab_instantiate: parent is not alive");
    const de__scene_view *v = &o->view;
    const de_type_registry *reg = de_world_types(w);

    de_scratch sc = de_scratch_begin(NULL, 0);
    inst_type *types = v->type_count ? DE_ARENA_PUSH(sc.arena, inst_type, v->type_count) : NULL;
    de_entity *ents = v->entity_count ? DE_ARENA_PUSH(sc.arena, de_entity, v->entity_count) : NULL;
    de_component *list =
        v->component_count ? DE_ARENA_PUSH(sc.arena, de_component, v->component_count) : NULL;
    inst_drop *drops =
        v->component_count ? DE_ARENA_PUSH(sc.arena, inst_drop, v->component_count) : NULL;
    u32 drop_count = 0;
    if ((v->type_count != 0 && types == NULL) || (v->entity_count != 0 && ents == NULL) ||
        (v->component_count != 0 && (list == NULL || drops == NULL))) {
        de_scratch_end(sc);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_prefab_instantiate: no scratch memory");
    }
    de_result r = DE_OK;
    if (v->entity_count != 0)
        memset(ents, 0, sizeof(de_entity) * v->entity_count);

    /* The scene's types as components of this world. */
    for (u32 t = 0; t < v->type_count; ++t) {
        de__scene_type_rec tr = de__scene_type_at(v, t);
        types[t].comp = de_component_find(w, tr.name);
        types[t].type = (de_type_id){0};
        types[t].tag = false;
        const ecs_component_info *ci =
            DE_HANDLE_VALID(types[t].comp) ? ecs_component_get(w, types[t].comp.id) : NULL;
        if (ci == NULL) {
            types[t].comp = (de_component){0};
            DE_WARN(s->log, "prefab %016llx: `%s` is not a component of the world; skipped",
                    (unsigned long long)sl->id.hash, tr.name);
            continue;
        }
        types[t].type = ci->type;
        types[t].tag = ci->size == 0;
    }

    /* Spawn every entity in the archetype of its components. */
    for (u32 i = 0; i < v->entity_count && r == DE_OK; ++i) {
        de__scene_entity_rec e = de__scene_entity_at(v, i);
        u32 n = 0;
        for (u32 k = 0; k < e.component_count; ++k) {
            de__scene_comp_rec c = de__scene_comp_at(v, e.first_component + k);
            if (DE_HANDLE_VALID(types[c.type_index].comp))
                list[n++] = types[c.type_index].comp;
        }
        r = de_entity_spawn_with(w, list, n, &ents[i]);
    }

    /* Read the components. Pointers into the tables stay valid: no structural change here. */
    inst_ctx ictx = {s, v, ents};
    de_ser_context sctx = {inst_entity_to_scene_id, inst_scene_id_to_entity, inst_asset_to_id,
                           inst_id_to_asset, &ictx};
    de_ser_options opt = {DE_SER_BINARY, DE_FIELD_SERIALIZE, &sctx};
    for (u32 i = 0; i < v->entity_count && r == DE_OK; ++i) {
        de__scene_entity_rec e = de__scene_entity_at(v, i);
        for (u32 k = 0; k < e.component_count && r == DE_OK; ++k) {
            de__scene_comp_rec c = de__scene_comp_at(v, e.first_component + k);
            const inst_type *it = &types[c.type_index];
            if (!DE_HANDLE_VALID(it->comp) || it->tag)
                continue;
            void *data = de_entity_get(w, ents[i], it->comp);
            de_mem_reader mr;
            de_mem_reader_init(&mr, c.data, c.size);
            de_reader rd = de_reader_from_mem(&mr);
            de_result cr = de_deserialize_binary(reg, it->type, data, &opt, NULL, &rd);
            if (cr == DE_ERR_VERSION_MISMATCH) {
                /* SPEC §9.2: a newer stored version, or `migrate` dropped it: skip the object. */
                const de_error *er = de_error_last();
                DE_WARN(s->log, "prefab %016llx: entity %016llx: component `%s` dropped: %s",
                        (unsigned long long)sl->id.hash, (unsigned long long)e.scene_id,
                        de__scene_type_at(v, c.type_index).name,
                        er != NULL ? er->message : "version mismatch");
                drops[drop_count].entity = i;
                drops[drop_count].comp = it->comp;
                drop_count++;
            } else if (cr != DE_OK) {
                r = DE_FAIL_WRAP(cr, "prefab %016llx: entity %016llx: component `%s`",
                                 (unsigned long long)sl->id.hash, (unsigned long long)e.scene_id,
                                 de__scene_type_at(v, c.type_index).name);
            }
        }
    }
    for (u32 i = 0; i < drop_count && r == DE_OK; ++i)
        de_entity_remove(w, ents[drops[i].entity], drops[i].comp);

    /* Hierarchy: children in scene-ID order; the roots under `parent`. */
    for (u32 i = 0; i < v->entity_count && r == DE_OK; ++i) {
        de__scene_entity_rec e = de__scene_entity_at(v, i);
        if (e.parent != DE__SCENE_NO_PARENT)
            r = de_entity_set_parent(w, ents[i], ents[e.parent]);
        else if (parent.bits != 0)
            r = de_entity_set_parent(w, ents[i], parent);
        if (out_root != NULL && e.parent == DE__SCENE_NO_PARENT && out_root->bits == 0)
            *out_root = ents[i];
    }

    if (r != DE_OK) {
        inst_rollback(w, ents, v->entity_count);
        if (out_root != NULL)
            *out_root = DE_ENTITY_NULL;
    }
    de_scratch_end(sc);
    return r;
}
