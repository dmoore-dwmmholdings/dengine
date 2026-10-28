/*
 * typed_access.c: de_asset_texture / _mesh / ... / _blob and the generic de__asset_handle_of
 * (SPEC §11.1, §11.3; DENG-140).
 *
 * A typed accessor returns the handle of the asset's slot, creating the slot on first use, so a
 * game can keep the handle before the asset is loaded; the data appears behind it when it is
 * (§11.3). Each accessor checks the asset's declared type against its own type name.
 */

#include "asset/slots_internal.h"

#include "asset/asset_internal.h"
#include "asset/budget_internal.h"     /* DENG-144 */
#include "asset/cook_stage_internal.h" /* DENG-209 */
#include "asset/mount_internal.h"      /* DENG-212 */
#include "asset/resolve_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

#include <string.h>

/* DENG-81: this explicit builtin alias shares the DSCP loader/object and de_anim_clip
 * handle family. Reflected sprite_clip references must resolve a primary Aseprite too.
 * No other type is widened, and requesting the narrower aseprite type remains exact. */
static bool type_matches(u64 actual, u64 wanted) {
    return actual == wanted ||
           (wanted == de_hash64_str("sprite_clip") && actual == de_hash64_str("aseprite"));
}

u32 de__asset_handle_of(de_asset_system *s, de_asset_id id, const char *type_name) {
    if (s == NULL || id.hash == 0) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "asset handle: NULL system or asset ID 0");
        return 0;
    }
    u64 want = type_name != NULL ? de_hash64_str(type_name) : 0u;

    u32 h = de__slot_find(s, id);
    if (h != 0) {
        const de__slot *sl = de__slot_get(s, h);
        if (want != 0 && !type_matches(sl->type_hash, want)) {
            const de__manifest_asset *ma = de__resolved_asset(s->resolved, id);
            const de_asset_type_desc *td = de__asset_type_get(s, de__slot_type(s, sl));
            const char *have = ma != NULL ? ma->type : td != NULL ? td->name : "another type";
            (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "asset %s%s%016llx is a %s, not a %s",
                          ma != NULL ? ma->id : "", ma != NULL ? " " : "",
                          (unsigned long long)id.hash, have, type_name);
            return 0;
        }
        de__budget_touch(s, h); /* DENG-144: a typed access is a use (LRU) */
        return h;
    }

    const de__manifest_asset *ma = de__resolved_asset(s->resolved, id);
    de__mount_entry pe;
    if (ma == NULL && de__mount_lookup(s, id, &pe)) {
        /* DENG-212: an asset of a mounted pack (PACKED mode has no manifest): its TOC type. */
        if (want != 0 && !type_matches(pe.toc.type_hash, want)) {
            (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "asset %016llx of pack \"%s\" is not a %s",
                          (unsigned long long)id.hash, pe.pack->vpath, type_name);
            return 0;
        }
        if (de__slot_create(s, id, pe.toc.type_hash, &h) != DE_OK)
            return 0;
        return h;
    }
    u64 sub_type = 0;
    if (ma == NULL && de__cook_stage_subasset_type(s, id, &sub_type)) {
        /* DENG-209: a sub-asset an in-process cook declared (cook_stage.c). */
        if (want != 0 && !type_matches(sub_type, want)) {
            (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "sub-asset %016llx is of another type, not a %s",
                          (unsigned long long)id.hash, type_name);
            return 0;
        }
        if (de__slot_create(s, id, sub_type, &h) != DE_OK)
            return 0;
        return h;
    }
    if (ma == NULL) {
        (void)DE_FAIL(DE_ERR_NOT_FOUND, "asset %016llx is not in the asset manifest",
                      (unsigned long long)id.hash);
        return 0;
    }
    if (type_name != NULL && !type_matches(de_hash64_str(ma->type), want)) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "asset \"%s\" is a %s (%s), not a %s", ma->id,
                      ma->type, ma->file, type_name);
        return 0;
    }
    if (de__slot_create(s, id, de_hash64_str(ma->type), &h) != DE_OK)
        return 0;
    return h;
}

void *de__asset_object(de_asset_system *s, u32 handle_id) {
    const de__slot *sl = de__slot_get(s, handle_id);
    return sl != NULL ? de__slot_object(sl) : NULL;
}

/* The type names match the manifest's `type = "..."` values (SPEC §11.2, examples/assets.toml)
 * and the modules that register them (S08+). */
de_texture de_asset_texture(de_asset_system *s, de_asset_id id) {
    return (de_texture){de__asset_handle_of(s, id, "texture")};
}
de_mesh de_asset_mesh(de_asset_system *s, de_asset_id id) {
    return (de_mesh){de__asset_handle_of(s, id, "mesh")};
}
de_model de_asset_model(de_asset_system *s, de_asset_id id) {
    return (de_model){de__asset_handle_of(s, id, "model")};
}
de_material de_asset_material(de_asset_system *s, de_asset_id id) {
    return (de_material){de__asset_handle_of(s, id, "material")};
}
de_sound de_asset_sound(de_asset_system *s, de_asset_id id) {
    return (de_sound){de__asset_handle_of(s, id, "sound")};
}
de_font de_asset_font(de_asset_system *s, de_asset_id id) {
    return (de_font){de__asset_handle_of(s, id, "font")};
}
de_prefab de_asset_prefab(de_asset_system *s, de_asset_id id) {
    return (de_prefab){de__asset_handle_of(s, id, "prefab")};
}
de_blob de_asset_blob(de_asset_system *s, de_asset_id id) {
    return (de_blob){de__asset_handle_of(s, id, "blob")};
}
de_shader de_asset_shader(de_asset_system *s, de_asset_id id) {
    return (de_shader){de__asset_handle_of(s, id, "shader")};
}

/* DENG-105: the `prefab` dependency of a READY model (its `#prefab` sub-asset). */
de_prefab de_model_prefab(de_asset_system *s, de_model m) {
    const de__slot *sl = s != NULL ? de__slot_get(s, m.id) : NULL;
    if (sl == NULL || sl->state != DE_LOAD_READY || sl->type_hash != de_hash64_str("model"))
        return (de_prefab){0};
    const u64 prefab = de_hash64_str("prefab");
    for (u32 i = 0; i < sl->dep_count; ++i) {
        u32 h = de__slot_find(s, sl->deps[i]);
        const de__slot *d = h != 0 ? de__slot_get(s, h) : NULL;
        if (d != NULL && d->type_hash == prefab)
            return (de_prefab){h};
    }
    return (de_prefab){0};
}

/* DENG-264 */
de_string_table de_asset_string_table(de_asset_system *s, de_asset_id id) {
    return (de_string_table){de__asset_handle_of(s, id, "string_table")};
}
