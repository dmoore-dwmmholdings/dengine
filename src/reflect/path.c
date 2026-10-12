/*
 * path.c: de_reflect_resolve_path (DENG-294).
 *
 * Grammar: path = segment { "." segment }; segment = identifier [ "[" digits "]" ].
 * A segment resolves against the current struct's fields, or, when the current field is a
 * vector kind, against its components (x y z w; r g b a for COLOR). `[n]` applies only to
 * FIXED_ARRAY fields and moves to element n. Allocates nothing.
 */
#include "reflect_internal.h"

#include <string.h>

#define SEG_MAX 128u /* longest segment quoted in an error message */

typedef struct cursor {
    const de_field_info *field;    /* the current field, or NULL at the root */
    const de__field_extra *extra;  /* its derived data (NULL for components) */
    bool in_element;               /* `field` is an array element (after `[n]`) */
    const de__type_layout *layout; /* struct whose fields the next segment names */
    u32 offset;                    /* byte offset of `field` from the object start */
} cursor;

static void quote(char *dst, const char *s, size_t n) {
    size_t m = n < SEG_MAX - 1 ? n : SEG_MAX - 1;
    memcpy(dst, s, m);
    dst[m] = '\0';
}

static bool is_ident_char(char c, bool first) {
    bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    return alpha || (!first && c >= '0' && c <= '9');
}

/* true if the current field is a vector (a field or an array element) with components. */
static bool has_components(const cursor *c) {
    u32 comp_size;
    return c->extra && c->extra->comp_count > 0 &&
           de__reflect_kind_components(c->field->kind, &comp_size) > 0;
}

/* Finds component `name` (len n) among `x`. */
static const de_field_info *find_comp(const de__field_extra *x, de_field_kind kind,
                                      const char *name, size_t n) {
    if (n != 1)
        return NULL;
    static const char xyzw[4] = {'x', 'y', 'z', 'w'};
    for (u32 i = 0; i < x->comp_count; ++i) {
        /* COLOR components are named r g b a, and x y z w are accepted as aliases. */
        if (x->comps[i].name[0] == name[0] || (kind == DE_FK_COLOR && xyzw[i] == name[0]))
            return &x->comps[i];
    }
    return NULL;
}

de_result de_reflect_resolve_path(const de_type_registry *r, de_type_id type, const char *path,
                                  const de_field_info **field, u32 *offset) {
    if (!r || !path || !field || !offset)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_reflect_resolve_path: NULL argument");
    const de__type_entry *e = de__reflect_entry(r, type);
    if (!e || e->kind != DE__ENTRY_STRUCT)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_reflect_resolve_path: invalid struct type id (path '%s')", path);
    const char *tn = e->name;

    cursor c;
    memset(&c, 0, sizeof c);
    c.layout = e->cur;

    const char *p = path;
    char seg[SEG_MAX];
    for (u32 depth = 0;; ++depth) {
        const char *start = p;
        while (is_ident_char(*p, p == start))
            ++p;
        size_t n = (size_t)(p - start);
        if (n == 0 || depth >= DE__TYPE_MAX_DEPTH) {
            size_t rest = strcspn(start, ".");
            quote(seg, start, rest);
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "type '%s' path '%s': malformed segment '%s'",
                           tn, path, seg);
        }

        /* Name: a field of the current struct, or a component of the current vector. */
        if (c.layout) {
            const de__type_layout *l = c.layout;
            u32 i = 0;
            for (; i < l->desc.field_count; ++i) {
                const char *fname = l->fields[i].name;
                if (strlen(fname) == n && memcmp(fname, start, n) == 0)
                    break;
            }
            if (i == l->desc.field_count) {
                quote(seg, start, n);
                return DE_FAIL(DE_ERR_NOT_FOUND, "type '%s' path '%s': no field '%s' in '%s'", tn,
                               path, seg, l->desc.name);
            }
            c.offset += l->fields[i].offset;
            c.field = &l->fields[i];
            c.extra = &l->extra[i];
            c.in_element = false;
        } else {
            const de_field_info *comp = NULL;
            if (has_components(&c))
                comp = find_comp(c.extra, c.field->kind, start, n);
            if (!comp) {
                quote(seg, start, n);
                return DE_FAIL(DE_ERR_NOT_FOUND,
                               "type '%s' path '%s': field '%s' has no member '%s'", tn, path,
                               c.field->name, seg);
            }
            c.offset += comp->offset;
            c.field = comp;
            c.extra = NULL;
            c.in_element = false;
        }

        /* Optional index. */
        if (*p == '[') {
            ++p;
            u64 v = 0;
            const char *digits = p;
            while (*p >= '0' && *p <= '9') {
                v = v * 10u + (u64)(*p - '0');
                if (v > 0xffffffffu)
                    v = 0x100000000u; /* saturate: any such index is out of range */
                ++p;
            }
            if (p == digits || *p != ']') {
                size_t rest = strcspn(start, ".");
                quote(seg, start, rest);
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                               "type '%s' path '%s': malformed index in segment '%s'", tn, path,
                               seg);
            }
            ++p;
            if (c.field->kind != DE_FK_FIXED_ARRAY || c.in_element || !c.extra) {
                quote(seg, start, (size_t)(p - start));
                return DE_FAIL(DE_ERR_NOT_FOUND,
                               "type '%s' path '%s': segment '%s' indexes field '%s', which is "
                               "not a FIXED_ARRAY",
                               tn, path, seg, c.field->name);
            }
            if (v >= c.field->attrs.array_count) {
                quote(seg, start, (size_t)(p - start));
                return DE_FAIL(DE_ERR_NOT_FOUND,
                               "type '%s' path '%s': segment '%s': index out of range (%u "
                               "elements)",
                               tn, path, seg, c.field->attrs.array_count);
            }
            c.offset += (u32)v * c.extra->element.size;
            c.field = &c.extra->element;
            c.in_element = true;
        }

        /* What the next segment names. */
        c.layout = NULL;
        if (c.field->kind == DE_FK_STRUCT)
            c.layout = de__reflect_find_layout(r, c.field->attrs.nested_type);

        if (*p == '\0')
            break;
        if (*p != '.') {
            size_t rest = strcspn(start, ".");
            quote(seg, start, rest);
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "type '%s' path '%s': malformed segment '%s'",
                           tn, path, seg);
        }
        ++p;
        if (!c.layout && !has_components(&c)) {
            size_t rest = strcspn(p, ".");
            quote(seg, p, rest);
            return DE_FAIL(DE_ERR_NOT_FOUND, "type '%s' path '%s': field '%s' has no member '%s'",
                           tn, path, c.field->name, seg);
        }
    }
    *field = c.field;
    *offset = c.offset;
    return DE_OK;
}
