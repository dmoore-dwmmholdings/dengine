/*
 * nav_settings.c: the project nav settings file `nav.toml` (DENG-833). The editor service
 * (src/host/editor/editor_nav.c) writes it on nav.configure and loads it at start; the game
 * runtime reads the same file with de_nav_settings_load and bakes with de_nav_settings_bake.
 * Built into de_nav_build, next to the cook code that reads TOML.
 */
#include "gameplay/nav/nav_build.h"

#include "core/de_toml_internal.h"

#include <dengine/de_base.h>
#include <dengine/de_gameplay.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define NS_MAX_TILES 4096u
#define NS_MAX_FILE  (256u << 10)
#define NS_PI        3.14159265358979f

/* No libm here (SPEC 5.7, check_determinism_src): finite and whole-number tests by hand. */
static bool finite(f64 v) {
    return v - v == 0.0;
}

#define NS_LIMIT 1.0e9 /* coordinates beyond this are refused, so the i64 casts are safe */

static bool whole(f64 v) {
    return finite(v) && v > -NS_LIMIT && v < NS_LIMIT && v == (f64)(i64)v;
}

/* ceil(v) for 0 <= v < NS_LIMIT. */
static f64 ceil_pos(f64 v) {
    const f64 t = (f64)(i64)v;
    return t < v ? t + 1.0 : t;
}

void de_nav_settings_default(de_nav_settings *s) {
    memset(s, 0, sizeof *s);
    de_nav_agent_settings *a = &s->agents[0];
    snprintf(a->name, sizeof a->name, "humanoid");
    a->radius = 0.4f;
    a->height = 1.8f;
    a->max_slope_deg = 45.0f;
    a->step_height = 0.3f;
    a->cell_size = 0.5f;
    a->cell_height = 0.2f;
    s->agent_count = 1;
    static const struct {
        const char *name;
        f32 cost;
    } k_areas[4] = {{"ground", 1.0f}, {"road", 1.0f}, {"water", 5.0f}, {"jump", 2.0f}};
    for (u32 i = 0; i < 4; ++i) {
        s->areas[i].id = i;
        snprintf(s->areas[i].name, sizeof s->areas[i].name, "%s", k_areas[i].name);
        s->areas[i].cost = k_areas[i].cost;
    }
    s->area_count = 4;
    s->bounds_min[0] = -32, s->bounds_min[1] = -32, s->bounds_min[2] = -2;
    s->bounds_max[0] = 32, s->bounds_max[1] = 32, s->bounds_max[2] = 10;
    s->tile_size = 8.0f;
    s->auto_rebake = true;
}

de_result de_nav_settings_build_desc(const de_nav_settings *s, u32 agent,
                                     de_navmesh_build_desc *out) {
    if (s == NULL || out == NULL || agent >= s->agent_count)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav_settings_build_desc: bad argument");
    const de_nav_agent_settings *a = &s->agents[agent];
    memset(out, 0, sizeof *out);
    out->cell_size = a->cell_size;
    out->cell_height = a->cell_height;
    out->agent_radius = a->radius;
    out->agent_height = a->height;
    out->agent_max_climb = a->step_height;
    /* Recast needs a slope above 0; 0 degrees means flat ground only. */
    const f32 deg = a->max_slope_deg < 0.01f ? 0.01f : a->max_slope_deg;
    out->agent_max_slope_rad = deg * NS_PI / 180.0f;
    out->tile_size = s->tile_size;
    out->collision_mask = a->collision_mask;
    return DE_OK;
}

static de_result bad(const char *fmt, const char *arg) {
    return DE_FAIL(DE_ERR_INVALID_ARGUMENT, fmt, arg != NULL ? arg : "");
}

de_result de_nav_settings_validate(const de_nav_settings *s) {
    if (s == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "nav settings: NULL");
    if (s->agent_count < 1 || s->agent_count > DE_NAV_MAX_AGENTS)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "nav settings: 1 to %u agent types",
                       DE_NAV_MAX_AGENTS);
    if (!(s->tile_size > 0.0f) || !finite((f64)s->tile_size) || !whole(256.0 / (f64)s->tile_size))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "nav settings: tile_size %g does not divide 256 m",
                       (f64)s->tile_size);
    for (u32 i = 0; i < 3; ++i)
        if (!finite(s->bounds_min[i]) || !finite(s->bounds_max[i]) ||
            !(s->bounds_max[i] > s->bounds_min[i]) || s->bounds_min[i] < -NS_LIMIT ||
            s->bounds_max[i] > NS_LIMIT)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "nav settings: bounds need 3 finite values each, max above min");
    if (!whole(s->bounds_min[0] / (f64)s->tile_size) ||
        !whole(s->bounds_min[1] / (f64)s->tile_size))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "nav settings: bounds.min x and y must be multiples of tile_size");
    const f64 tx = ceil_pos((s->bounds_max[0] - s->bounds_min[0]) / (f64)s->tile_size);
    const f64 ty = ceil_pos((s->bounds_max[1] - s->bounds_min[1]) / (f64)s->tile_size);
    if (tx * ty > (f64)NS_MAX_TILES)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "nav settings: more than %u tiles", NS_MAX_TILES);
    for (u32 i = 0; i < s->agent_count; ++i) {
        const de_nav_agent_settings *a = &s->agents[i];
        const size_t n = strnlen(a->name, sizeof a->name);
        if (n < 1 || n > DE_NAV_AGENT_NAME_MAX)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "nav settings: an agent name must have 1 to 32 characters");
        for (u32 j = 0; j < i; ++j)
            if (strcmp(s->agents[j].name, a->name) == 0)
                return bad("nav settings: agent %s is listed twice", a->name);
        de_navmesh_build_desc d;
        de__nav_settings st;
        const char *why = NULL;
        if (de_nav_settings_build_desc(s, i, &d) != DE_OK ||
            de__nav_settings_make(&d, &st, &why) != DE_OK)
            return bad("nav settings: %s", why != NULL ? why : "agent value out of range");
    }
    if (s->area_count < 1 || s->area_count > DE_NAV_MAX_AREAS)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "nav settings: 1 to %u areas", DE_NAV_MAX_AREAS);
    bool has0 = false;
    for (u32 i = 0; i < s->area_count; ++i) {
        const de_nav_area_settings *r = &s->areas[i];
        const size_t n = strnlen(r->name, sizeof r->name);
        has0 = has0 || r->id == 0;
        for (u32 j = 0; j < i; ++j)
            if (s->areas[j].id == r->id)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "nav settings: area %u is listed twice",
                               r->id);
        if (r->id >= DE_NAV_MAX_AREAS || !(r->cost >= 1.0f) || !finite((f64)r->cost) || n < 1 ||
            n > DE_NAV_AREA_NAME_MAX)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "nav settings: area %u is out of range",
                           r->id);
    }
    if (!has0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "nav settings: the areas must hold area 0");
    return DE_OK;
}

/* ---------- Writing ---------- */

typedef struct out_buf {
    char *p;
    size_t cap, len;
    bool over;
} out_buf;

static void put(out_buf *o, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
static void put(out_buf *o, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(o->p + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->cap - o->len)
        o->over = true;
    else
        o->len += (size_t)n;
}

/* A TOML basic string: quotes, backslashes and control characters escaped. */
static void put_str(out_buf *o, const char *key, const char *v) {
    put(o, "%s = \"", key);
    for (; *v != '\0'; ++v) {
        const unsigned char c = (unsigned char)*v;
        if (c == '"' || c == '\\')
            put(o, "\\%c", c);
        else if (c < 0x20 || c == 0x7f)
            put(o, "\\u%04X", (unsigned)c);
        else
            put(o, "%c", c);
    }
    put(o, "\"\n");
}

/* A float that always reads back as a TOML float or integer, and as the same f32 (%.9g). */
static void put_f32(out_buf *o, const char *key, f32 v) {
    put(o, "%s = %.9g\n", key, (f64)v);
}

de_result de_nav_settings_format(const de_nav_settings *s, char *buf, size_t cap, size_t *len) {
    if (buf == NULL || cap == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav_settings_format: no buffer");
    const de_result v = de_nav_settings_validate(s);
    if (v != DE_OK)
        return v;
    out_buf o = {buf, cap, 0, false};
    buf[0] = '\0';
    put(&o, "# DEngine nav settings (written by the editor's nav.configure).\n");
    put(&o, "version = 1\n");
    put_f32(&o, "tile_size", s->tile_size);
    put(&o, "auto_rebake = %s\n", s->auto_rebake ? "true" : "false");
    put(&o, "\n[bounds]\n");
    put(&o, "min = [%.17g, %.17g, %.17g]\n", s->bounds_min[0], s->bounds_min[1], s->bounds_min[2]);
    put(&o, "max = [%.17g, %.17g, %.17g]\n", s->bounds_max[0], s->bounds_max[1], s->bounds_max[2]);
    for (u32 i = 0; i < s->agent_count; ++i) {
        const de_nav_agent_settings *a = &s->agents[i];
        put(&o, "\n[[agent]]\n");
        put_str(&o, "name", a->name);
        put_f32(&o, "radius", a->radius);
        put_f32(&o, "height", a->height);
        put_f32(&o, "max_slope_deg", a->max_slope_deg);
        put_f32(&o, "step_height", a->step_height);
        put_f32(&o, "cell_size", a->cell_size);
        put_f32(&o, "cell_height", a->cell_height);
        put(&o, "collision_mask = %u\n", a->collision_mask);
    }
    for (u32 i = 0; i < s->area_count; ++i) {
        put(&o, "\n[[area]]\n");
        put(&o, "id = %u\n", s->areas[i].id);
        put_str(&o, "name", s->areas[i].name);
        put_f32(&o, "cost", s->areas[i].cost);
    }
    if (o.over) {
        buf[0] = '\0';
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav_settings_format: buffer too small");
    }
    if (len != NULL)
        *len = o.len;
    return DE_OK;
}

/* ---------- Reading ---------- */

static bool num(toml_datum_t d, f64 *out) {
    if (d.type == TOML_FP64) {
        *out = d.u.fp64;
        return finite(*out);
    }
    if (d.type == TOML_INT64) {
        *out = (f64)d.u.int64;
        return true;
    }
    return false;
}

static de_result perr(const char *what, int line) {
    return DE_FAIL(DE_ERR_PARSE, "nav.toml: %s (line %d)", what, line);
}

/* An optional number key: absent keeps *out. */
static de_result opt_f32(toml_datum_t t, const char *key, f32 *out) {
    toml_datum_t d = toml_get(t, key);
    if (d.type == TOML_UNKNOWN)
        return DE_OK;
    f64 v;
    if (!num(d, &v))
        return perr(key, t.lineno);
    *out = (f32)v;
    return DE_OK;
}

static de_result opt_u32(toml_datum_t t, const char *key, u32 *out) {
    toml_datum_t d = toml_get(t, key);
    if (d.type == TOML_UNKNOWN)
        return DE_OK;
    if (d.type != TOML_INT64 || d.u.int64 < 0 || d.u.int64 > 0xFFFFFFFFll)
        return perr(key, t.lineno);
    *out = (u32)d.u.int64;
    return DE_OK;
}

static de_result name_of(toml_datum_t t, char *dst, size_t cap) {
    toml_datum_t d = toml_get(t, "name");
    if (d.type != TOML_STRING || strlen(d.u.s) < 1 || strlen(d.u.s) >= cap)
        return perr("name must be a string of 1 to a few dozen characters", t.lineno);
    memcpy(dst, d.u.s, strlen(d.u.s) + 1);
    return DE_OK;
}

static de_result read_vec3(toml_datum_t t, const char *key, f64 *out) {
    toml_datum_t d = toml_get(t, key);
    if (d.type == TOML_UNKNOWN)
        return DE_OK;
    if (d.type != TOML_ARRAY || d.u.arr.size != 3)
        return perr("bounds need [x, y, z]", t.lineno);
    f64 v[3];
    for (int i = 0; i < 3; ++i)
        if (!num(d.u.arr.elem[i], &v[i]))
            return perr("bounds need 3 finite numbers", t.lineno);
    memcpy(out, v, sizeof v);
    return DE_OK;
}

static de_result read_all(toml_datum_t top, de_nav_settings *s) {
    de_result r = opt_f32(top, "tile_size", &s->tile_size);
    toml_datum_t ar = toml_get(top, "auto_rebake");
    if (ar.type != TOML_UNKNOWN) {
        if (ar.type != TOML_BOOLEAN)
            return perr("auto_rebake must be true or false", top.lineno);
        s->auto_rebake = ar.u.boolean;
    }
    toml_datum_t b = toml_get(top, "bounds");
    if (r == DE_OK && b.type != TOML_UNKNOWN) {
        if (b.type != TOML_TABLE)
            return perr("[bounds] must be a table", b.lineno);
        r = read_vec3(b, "min", s->bounds_min);
        if (r == DE_OK)
            r = read_vec3(b, "max", s->bounds_max);
    }
    toml_datum_t ag = toml_get(top, "agent");
    if (r == DE_OK && ag.type != TOML_UNKNOWN) {
        if (ag.type != TOML_ARRAY || ag.u.arr.size < 1 || ag.u.arr.size > (int)DE_NAV_MAX_AGENTS)
            return perr("[[agent]]: 1 to 4 tables", ag.lineno);
        memset(s->agents, 0, sizeof s->agents);
        s->agent_count = (u32)ag.u.arr.size;
        for (u32 i = 0; i < s->agent_count && r == DE_OK; ++i) {
            toml_datum_t t = ag.u.arr.elem[i];
            de_nav_agent_settings *a = &s->agents[i];
            if (t.type != TOML_TABLE)
                return perr("[[agent]] must be a table", ag.lineno);
            /* An agent starts from the humanoid values, so a short entry is still valid. */
            a->radius = 0.4f, a->height = 1.8f, a->max_slope_deg = 45.0f;
            a->step_height = 0.3f, a->cell_size = 0.5f, a->cell_height = 0.2f;
            r = name_of(t, a->name, sizeof a->name);
            if (r == DE_OK)
                r = opt_f32(t, "radius", &a->radius);
            if (r == DE_OK)
                r = opt_f32(t, "height", &a->height);
            if (r == DE_OK)
                r = opt_f32(t, "max_slope_deg", &a->max_slope_deg);
            if (r == DE_OK)
                r = opt_f32(t, "step_height", &a->step_height);
            if (r == DE_OK)
                r = opt_f32(t, "cell_size", &a->cell_size);
            if (r == DE_OK)
                r = opt_f32(t, "cell_height", &a->cell_height);
            if (r == DE_OK)
                r = opt_u32(t, "collision_mask", &a->collision_mask);
        }
    }
    toml_datum_t ae = toml_get(top, "area");
    if (r == DE_OK && ae.type != TOML_UNKNOWN) {
        if (ae.type != TOML_ARRAY || ae.u.arr.size < 1 || ae.u.arr.size > (int)DE_NAV_MAX_AREAS)
            return perr("[[area]]: 1 to 16 tables", ae.lineno);
        memset(s->areas, 0, sizeof s->areas);
        s->area_count = (u32)ae.u.arr.size;
        for (u32 i = 0; i < s->area_count && r == DE_OK; ++i) {
            toml_datum_t t = ae.u.arr.elem[i];
            de_nav_area_settings *a = &s->areas[i];
            if (t.type != TOML_TABLE)
                return perr("[[area]] must be a table", ae.lineno);
            a->cost = 1.0f;
            r = opt_u32(t, "id", &a->id);
            if (r == DE_OK)
                r = name_of(t, a->name, sizeof a->name);
            if (r == DE_OK)
                r = opt_f32(t, "cost", &a->cost);
        }
    }
    return r;
}

de_result de_nav_settings_parse(const char *text, u64 len, de_nav_settings *out) {
    if (text == NULL || out == NULL || len > NS_MAX_FILE)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav_settings_parse: bad arguments");
    de_nav_settings s;
    de_nav_settings_default(&s);
    toml_result_t res = de__toml_parse(de_heap_allocator(), text, (int)len);
    if (!res.ok) {
        const de_result r = DE_FAIL(DE_ERR_PARSE, "nav.toml: %s", res.errmsg);
        de__toml_free(res);
        return r;
    }
    de_result r = read_all(res.toptab, &s);
    de__toml_free(res);
    if (r == DE_OK && de_nav_settings_validate(&s) != DE_OK)
        r = DE_ERR_PARSE; /* the validate message stays in the error context */
    if (r == DE_OK)
        *out = s;
    return r;
}

de_result de_nav_settings_load(const char *vpath, de_nav_settings *out) {
    if (vpath == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav_settings_load: bad arguments");
    de_nav_settings_default(out);
    void *data = NULL;
    u64 size = 0;
    const de_result rd = de_file_read_all(vpath, de_heap_allocator(), &data, &size);
    if (rd != DE_OK)
        return rd;
    const de_result r = de_nav_settings_parse((const char *)data, size, out);
    DE_FREE(de_heap_allocator(), data, (size_t)size + 1);
    return r;
}

de_result de_nav_settings_save(const char *vpath, const de_nav_settings *s) {
    if (vpath == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav_settings_save: NULL path");
    char *buf = (char *)DE_ALLOC(de_heap_allocator(), NS_MAX_FILE, 1, DE_MEM_ASSET);
    if (buf == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav_settings_save");
    size_t len = 0;
    de_result r = de_nav_settings_format(s, buf, NS_MAX_FILE, &len);
    if (r == DE_OK)
        r = de_file_write_all_atomic(vpath, buf, len);
    DE_FREE(de_heap_allocator(), buf, NS_MAX_FILE);
    return r;
}

de_result de_nav_settings_bake(de_nav *n, const de_nav_settings *s, u32 agent) {
    de_navmesh_build_desc d;
    if (n == NULL || s == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav_settings_bake: NULL argument");
    const de_result r = de_nav_settings_build_desc(s, agent, &d);
    if (r != DE_OK)
        return r;
    de_aabb box;
    box.min = (de_vec3){(f32)s->bounds_min[0], (f32)s->bounds_min[1], (f32)s->bounds_min[2]};
    box.max = (de_vec3){(f32)s->bounds_max[0], (f32)s->bounds_max[1], (f32)s->bounds_max[2]};
    return de_nav_rebuild_tiles(n, &d, box);
}
