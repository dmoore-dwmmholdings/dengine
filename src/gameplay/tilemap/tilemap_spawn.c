/*
 * tilemap_spawn.c: entity spawns of the tilemap runtime (DENG-80, SPEC §21.4). Each spawn record
 * whose type the import settings mapped to a prefab (`entities = ["Player=prefabs/player"]`) is
 * instantiated once per map instance, as a root entity at its map position in world space (the
 * map entity's transform applied), and each field record is written by reflection path
 * (`THealth.current`: the component THealth, its field `current`; the component is added when
 * the prefab has none).
 */
#include "gameplay/tilemap/tilemap_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>
#include <dengine/de_reflect.h>

#include <math.h>
#include <string.h>

static bool write_int(void *p, u32 size, bool is_signed, f64 n) {
    if (!(n == n) || n != floor(n))
        return false;
    switch (size) {
    case 1:
        if (is_signed ? (n < -128 || n > 127) : (n < 0 || n > 255))
            return false;
        if (is_signed) {
            i8 v = (i8)n;
            memcpy(p, &v, 1);
        } else {
            u8 v = (u8)n;
            memcpy(p, &v, 1);
        }
        return true;
    case 2:
        if (is_signed ? (n < -32768 || n > 32767) : (n < 0 || n > 65535))
            return false;
        if (is_signed) {
            i16 v = (i16)n;
            memcpy(p, &v, 2);
        } else {
            u16 v = (u16)n;
            memcpy(p, &v, 2);
        }
        return true;
    case 4:
        if (is_signed ? (n < -2147483648.0 || n > 2147483647.0) : (n < 0 || n > 4294967295.0))
            return false;
        if (is_signed) {
            i32 v = (i32)n;
            memcpy(p, &v, 4);
        } else {
            u32 v = (u32)n;
            memcpy(p, &v, 4);
        }
        return true;
    case 8:
        if (is_signed ? (n < -9.2233720368547758e18 || n >= 9.2233720368547758e18)
                      : (n < 0 || n >= 1.8446744073709552e19))
            return false;
        if (is_signed) {
            i64 v = (i64)n;
            memcpy(p, &v, 8);
        } else {
            u64 v = (u64)n;
            memcpy(p, &v, 8);
        }
        return true;
    default:
        return false;
    }
}

bool de__tm_apply_field(de__tilemap_sys *s, de_entity e, const de__tm_view *v,
                        const de__tm_field *f) {
    const char *path = de__tm_str(v, f->path);
    const char *dot = strchr(path, '.');
    char comp_name[128];
    size_t n = dot != NULL ? (size_t)(dot - path) : 0;
    if (dot == NULL || n == 0 || n >= sizeof comp_name || dot[1] == '\0') {
        DE_WARN(s->log, "tilemap spawn: field \"%s\" is not `Component.field`", path);
        return false;
    }
    memcpy(comp_name, path, n);
    comp_name[n] = '\0';
    de_type_registry *types = de_world_types(s->w);
    de_type_id type = de_type_find(types, comp_name);
    de_component comp = de_component_find(s->w, comp_name);
    if (!DE_HANDLE_VALID(type) || !DE_HANDLE_VALID(comp)) {
        DE_WARN(s->log, "tilemap spawn: \"%s\" is not a component of this world", comp_name);
        return false;
    }
    const de_field_info *fi = NULL;
    u32 offset = 0;
    if (de_reflect_resolve_path(types, type, dot + 1, &fi, &offset) != DE_OK || fi == NULL) {
        de_error_clear();
        DE_WARN(s->log, "tilemap spawn: \"%s\" names no field", path);
        return false;
    }
    u8 *obj = (u8 *)de_entity_get(s->w, e, comp);
    if (obj == NULL)
        obj = (u8 *)de_entity_add(s->w, e, comp);
    if (obj == NULL)
        return false;
    u8 *p = obj + offset;
    bool ok = false;
    const bool number = f->kind != DE__TM_FIELD_STRING;
    switch (fi->kind) {
    case DE_FK_BOOL:
        if (number) {
            bool b = f->number != 0.0;
            memcpy(p, &b, sizeof b);
            ok = true;
        }
        break;
    case DE_FK_I8:
    case DE_FK_I16:
    case DE_FK_I32:
    case DE_FK_I64:
        ok = number && write_int(p, fi->size, true, f->number);
        break;
    case DE_FK_U8:
    case DE_FK_U16:
    case DE_FK_U32:
    case DE_FK_U64:
        ok = number && write_int(p, fi->size, false, f->number);
        break;
    case DE_FK_F32:
        if (number) {
            f32 x = (f32)f->number;
            memcpy(p, &x, sizeof x);
            ok = true;
        }
        break;
    case DE_FK_F64:
        if (number) {
            memcpy(p, &f->number, sizeof f->number);
            ok = true;
        }
        break;
    case DE_FK_STRING:
        if (!number && fi->size >= 1) {
            const char *str = de__tm_str(v, f->string);
            size_t len = strlen(str);
            if (len < fi->size) {
                memcpy(p, str, len + 1);
                ok = true;
            }
        }
        break;
    case DE_FK_ENUM: {
        de_type_id et = fi->attrs.nested_type != NULL ? de_type_find(types, fi->attrs.nested_type)
                                                      : (de_type_id){0};
        i64 value = 0;
        if (!number && DE_HANDLE_VALID(et) &&
            de_enum_value_of(types, et, de__tm_str(v, f->string), &value))
            ok = write_int(p, fi->size, true, (f64)value);
        else if (number)
            ok = write_int(p, fi->size, true, f->number);
        break;
    }
    default:
        break;
    }
    if (!ok)
        DE_WARN(s->log, "tilemap spawn: the value of \"%s\" does not fit its field", path);
    return ok;
}

void de__tm_spawn_entities(de__tilemap_sys *s, de__tm_instance *in) {
    if (s->assets == NULL)
        return;
    const de__tm_view *v = &in->asset->view;
    const de_transform map = de__tm_entity_xform(s, in->entity);
    for (u32 i = 0; i < v->h.spawn_count; ++i) {
        const de__tm_spawn sp = de__tm_spawn_at(v, i);
        if (sp.prefab == 0)
            continue;
        de_prefab p = de_asset_prefab(s->assets, (de_asset_id){sp.prefab});
        de_entity root = DE_ENTITY_NULL;
        if (!DE_HANDLE_VALID(p) ||
            de_prefab_instantiate(s->assets, s->w, p, DE_ENTITY_NULL, &root) != DE_OK ||
            root.bits == 0) {
            DE_WARN(s->log, "tilemap spawn \"%s\": prefab %016llx not instantiated: %s",
                    de__tm_str(v, sp.name), (unsigned long long)sp.prefab,
                    de_error_last() != NULL ? de_error_last()->message : "");
            de_error_clear();
            continue;
        }
        in->spawned[in->spawned_count++] = root;
        de_transform *t = (de_transform *)de_entity_get(s->w, root, s->transform);
        if (t == NULL) {
            t = (de_transform *)de_entity_add(s->w, root, s->transform);
            if (t != NULL)
                *t = de_transform_identity();
        }
        if (t != NULL) {
            de_transform local = de_transform_identity();
            local.position = de__tm_local(s, sp.u, sp.v, 0.0);
            t->position = de_transform_point(&map, local.position);
        }
        for (u32 k = 0; k < sp.field_count; ++k) {
            de__tm_field f = de__tm_field_at(v, sp.field_first + k);
            (void)de__tm_apply_field(s, root, v, &f);
        }
    }
}
