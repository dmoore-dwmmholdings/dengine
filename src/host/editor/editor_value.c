/* editor_value.c: reflected values to and from MessagePack (DENG-625, DENG-626).
 *
 * A struct is a map of field name to value. Vectors, quaternions, colours and matrices are
 * arrays of numbers; an enum is its value's name; flags are a list of names; an entity is its
 * u64 bits; a fixed array is a list; a nested struct is a map. */
#include "host/editor/editor_internal.h"

#include "asset/asset_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"

#include <string.h>

/* Sub-asset paths seen so far (de__ed_note_asset_paths), by ID hash, in a fixed store. */
#define ED_SUBPATHS     1024u
#define ED_SUBPATH_TEXT (64u * 1024u)

static struct {
    u64 hash[ED_SUBPATHS];
    u32 at[ED_SUBPATHS];
    u32 count, used;
    char text[ED_SUBPATH_TEXT];
} g_sub;

static void note_path(const char *p, size_t n) {
    if (n == 0 || n >= 512 || memchr(p, '#', n) == NULL)
        return;
    const u64 h = de_hash64(p, n, 0);
    for (u32 i = 0; i < g_sub.count; ++i)
        if (g_sub.hash[i] == h)
            return;
    if (g_sub.count == ED_SUBPATHS || g_sub.used + n + 1 > ED_SUBPATH_TEXT)
        return;
    memcpy(g_sub.text + g_sub.used, p, n);
    g_sub.text[g_sub.used + n] = '\0';
    g_sub.hash[g_sub.count] = h;
    g_sub.at[g_sub.count++] = g_sub.used;
    g_sub.used += (u32)n + 1;
}

void de__ed_note_asset_paths(const char *text, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (text[i] != '"')
            continue;
        size_t j = i + 1;
        while (j < n && text[j] != '"' && text[j] != '\n')
            ++j;
        if (j < n && text[j] == '"')
            note_path(text + i + 1, j - i - 1);
        i = j;
    }
}

const char *de__ed_asset_path(de__editor *ed, u32 handle) {
    de_asset_system *s = ed->e->assets;
    if (s == NULL || handle == 0)
        return NULL;
    const de__slot *sl = de__slot_get(s, handle);
    if (sl == NULL || sl->id.hash == 0)
        return NULL;
    const char *p = s->resolved != NULL ? de__resolved_path(s->resolved, sl->id) : NULL;
    for (u32 i = 0; p == NULL && i < g_sub.count; ++i)
        if (g_sub.hash[i] == sl->id.hash)
            p = g_sub.text + g_sub.at[i];
    return p;
}

u32 de__ed_asset_handle(de__editor *ed, const char *path, const char *type) {
    de_asset_system *s = ed->e->assets;
    if (s == NULL || path == NULL || path[0] == '\0')
        return 0;
    const u32 h = de__asset_handle_of(s, de_asset_id_from(path), type);
    if (h == 0)
        de_error_clear();
    else
        note_path(path, strlen(path));
    return h;
}

static i64 read_int(const void *p, u32 size, bool is_signed) {
    switch (size) {
    case 1: return is_signed ? (i64) * (const i8 *)p : (i64) * (const u8 *)p;
    case 2: return is_signed ? (i64) * (const i16 *)p : (i64) * (const u16 *)p;
    case 4: return is_signed ? (i64) * (const i32 *)p : (i64) * (const u32 *)p;
    default: return *(const i64 *)p;
    }
}

static void write_int(void *p, u32 size, i64 v) {
    switch (size) {
    case 1: *(u8 *)p = (u8)v; break;
    case 2: *(u16 *)p = (u16)v; break;
    case 4: *(u32 *)p = (u32)v; break;
    default: *(i64 *)p = v; break;
    }
}

static void write_floats(mpack_writer_t *w, const f32 *v, u32 n) {
    mpack_start_array(w, n);
    for (u32 i = 0; i < n; ++i)
        mpack_write_double(w, (double)v[i]);
    mpack_finish_array(w);
}

void de__ed_write_object(de__editor *ed, mpack_writer_t *w, de_type_id type, const void *obj);
void de__ed_write_field(de__editor *ed, mpack_writer_t *w, de_type_id owner, u32 index,
                        const de_field_info *f, const void *p);

void de__ed_write_field(de__editor *ed, mpack_writer_t *w, de_type_id owner, u32 index,
                        const de_field_info *f, const void *p) {
    const de_type_registry *r = ed->e->types;
    switch (f->kind) {
    case DE_FK_BOOL: mpack_write_bool(w, *(const bool *)p); break;
    case DE_FK_I8: case DE_FK_I16: case DE_FK_I32: case DE_FK_I64:
        mpack_write_i64(w, read_int(p, f->size, true));
        break;
    case DE_FK_U8: case DE_FK_U16: case DE_FK_U32:
        mpack_write_u64(w, (u64)read_int(p, f->size, false));
        break;
    case DE_FK_U64: mpack_write_u64(w, *(const u64 *)p); break;
    case DE_FK_F32: mpack_write_double(w, (double)*(const f32 *)p); break;
    case DE_FK_F64: mpack_write_double(w, *(const f64 *)p); break;
    case DE_FK_VEC2: write_floats(w, (const f32 *)p, 2); break;
    case DE_FK_VEC3: write_floats(w, (const f32 *)p, 3); break;
    case DE_FK_VEC4: case DE_FK_QUAT: case DE_FK_COLOR: write_floats(w, (const f32 *)p, 4); break;
    case DE_FK_MAT4: write_floats(w, (const f32 *)p, 16); break;
    case DE_FK_DVEC3: {
        const f64 *v = (const f64 *)p;
        mpack_start_array(w, 3);
        for (u32 i = 0; i < 3; ++i)
            mpack_write_double(w, v[i]);
        mpack_finish_array(w);
        break;
    }
    case DE_FK_NAME: {
        const char *s = de_name_str(*(const de_name *)p);
        if (s != NULL)
            mpack_write_cstr(w, s);
        else
            mpack_write_u64(w, *(const u64 *)p);
        break;
    }
    case DE_FK_STRING: {
        const char *s = (const char *)p;
        size_t n = 0;
        while (n < f->size && s[n] != '\0')
            ++n;
        mpack_write_str(w, s, (u32)n);
        break;
    }
    case DE_FK_ENTITY: mpack_write_u64(w, ((const de_entity *)p)->bits); break;
    case DE_FK_ASSET: {
        const char *path = de__ed_asset_path(ed, *(const u32 *)p);
        if (path != NULL)
            mpack_write_cstr(w, path);
        else if (*(const u32 *)p == 0)
            mpack_write_cstr(w, "");
        else
            mpack_write_u64(w, (u64)*(const u32 *)p);
        break;
    }
    case DE_FK_ENUM: {
        const i64 v = read_int(p, f->size, true);
        const de_type_id et = f->attrs.nested_type ? de_type_find(r, f->attrs.nested_type)
                                                   : (de_type_id){0};
        const char *name = et.id ? de_enum_name_of(r, et, v) : NULL;
        if (name != NULL)
            mpack_write_cstr(w, name);
        else
            mpack_write_i64(w, v);
        break;
    }
    case DE_FK_FLAGS: {
        const u64 bits = (u64)read_int(p, f->size, false);
        const de_type_id et = f->attrs.nested_type ? de_type_find(r, f->attrs.nested_type)
                                                   : (de_type_id){0};
        const de_enum_info *ei = et.id ? de_enum_get(r, et) : NULL;
        if (ei == NULL) {
            mpack_write_u64(w, bits);
            break;
        }
        mpack_build_array(w);
        for (u32 i = 0; i < ei->count; ++i)
            if (ei->values[i].value != 0 && (bits & (u64)ei->values[i].value) == (u64)ei->values[i].value)
                mpack_write_cstr(w, ei->values[i].name);
        mpack_complete_array(w);
        break;
    }
    case DE_FK_STRUCT: {
        const de_type_id nt = f->attrs.nested_type ? de_type_find(r, f->attrs.nested_type)
                                                   : (de_type_id){0};
        if (nt.id)
            de__ed_write_object(ed, w, nt, p);
        else
            mpack_write_nil(w);
        break;
    }
    case DE_FK_FIXED_ARRAY: {
        const de_field_info *el = owner.id ? de_type_array_element(r, owner, index) : NULL;
        if (el == NULL || el->size == 0) {
            mpack_write_nil(w);
            break;
        }
        const u32 n = f->attrs.array_count;
        mpack_start_array(w, n);
        for (u32 i = 0; i < n; ++i)
            de__ed_write_field(ed, w, (de_type_id){0}, 0, el, (const u8 *)p + (size_t)i * el->size);
        mpack_finish_array(w);
        break;
    }
    default: mpack_write_nil(w); break;
    }
}

void de__ed_write_object(de__editor *ed, mpack_writer_t *w, de_type_id type, const void *obj) {
    u32 n = 0;
    const de_field_info *fields = de_type_fields(ed->e->types, type, &n);
    mpack_build_map(w);
    for (u32 i = 0; i < n && fields != NULL; ++i) {
        mpack_write_cstr(w, fields[i].name);
        de__ed_write_field(ed, w, type, i, &fields[i], (const u8 *)obj + fields[i].offset);
    }
    mpack_complete_map(w);
}

/* ---------- Reading ---------- */

static bool node_number(mpack_node_t n, f64 *out) {
    switch (mpack_node_type(n)) {
    case mpack_type_int: *out = (f64)mpack_node_i64(n); return true;
    case mpack_type_uint: *out = (f64)mpack_node_u64(n); return true;
    case mpack_type_float: *out = (f64)mpack_node_float(n); return true;
    case mpack_type_double: *out = mpack_node_double(n); return true;
    case mpack_type_bool: *out = mpack_node_bool(n) ? 1.0 : 0.0; return true;
    default: return false;
    }
}

static bool read_floats(mpack_node_t n, f32 *v, u32 count) {
    if (mpack_node_type(n) != mpack_type_array || mpack_node_array_length(n) != count)
        return false;
    for (u32 i = 0; i < count; ++i) {
        f64 x;
        if (!node_number(mpack_node_array_at(n, i), &x))
            return false;
        v[i] = (f32)x;
    }
    return true;
}

static bool node_cstr(mpack_node_t n, char *buf, size_t cap) {
    if (mpack_node_type(n) != mpack_type_str)
        return false;
    const size_t len = mpack_node_strlen(n);
    if (len + 1 > cap)
        return false;
    memcpy(buf, mpack_node_str(n), len);
    buf[len] = '\0';
    return true;
}

bool de__ed_read_object(de__editor *ed, mpack_node_t n, de_type_id type, void *obj);
bool de__ed_read_field(de__editor *ed, mpack_node_t n, de_type_id owner, u32 index,
                       const de_field_info *f, void *p);

bool de__ed_read_field(de__editor *ed, mpack_node_t n, de_type_id owner, u32 index,
                       const de_field_info *f, void *p) {
    const de_type_registry *r = ed->e->types;
    f64 x = 0;
    switch (f->kind) {
    case DE_FK_BOOL:
        if (!node_number(n, &x))
            return false;
        *(bool *)p = x != 0.0;
        return true;
    case DE_FK_I8: case DE_FK_I16: case DE_FK_I32: case DE_FK_I64:
    case DE_FK_U8: case DE_FK_U16: case DE_FK_U32:
        if (mpack_node_type(n) == mpack_type_int)
            write_int(p, f->size, mpack_node_i64(n));
        else if (mpack_node_type(n) == mpack_type_uint)
            write_int(p, f->size, (i64)mpack_node_u64(n));
        else if (node_number(n, &x))
            write_int(p, f->size, (i64)x);
        else
            return false;
        return true;
    case DE_FK_U64:
        if (mpack_node_type(n) == mpack_type_uint)
            *(u64 *)p = mpack_node_u64(n);
        else if (node_number(n, &x))
            *(u64 *)p = (u64)x;
        else
            return false;
        return true;
    case DE_FK_F32:
        if (!node_number(n, &x))
            return false;
        *(f32 *)p = (f32)x;
        return true;
    case DE_FK_F64:
        if (!node_number(n, &x))
            return false;
        *(f64 *)p = x;
        return true;
    case DE_FK_VEC2: return read_floats(n, (f32 *)p, 2);
    case DE_FK_VEC3: return read_floats(n, (f32 *)p, 3);
    case DE_FK_VEC4: case DE_FK_QUAT: case DE_FK_COLOR: return read_floats(n, (f32 *)p, 4);
    case DE_FK_MAT4: return read_floats(n, (f32 *)p, 16);
    case DE_FK_DVEC3: {
        if (mpack_node_type(n) != mpack_type_array || mpack_node_array_length(n) != 3)
            return false;
        f64 *v = (f64 *)p;
        for (u32 i = 0; i < 3; ++i)
            if (!node_number(mpack_node_array_at(n, i), &v[i]))
                return false;
        return true;
    }
    case DE_FK_NAME: {
        char buf[256];
        if (node_cstr(n, buf, sizeof buf)) {
            *(de_name *)p = de_name_from(buf);
            return true;
        }
        if (mpack_node_type(n) == mpack_type_uint) {
            *(u64 *)p = mpack_node_u64(n);
            return true;
        }
        return false;
    }
    case DE_FK_STRING: {
        if (mpack_node_type(n) != mpack_type_str)
            return false;
        const size_t len = mpack_node_strlen(n);
        if (f->size == 0 || len >= f->size)
            return false;
        memcpy(p, mpack_node_str(n), len);
        memset((char *)p + len, 0, f->size - len);
        return true;
    }
    case DE_FK_ENTITY:
        if (mpack_node_type(n) == mpack_type_nil) {
            ((de_entity *)p)->bits = 0;
            return true;
        }
        if (!node_number(n, &x) && mpack_node_type(n) != mpack_type_uint)
            return false;
        ((de_entity *)p)->bits =
            mpack_node_type(n) == mpack_type_uint ? mpack_node_u64(n) : (u64)x;
        return true;
    case DE_FK_ASSET: {
        char buf[512];
        if (node_cstr(n, buf, sizeof buf)) {
            const u32 h = buf[0] != '\0' ? de__ed_asset_handle(ed, buf, f->attrs.nested_type) : 0;
            if (buf[0] != '\0' && h == 0)
                return false;
            *(u32 *)p = h;
            return true;
        }
        if (!node_number(n, &x))
            return false;
        *(u32 *)p = (u32)x;
        return true;
    }
    case DE_FK_ENUM: {
        const de_type_id et = f->attrs.nested_type ? de_type_find(r, f->attrs.nested_type)
                                                   : (de_type_id){0};
        char buf[128];
        i64 v = 0;
        if (node_cstr(n, buf, sizeof buf)) {
            if (!et.id || !de_enum_value_of(r, et, buf, &v))
                return false;
        } else if (node_number(n, &x)) {
            v = (i64)x;
        } else {
            return false;
        }
        write_int(p, f->size, v);
        return true;
    }
    case DE_FK_FLAGS: {
        const de_type_id et = f->attrs.nested_type ? de_type_find(r, f->attrs.nested_type)
                                                   : (de_type_id){0};
        if (node_number(n, &x)) {
            write_int(p, f->size, (i64)x);
            return true;
        }
        if (mpack_node_type(n) != mpack_type_array || !et.id)
            return false;
        i64 bits = 0;
        for (size_t i = 0; i < mpack_node_array_length(n); ++i) {
            char buf[128];
            i64 v = 0;
            if (!node_cstr(mpack_node_array_at(n, i), buf, sizeof buf) ||
                !de_enum_value_of(r, et, buf, &v))
                return false;
            bits |= v;
        }
        write_int(p, f->size, bits);
        return true;
    }
    case DE_FK_STRUCT: {
        const de_type_id nt = f->attrs.nested_type ? de_type_find(r, f->attrs.nested_type)
                                                   : (de_type_id){0};
        return nt.id && de__ed_read_object(ed, n, nt, p);
    }
    case DE_FK_FIXED_ARRAY: {
        const de_field_info *el = owner.id ? de_type_array_element(r, owner, index) : NULL;
        if (el == NULL || mpack_node_type(n) != mpack_type_array)
            return false;
        const size_t count = mpack_node_array_length(n);
        if (count > f->attrs.array_count)
            return false;
        for (size_t i = 0; i < count; ++i)
            if (!de__ed_read_field(ed, mpack_node_array_at(n, i), (de_type_id){0}, 0, el,
                                   (u8 *)p + i * el->size))
                return false;
        return true;
    }
    default: return false;
    }
}

bool de__ed_read_object(de__editor *ed, mpack_node_t n, de_type_id type, void *obj) {
    if (mpack_node_type(n) != mpack_type_map)
        return false;
    u32 count = 0;
    const de_field_info *fields = de_type_fields(ed->e->types, type, &count);
    for (size_t i = 0; i < mpack_node_map_count(n); ++i) {
        mpack_node_t k = mpack_node_map_key_at(n, i);
        if (mpack_node_type(k) != mpack_type_str)
            return false;
        const char *key = mpack_node_str(k);
        const size_t klen = mpack_node_strlen(k);
        u32 f = 0;
        while (f < count && !(strlen(fields[f].name) == klen && memcmp(fields[f].name, key, klen) == 0))
            ++f;
        if (f == count)
            return false;
        if (fields[f].flags & DE_FIELD_READONLY)
            continue;
        if (!de__ed_read_field(ed, mpack_node_map_value_at(n, i), type, f, &fields[f],
                               (u8 *)obj + fields[f].offset))
            return false;
    }
    return true;
}
