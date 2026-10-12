/*
 * validate.c: kind tables and the registration checks of the type registry (DENG-294).
 *
 * Every failure is DE_ERR_INVALID_ARGUMENT (DE_ERR_OUT_OF_MEMORY never happens here) and the
 * message names the type and, for a field rule, the field: "type 'T' field 'f': ...".
 */
#include "reflect_internal.h"

#include <string.h>

/* ---- Kind tables --------------------------------------------------------------------------- */

typedef struct kind_row {
    const char *name; /* built-in element name for FIXED_ARRAY */
    u32 fixed_size;   /* 0: depends on the field */
    u32 comps;        /* vector components */
    u32 comp_size;
} kind_row;

/* Indexed by de_field_kind. */
static const kind_row k_kinds[DE_FIELD_KIND_COUNT] = {
    [DE_FK_BOOL] = {"bool", 1, 0, 0},     [DE_FK_I8] = {"i8", 1, 0, 0},
    [DE_FK_I16] = {"i16", 2, 0, 0},       [DE_FK_I32] = {"i32", 4, 0, 0},
    [DE_FK_I64] = {"i64", 8, 0, 0},       [DE_FK_U8] = {"u8", 1, 0, 0},
    [DE_FK_U16] = {"u16", 2, 0, 0},       [DE_FK_U32] = {"u32", 4, 0, 0},
    [DE_FK_U64] = {"u64", 8, 0, 0},       [DE_FK_F32] = {"f32", 4, 0, 0},
    [DE_FK_F64] = {"f64", 8, 0, 0},       [DE_FK_VEC2] = {"vec2", 8, 2, 4},
    [DE_FK_VEC3] = {"vec3", 12, 3, 4},    [DE_FK_VEC4] = {"vec4", 16, 4, 4},
    [DE_FK_DVEC3] = {"dvec3", 24, 3, 8},  [DE_FK_QUAT] = {"quat", 16, 4, 4},
    [DE_FK_MAT4] = {"mat4", 64, 0, 0},    [DE_FK_COLOR] = {"color", 16, 4, 4},
    [DE_FK_NAME] = {"name", 8, 0, 0},     [DE_FK_STRING] = {"string", 0, 0, 0},
    [DE_FK_ENTITY] = {"entity", 8, 0, 0}, [DE_FK_ASSET] = {"asset", 4, 0, 0},
    [DE_FK_ENUM] = {NULL, 0, 0, 0},       [DE_FK_FLAGS] = {NULL, 0, 0, 0},
    [DE_FK_STRUCT] = {NULL, 0, 0, 0},     [DE_FK_FIXED_ARRAY] = {NULL, 0, 0, 0},
};

static bool kind_valid(de_field_kind kind) {
    return (u32)kind < DE_FIELD_KIND_COUNT;
}

u32 de__reflect_kind_fixed_size(de_field_kind kind) {
    return kind_valid(kind) ? k_kinds[kind].fixed_size : 0;
}

u32 de__reflect_kind_components(de_field_kind kind, u32 *comp_size) {
    if (!kind_valid(kind)) {
        *comp_size = 0;
        return 0;
    }
    *comp_size = k_kinds[kind].comp_size;
    return k_kinds[kind].comps;
}

bool de__reflect_builtin_kind(const char *name, de_field_kind *out_kind) {
    if (!name)
        return false;
    for (u32 k = 0; k < DE_FIELD_KIND_COUNT; ++k) {
        if (k_kinds[k].name && strcmp(k_kinds[k].name, name) == 0) {
            *out_kind = (de_field_kind)k;
            return true;
        }
    }
    return false;
}

bool de__reflect_is_identifier(const char *s) {
    if (!s || !*s)
        return false;
    for (const char *p = s; *p; ++p) {
        char c = *p;
        bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        bool digit = c >= '0' && c <= '9';
        if (!(alpha || (digit && p != s)))
            return false;
    }
    return true;
}

static bool is_int_size(u32 size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

/* true if every value of `e` fits an integer of `size` bytes (signed or unsigned reading). */
static bool enum_fits(const de_enum_info *e, u32 size) {
    if (size >= 8)
        return true;
    i64 lo = -((i64)1 << (size * 8 - 1));
    i64 hi = ((i64)1 << (size * 8)) - 1;
    for (u32 i = 0; i < e->count; ++i) {
        if (e->values[i].value < lo || e->values[i].value > hi)
            return false;
    }
    return true;
}

/* The registered enum `name` (any flags-ness), or NULL. */
static const de_enum_info *find_enum(const de_type_registry *r, const char *name) {
    const de__type_entry *e = name ? de__reflect_find_entry(r, name) : NULL;
    return (e && e->kind == DE__ENTRY_ENUM) ? e->enum_info : NULL;
}

/* ENUM / FLAGS of `size` bytes naming `nested`. */
static de_result check_enum_ref(const de_type_registry *r, const char *tn, const char *fn,
                                de_field_kind kind, const char *nested, u32 size) {
    const char *what = kind == DE_FK_FLAGS ? "FLAGS" : "ENUM";
    if (!is_int_size(size))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "type '%s' field '%s': %s size %u is not 1, 2, 4 or 8", tn, fn, what, size);
    if (!nested || !*nested)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "type '%s' field '%s': %s needs attrs.nested_type (an enum name)", tn, fn,
                       what);
    const de_enum_info *e = find_enum(r, nested);
    if (!e)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "type '%s' field '%s': nested_type '%s' is not a registered enum", tn, fn,
                       nested);
    if (e->is_flags != (kind == DE_FK_FLAGS))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "type '%s' field '%s': %s field names %s enum '%s'",
                       tn, fn, what, e->is_flags ? "flags" : "a non-flags", nested);
    if (!enum_fits(e, size))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "type '%s' field '%s': values of enum '%s' do not fit %u bytes", tn, fn,
                       nested, size);
    return DE_OK;
}

/* true if struct type `name` (current layouts in `r`) reaches `target` through STRUCT or
 * FIXED_ARRAY nesting, or is `target`. */
static bool nests(const de_type_registry *r, const char *name, const char *target, u32 depth) {
    if (strcmp(name, target) == 0)
        return true;
    if (depth >= DE__TYPE_MAX_DEPTH)
        return true; /* treat absurd depth as a cycle */
    const de__type_layout *l = de__reflect_find_layout(r, name);
    if (!l)
        return false;
    for (u32 i = 0; i < l->desc.field_count; ++i) {
        const de_field_info *f = &l->fields[i];
        const char *n = f->attrs.nested_type;
        if ((f->kind == DE_FK_STRUCT || f->kind == DE_FK_FIXED_ARRAY) && n &&
            nests(r, n, target, depth + 1))
            return true;
    }
    return false;
}

/* STRUCT of `size` bytes naming `nested`. */
static de_result check_struct_ref(const de_type_registry *r, const char *tn, const char *fn,
                                  const char *nested, u32 size, const char *what) {
    if (!nested || !*nested)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "type '%s' field '%s': %s needs attrs.nested_type (a type name)", tn, fn,
                       what);
    const de__type_layout *l = de__reflect_find_layout(r, nested);
    if (!l)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "type '%s' field '%s': nested_type '%s' is not a registered struct type", tn,
                       fn, nested);
    if (l->desc.size != size)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "type '%s' field '%s': %s size %u differs from size %u of type '%s'", tn, fn,
                       what, size, l->desc.size, nested);
    if (nests(r, nested, tn, 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "type '%s' field '%s': nested type '%s' contains '%s' (a cycle)", tn, fn,
                       nested, tn);
    return DE_OK;
}

de_result de__reflect_array_element(const de_type_registry *r, const char *tn,
                                    const de_field_info *f, de_field_kind *out_kind, u32 *out_size,
                                    const char **out_elem_nested) {
    const char *fn = f->name;
    u32 count = f->attrs.array_count;
    if (count == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "type '%s' field '%s': FIXED_ARRAY needs attrs.array_count >= 1", tn, fn);
    if (f->size % count != 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "type '%s' field '%s': FIXED_ARRAY size %u is not a multiple of "
                       "array_count %u",
                       tn, fn, f->size, count);
    u32 elem = f->size / count;
    const char *nested = f->attrs.nested_type;
    de_field_kind kind;
    *out_elem_nested = NULL;

    if (!nested) {
        if (!is_int_size(elem))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "type '%s' field '%s': FIXED_ARRAY element size %u needs a "
                           "nested_type (only 1, 2, 4 and 8 bytes default to unsigned ints)",
                           tn, fn, elem);
        kind = elem == 1 ? DE_FK_U8 : elem == 2 ? DE_FK_U16 : elem == 4 ? DE_FK_U32 : DE_FK_U64;
    } else if (de__reflect_builtin_kind(nested, &kind)) {
        u32 fixed = de__reflect_kind_fixed_size(kind);
        if (fixed != 0 && fixed != elem)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "type '%s' field '%s': FIXED_ARRAY element size %u does not fit "
                           "kind '%s' (%u bytes)",
                           tn, fn, elem, nested, fixed);
        if (kind == DE_FK_STRING && elem == 0)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "type '%s' field '%s': FIXED_ARRAY string elements are empty", tn, fn);
    } else {
        const de__type_entry *e = de__reflect_find_entry(r, nested);
        if (!e)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "type '%s' field '%s': FIXED_ARRAY nested_type '%s' is not a "
                           "registered type, enum or built-in kind",
                           tn, fn, nested);
        if (e->kind == DE__ENTRY_ENUM) {
            kind = e->enum_info->is_flags ? DE_FK_FLAGS : DE_FK_ENUM;
            DE_TRY(check_enum_ref(r, tn, fn, kind, nested, elem));
        } else {
            kind = DE_FK_STRUCT;
            DE_TRY(check_struct_ref(r, tn, fn, nested, elem, "FIXED_ARRAY element"));
        }
        *out_elem_nested = e->name;
    }
    *out_kind = kind;
    *out_size = elem;
    return DE_OK;
}

static const char *kind_label(de_field_kind kind) {
    static const char *const names[DE_FIELD_KIND_COUNT] = {
        "BOOL", "I8",     "I16",    "I32",   "I64",  "U8",    "U16",    "U32",        "U64",
        "F32",  "F64",    "VEC2",   "VEC3",  "VEC4", "DVEC3", "QUAT",   "MAT4",       "COLOR",
        "NAME", "STRING", "ENTITY", "ASSET", "ENUM", "FLAGS", "STRUCT", "FIXED_ARRAY"};
    return kind_valid(kind) ? names[kind] : "?";
}

/* The kind/size rule of one field. */
static de_result check_field_kind(const de_type_registry *r, const char *tn,
                                  const de_field_info *f) {
    const char *fn = f->name;
    if (!kind_valid(f->kind))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "type '%s' field '%s': unknown kind %d", tn, fn,
                       (int)f->kind);
    u32 fixed = k_kinds[f->kind].fixed_size;
    if (fixed != 0) {
        if (f->size != fixed)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "type '%s' field '%s': kind %s needs size %u, not %u", tn, fn,
                           kind_label(f->kind), fixed, f->size);
        return DE_OK;
    }
    switch (f->kind) {
    case DE_FK_STRING:
        if (f->size < 1)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "type '%s' field '%s': STRING needs a char[N] buffer, N >= 1", tn, fn);
        return DE_OK;
    case DE_FK_ENUM:
    case DE_FK_FLAGS:
        return check_enum_ref(r, tn, fn, f->kind, f->attrs.nested_type, f->size);
    case DE_FK_STRUCT:
        return check_struct_ref(r, tn, fn, f->attrs.nested_type, f->size, "STRUCT");
    case DE_FK_FIXED_ARRAY: {
        de_field_kind ek;
        u32 es;
        const char *en;
        return de__reflect_array_element(r, tn, f, &ek, &es, &en);
    }
    default:
        return DE_FAIL(DE_ERR_INTERNAL, "type '%s' field '%s': kind table incomplete", tn, fn);
    }
}

de_result de__reflect_validate_type(const de_type_registry *r, const de_type_desc *desc) {
    if (!desc)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_type_register: desc is NULL");
    const char *tn = desc->name;
    if (!de__reflect_is_identifier(tn))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "type '%s': the name is not an identifier",
                       tn ? tn : "(null)");
    de_field_kind bk;
    if (de__reflect_builtin_kind(tn, &bk))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "type '%s': the name is a built-in kind name", tn);
    if (desc->size == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "type '%s': size is 0", tn);
    if (desc->align != 0 &&
        ((desc->align & (desc->align - 1)) != 0 || desc->size % desc->align != 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "type '%s': align %u is not a power of two dividing size %u", tn,
                       desc->align, desc->size);
    if (desc->field_count > 0 && !desc->fields)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "type '%s': %u fields but fields is NULL", tn,
                       desc->field_count);

    for (u32 i = 0; i < desc->field_count; ++i) {
        const de_field_info *f = &desc->fields[i];
        if (!de__reflect_is_identifier(f->name))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "type '%s' field '%s' (#%u): the name is not an identifier", tn,
                           f->name ? f->name : "(null)", i);
        for (u32 j = 0; j < i; ++j) {
            if (strcmp(desc->fields[j].name, f->name) == 0)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                               "type '%s' field '%s': duplicate field name (#%u and #%u)", tn,
                               f->name, j, i);
        }
        if ((u64)f->offset + f->size > desc->size)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "type '%s' field '%s': offset %u + size %u exceeds type size %u", tn,
                           f->name, f->offset, f->size, desc->size);
        DE_TRY(check_field_kind(r, tn, f));
    }

    /* Overlap: O(n^2) on registration only; types have tens of fields. Sizes are >= 1 here. */
    for (u32 i = 0; i < desc->field_count; ++i) {
        const de_field_info *a = &desc->fields[i];
        for (u32 j = i + 1; j < desc->field_count; ++j) {
            const de_field_info *b = &desc->fields[j];
            if (a->offset < b->offset + b->size && b->offset < a->offset + a->size)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                               "type '%s' field '%s': bytes [%u, %u) overlap field '%s' "
                               "[%u, %u)",
                               tn, b->name, b->offset, b->offset + b->size, a->name, a->offset,
                               a->offset + a->size);
        }
    }
    return DE_OK;
}

de_result de__reflect_validate_enum(const char *name, const de_enum_value *values, u32 count) {
    if (!de__reflect_is_identifier(name))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "enum '%s': the name is not an identifier",
                       name ? name : "(null)");
    de_field_kind bk;
    if (de__reflect_builtin_kind(name, &bk))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "enum '%s': the name is a built-in kind name",
                       name);
    if (count == 0 || !values)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "enum '%s': needs at least one value", name);
    for (u32 i = 0; i < count; ++i) {
        if (!de__reflect_is_identifier(values[i].name))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "enum '%s' value '%s' (#%u): the name is not an identifier", name,
                           values[i].name ? values[i].name : "(null)", i);
        for (u32 j = 0; j < i; ++j) {
            if (strcmp(values[j].name, values[i].name) == 0)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "enum '%s' value '%s': duplicate name",
                               name, values[i].name);
            if (values[j].value == values[i].value)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                               "enum '%s' value '%s': value %lld already used by '%s'", name,
                               values[i].name, (long long)values[i].value, values[j].name);
        }
    }
    return DE_OK;
}
