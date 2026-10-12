/* builtin_components.c: the built-in components DeTransform, DeGlobalTransform, DeChildOf and
 * DeChildren, their lazy registration and the world's hierarchy state (DENG-351, SPEC §7.1).
 *
 * Registration is lazy (de_ecs.h, "Hierarchy and built-in components"): the shared test fixture
 * and the golden vectors of the other S05 tests depend on the first registered component having
 * id 1, and a world that never uses transforms gains no components and no system. See the Trama
 * page "Decision: ECS built-in components are registered on first use". */
#include "hierarchy_internal.h"

#include "system_internal.h" /* registry->running: no registration while a schedule runs */

/* Transforms are constructed as the identity: a zeroed de_transform has scale 0. */
static void construct_identity(void *obj) {
    *(de_transform *)obj = de_transform_identity();
}

static const de_field_info k_transform_fields[] = {
    DE_FIELD(de_transform, position, DE_FK_DVEC3, DE_FIELD_DEFAULT),
    DE_FIELD(de_transform, rotation, DE_FK_QUAT, DE_FIELD_DEFAULT),
    DE_FIELD(de_transform, scale, DE_FK_VEC3, DE_FIELD_DEFAULT),
};
/* Derived data: read-only for the editor, not serialized or snapshotted. */
static const de_field_info k_global_fields[] = {
    DE_FIELD(de_transform, position, DE_FK_DVEC3, DE_FIELD_READONLY),
    DE_FIELD(de_transform, rotation, DE_FK_QUAT, DE_FIELD_READONLY),
    DE_FIELD(de_transform, scale, DE_FK_VEC3, DE_FIELD_READONLY),
};
/* Links change only through the hierarchy API: read-only for the editor. */
static const de_field_info k_child_of_fields[] = {
    DE_FIELD(DeChildOf, parent, DE_FK_ENTITY, DE_FIELD_DEFAULT | DE_FIELD_READONLY),
    DE_FIELD(DeChildOf, next_sibling, DE_FK_ENTITY, DE_FIELD_DEFAULT | DE_FIELD_READONLY),
    DE_FIELD(DeChildOf, prev_sibling, DE_FK_ENTITY, DE_FIELD_DEFAULT | DE_FIELD_READONLY),
};
static const de_field_info k_children_fields[] = {
    DE_FIELD(DeChildren, first, DE_FK_ENTITY, DE_FIELD_DEFAULT | DE_FIELD_READONLY),
    DE_FIELD(DeChildren, last, DE_FK_ENTITY, DE_FIELD_DEFAULT | DE_FIELD_READONLY),
    DE_FIELD(DeChildren, count, DE_FK_U32, DE_FIELD_DEFAULT | DE_FIELD_READONLY),
};

typedef struct builtin_def {
    const char *name;
    u32 size, align;
    const de_field_info *fields;
    u32 field_count;
    bool identity;  /* construct hook: the identity transform */
    u32 comp_flags; /* de_component_flags */
} builtin_def;

#define BUILTIN_COMP_FLAGS (DE_COMP_SNAPSHOT | DE_COMP_SERIALIZE)

/* Registration order = component id order: transform, global, child_of, children. */
static const builtin_def k_builtins[4] = {
    {"DeTransform", (u32)sizeof(DeTransform), (u32)DE_ALIGNOF(DeTransform), k_transform_fields,
     (u32)(sizeof k_transform_fields / sizeof k_transform_fields[0]), true, BUILTIN_COMP_FLAGS},
    {"DeGlobalTransform", (u32)sizeof(DeGlobalTransform), (u32)DE_ALIGNOF(DeGlobalTransform),
     k_global_fields, (u32)(sizeof k_global_fields / sizeof k_global_fields[0]), true, 0u},
    {"DeChildOf", (u32)sizeof(DeChildOf), (u32)DE_ALIGNOF(DeChildOf), k_child_of_fields,
     (u32)(sizeof k_child_of_fields / sizeof k_child_of_fields[0]), false, BUILTIN_COMP_FLAGS},
    {"DeChildren", (u32)sizeof(DeChildren), (u32)DE_ALIGNOF(DeChildren), k_children_fields,
     (u32)(sizeof k_children_fields / sizeof k_children_fields[0]), false, BUILTIN_COMP_FLAGS},
};

static de_component register_builtin(de_world *w, const builtin_def *d) {
    de_type_desc td;
    memset(&td, 0, sizeof td);
    td.name = d->name;
    td.size = d->size;
    td.align = d->align;
    td.version = 1;
    td.fields = d->fields;
    td.field_count = d->field_count;
    td.hooks.construct = d->identity ? construct_identity : NULL;
    td.is_pod = true;
    /* Registering the same name and layout again (another world on the same registry) keeps the
     * type id; a different layout under the name is a user type in the way. */
    const de_type_id existing = de_type_find(w->types, d->name);
    if (DE_HANDLE_VALID(existing)) {
        const de_type_desc *cur = de_type_get(w->types, existing);
        if (cur == NULL || cur->size != d->size || cur->field_count != d->field_count) {
            DE_FAIL(DE_ERR_ALREADY_EXISTS,
                    "ecs: built-in type %s: the type registry has another type of that name",
                    d->name);
            return (de_component){0};
        }
    }
    const de_type_id t = de_type_register(w->types, &td);
    if (!DE_HANDLE_VALID(t))
        return (de_component){0};
    return de_component_register(w, t, d->comp_flags);
}

de_result ecs_builtins_ensure(de_world *w) {
    if (w->hier != NULL)
        return DE_OK;
    if (w->in_schedule || (w->systems != NULL && w->systems->running))
        return DE_FAIL(DE_ERR_BUSY, "ecs: the built-in components are registered on first use, "
                                    "which is not allowed while a schedule runs; call "
                                    "de_builtin_components() during setup");
    de_component ids[4];
    for (u32 i = 0; i < 4u; ++i) {
        ids[i] = register_builtin(w, &k_builtins[i]);
        if (!DE_HANDLE_VALID(ids[i]))
            return de_error_last() != NULL ? de_error_last()->result : DE_ERR_INTERNAL;
    }
    ecs_hier_state *st = DE_NEW(w->a, ecs_hier_state, DE_MEM_ECS);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory for the hierarchy state");
    memset(st, 0, sizeof *st);
    st->ids.transform = ids[0];
    st->ids.global_transform = ids[1];
    st->ids.child_of = ids[2];
    st->ids.children = ids[3];
    de_atomic_init_u64(&st->runs, 0);
    de_atomic_init_u64(&st->visited, 0);
    de_atomic_init_u64(&st->recomputed, 0);
    de_atomic_init_u64(&st->globals_added, 0);
    /* The system reads the ids from w->hier, so the state goes in first. */
    w->hier = st;
    const de_result r = ecs_transform_propagate_register(w, &st->propagate);
    if (r != DE_OK) {
        w->hier = NULL;
        DE_FREE(w->a, st, sizeof *st);
        return r;
    }
    return DE_OK;
}

void ecs_hier_destroy(de_world *w) {
    if (w->hier == NULL)
        return;
    DE_FREE(w->a, w->hier, sizeof *w->hier);
    w->hier = NULL;
}

void ecs_hier_stats_get(const de_world *w, ecs_hier_stats *out) {
    memset(out, 0, sizeof *out);
    if (w == NULL || w->hier == NULL)
        return;
    const ecs_hier_state *st = w->hier;
    out->runs = de_atomic_load_u64(&st->runs, DE_ATOMIC_RELAXED);
    out->visited = de_atomic_load_u64(&st->visited, DE_ATOMIC_RELAXED);
    out->recomputed = de_atomic_load_u64(&st->recomputed, DE_ATOMIC_RELAXED);
    out->globals_added = de_atomic_load_u64(&st->globals_added, DE_ATOMIC_RELAXED);
}

/* ---- Public API ------------------------------------------------------------------------------ */

de_builtin_component_ids de_builtin_components(de_world *w) {
    de_builtin_component_ids none;
    memset(&none, 0, sizeof none);
    if (w == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_builtin_components: world is NULL");
        return none;
    }
    if (w->hier == NULL && ecs_builtins_ensure(w) != DE_OK)
        return none;
    return w->hier->ids;
}
