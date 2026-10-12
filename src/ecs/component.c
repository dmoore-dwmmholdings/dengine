/* component.c: the component registry (DENG-344, SPEC §7.1).
 *
 * Components are reflected types. Ids are dense and given in registration order, so they are part
 * of the determinism contract. The two hash maps (by name, by type id) serve lookups only and are
 * never iterated. */
#include "ecs_internal.h"

#define ECS_COMPONENT_FLAGS_ALL                                                                    \
    ((u32)(DE_COMP_SNAPSHOT | DE_COMP_REPLICATE | DE_COMP_SERIALIZE | DE_COMP_TAG))

static u64 type_key_hash(u32 type_id) {
    return de_hash64(&type_id, sizeof type_id, 0);
}

/* Bytes one row of a single-component archetype needs: entity, data, two ticks, and the worst
 * alignment padding of the data. A component that does not fit one chunk can never be stored. */
static u64 single_row_bytes(u32 size, u32 align) {
    return sizeof(de_entity) + (u64)size + (u64)(align - 1u) + 2u * sizeof(u32);
}

de_component de_component_register(de_world *w, de_type_id type, u32 flags) {
    const de_component invalid = {0};
    if (w == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_component_register: world is NULL");
        return invalid;
    }
    ECS_ASSERT_STRUCTURAL(w);
    if ((flags & ~ECS_COMPONENT_FLAGS_ALL) != 0) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_component_register: unknown flag bits 0x%x",
                flags & ~ECS_COMPONENT_FLAGS_ALL);
        return invalid;
    }
    const de_type_desc *desc = de_type_get(w->types, type);
    if (desc == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                "de_component_register: type id 0x%x is not a struct type of the world's "
                "registry",
                type.id);
        return invalid;
    }

    const u64 type_hash = type_key_hash(type.id);
    const u32 *existing = DE_HASHMAP_GET(&w->component_by_type, u32, type_hash, &type.id);
    if (existing != NULL) {
        const ecs_component_info *info = ecs_component_get(w, *existing);
        if (info->flags != flags) {
            DE_FAIL(DE_ERR_ALREADY_EXISTS,
                    "de_component_register: type '%s' is already a component with flags 0x%x "
                    "(asked for 0x%x)",
                    desc->name, info->flags, flags);
            return invalid;
        }
        return (de_component){*existing};
    }

    const bool tag = (flags & DE_COMP_TAG) != 0;
    ecs_component_info info;
    memset(&info, 0, sizeof info);
    info.type = type;
    info.name = desc->name;
    info.flags = flags;
    info.is_pod = desc->is_pod;
    if (tag) {
        info.size = 0;
        info.align = 1;
    } else {
        info.size = desc->size;
        info.align = desc->align ? desc->align : 1u;
        info.hooks = desc->hooks;
        if (!desc->is_pod && (desc->hooks.copy == NULL || desc->hooks.destruct == NULL)) {
            DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                    "de_component_register: type '%s' is neither POD nor has copy and destruct "
                    "hooks (SPEC 7.1)",
                    desc->name);
            return invalid;
        }
        if (info.align > DE_ECS_MAX_COMPONENT_ALIGN) {
            DE_FAIL(DE_ERR_UNSUPPORTED,
                    "de_component_register: type '%s' has alignment %u; the most is %u", desc->name,
                    info.align, DE_ECS_MAX_COMPONENT_ALIGN);
            return invalid;
        }
        if (single_row_bytes(info.size, info.align) > w->chunk_bytes) {
            DE_FAIL(DE_ERR_UNSUPPORTED,
                    "de_component_register: type '%s' (%u bytes) does not fit a %u-byte chunk",
                    desc->name, info.size, w->chunk_bytes);
            return invalid;
        }
    }
    if (w->components.count >= 0x7FFFFFFFu) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_component_register: too many components");
        return invalid;
    }

    const u32 id = w->components.count + 1u;
    const u64 name_hash = de_hash64_str(desc->name);
    /* Append, then index; undo what was done when a map cannot grow. */
    if (de_array_push(&w->components, &info) != DE_OK) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_component_register: out of memory");
        return invalid;
    }
    if (de_hashmap_put(&w->component_by_type, type_hash, &type.id, &id) != DE_OK) {
        de_array_pop(&w->components, NULL);
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_component_register: out of memory");
        return invalid;
    }
    /* Names are unique in a type registry, so this never replaces another component. */
    if (de_hashmap_put(&w->component_by_name, name_hash, &name_hash, &id) != DE_OK) {
        de_hashmap_remove(&w->component_by_type, type_hash, &type.id, NULL);
        de_array_pop(&w->components, NULL);
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_component_register: out of memory");
        return invalid;
    }
    return (de_component){id};
}

de_component de_component_find(const de_world *w, const char *type_name) {
    const de_component invalid = {0};
    if (w == NULL || type_name == NULL)
        return invalid;
    const u64 name_hash = de_hash64_str(type_name);
    const u32 *id = DE_HASHMAP_GET(&w->component_by_name, u32, name_hash, &name_hash);
    if (id == NULL)
        return invalid;
    /* Guard against a 64-bit hash collision with another name. */
    const ecs_component_info *info = ecs_component_get(w, *id);
    if (strcmp(info->name, type_name) != 0)
        return invalid;
    return (de_component){*id};
}
