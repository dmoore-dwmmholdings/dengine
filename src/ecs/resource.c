/* resource.c: typed singleton resources of a world (DENG-348, SPEC §7.1).
 *
 * One slot per resource type, in first-insert order (resource id = slot index + 1). A slot stays
 * bound to its type for the life of the world, so an id never names another type: removing a
 * resource frees its value and leaves the slot empty, and inserting the type again refills the
 * same slot. Each value is its own allocation, aligned to the type, so a pointer from
 * de_resource_get() survives inserts of other resources and replacing the value.
 *
 * The type -> slot hash map serves lookups only and is never iterated (SPEC §5.7 rule 5); world
 * teardown walks the dense slot array. */
#include "ecs_internal.h"

static u64 type_key_hash(u32 type_id) {
    return de_hash64(&type_id, sizeof type_id, 0);
}

static ecs_resource *resource_slot(const de_world *w, de_resource_id id) {
    if (id.id == 0 || id.id > w->resources.count)
        return NULL;
    return (ecs_resource *)de_array_at(&w->resources, id.id - 1u);
}

/* Writes a new value into `dst`: a copy of `init` (copy hook, or memcpy for POD), or a constructed
 * value (construct hook, or zero) when `init` is NULL. */
static void resource_fill(const ecs_resource *r, void *dst, const void *init) {
    if (init != NULL) {
        if (r->hooks.copy != NULL)
            r->hooks.copy(dst, init);
        else
            memcpy(dst, init, r->size);
    } else if (r->hooks.construct != NULL) {
        r->hooks.construct(dst);
    } else {
        memset(dst, 0, r->size);
    }
}

static void resource_free_value(de_world *w, ecs_resource *r) {
    if (r->data == NULL)
        return;
    if (r->hooks.destruct != NULL)
        r->hooks.destruct(r->data);
    DE_FREE(w->a, r->data, r->size);
    r->data = NULL;
}

de_result ecs_resources_init(de_world *w) {
    DE_ARRAY_INIT_TAG(&w->resources, ecs_resource, w->a, DE_MEM_ECS);
    const de_hashmap_desc by_type = {
        .allocator = w->a, .key_size = sizeof(u32), .value_size = sizeof(u32), .tag = DE_MEM_ECS};
    return de_hashmap_init(&w->resource_by_type, &by_type);
}

void ecs_resources_destroy(de_world *w) {
    /* Reverse first-insert order: a resource inserted later may refer to an earlier one. */
    for (u32 i = w->resources.count; i > 0; --i)
        resource_free_value(w, (ecs_resource *)de_array_at(&w->resources, i - 1u));
    de_array_free(&w->resources);
    de_hashmap_free(&w->resource_by_type);
}

de_resource_id de_resource_insert(de_world *w, de_type_id type, const void *init) {
    const de_resource_id invalid = {0};
    if (w == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_resource_insert: world is NULL");
        return invalid;
    }
    const de_type_desc *desc = de_type_get(w->types, type);
    if (desc == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                "de_resource_insert: type id 0x%x is not a struct type of the world's registry",
                type.id);
        return invalid;
    }

    const u64 hash = type_key_hash(type.id);
    const u32 *existing = DE_HASHMAP_GET(&w->resource_by_type, u32, hash, &type.id);
    if (existing != NULL) {
        const de_resource_id id = {*existing};
        ecs_resource *r = resource_slot(w, id);
        if (init != NULL && !r->is_pod && r->hooks.copy == NULL) {
            DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                    "de_resource_insert: type '%s' is neither POD nor has a copy hook, so it "
                    "cannot be copied from `init`",
                    desc->name);
            return invalid;
        }
        if (r->data != NULL) {
            /* Replace in place: the pointer stays valid. Re-inserting the value itself is a
             * no-op (destructing it first would leave nothing to copy from). */
            if (init == r->data)
                return id;
            if (r->hooks.destruct != NULL)
                r->hooks.destruct(r->data);
            resource_fill(r, r->data, init);
            return id;
        }
        void *data = DE_ALLOC(w->a, r->size, r->align, DE_MEM_ECS);
        if (data == NULL) {
            DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_resource_insert: out of memory for '%s'", desc->name);
            return invalid;
        }
        resource_fill(r, data, init);
        r->data = data;
        return id;
    }

    /* A new slot. */
    ecs_resource r;
    memset(&r, 0, sizeof r);
    r.type = type;
    r.size = desc->size;
    r.align = desc->align ? desc->align : 1u;
    r.is_pod = desc->is_pod;
    r.hooks = desc->hooks;
    if (init != NULL && !r.is_pod && r.hooks.copy == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                "de_resource_insert: type '%s' is neither POD nor has a copy hook, so it cannot "
                "be copied from `init`",
                desc->name);
        return invalid;
    }
    if (r.size == 0) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_resource_insert: type '%s' has size 0", desc->name);
        return invalid;
    }
    if (r.align > DE_ECS_MAX_COMPONENT_ALIGN) {
        DE_FAIL(DE_ERR_UNSUPPORTED,
                "de_resource_insert: type '%s' has alignment %u; the most is %u", desc->name,
                r.align, DE_ECS_MAX_COMPONENT_ALIGN);
        return invalid;
    }
    if (w->resources.count >= 0x7FFFFFFFu) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_resource_insert: too many resources");
        return invalid;
    }

    void *data = DE_ALLOC(w->a, r.size, r.align, DE_MEM_ECS);
    if (data == NULL) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_resource_insert: out of memory for '%s'", desc->name);
        return invalid;
    }
    const u32 id = w->resources.count + 1u;
    /* Index first, then fill: a failure leaves no trace and runs no hook. */
    if (de_array_push(&w->resources, &r) != DE_OK) {
        DE_FREE(w->a, data, r.size);
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_resource_insert: out of memory");
        return invalid;
    }
    if (de_hashmap_put(&w->resource_by_type, hash, &type.id, &id) != DE_OK) {
        de_array_pop(&w->resources, NULL);
        DE_FREE(w->a, data, r.size);
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_resource_insert: out of memory");
        return invalid;
    }
    ecs_resource *slot = (ecs_resource *)de_array_at(&w->resources, id - 1u);
    resource_fill(slot, data, init);
    slot->data = data;
    return (de_resource_id){id};
}

void *de_resource_get(de_world *w, de_resource_id id) {
    if (w == NULL)
        return NULL;
    const ecs_resource *r = resource_slot(w, id);
    return r != NULL ? r->data : NULL;
}

de_resource_id de_resource_find(const de_world *w, de_type_id type) {
    const de_resource_id invalid = {0};
    if (w == NULL)
        return invalid;
    const u32 *id = DE_HASHMAP_GET(&w->resource_by_type, u32, type_key_hash(type.id), &type.id);
    if (id == NULL)
        return invalid;
    const de_resource_id rid = {*id};
    return resource_slot(w, rid)->data != NULL ? rid : invalid;
}

void de_resource_remove(de_world *w, de_resource_id id) {
    if (w == NULL)
        return;
    ecs_resource *r = resource_slot(w, id);
    if (r != NULL)
        resource_free_value(w, r);
}
