/* foliage.c: the CPU foliage model (DENG-581). See foliage.h. A port of the DIDE mock engine's
 * foliage (mock-engine/src/foliage.rs): same order of operations and random draws, f32 math
 * through de_dmath. */
#include "world/foliage/foliage.h"

#include "core/de_toml_internal.h"
#include "world/terrain/terrain.h"

#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CELL      DE_FOLIAGE_CELL_SIZE

static de_foliage_error fail(de_foliage_error e, char *msg, size_t cap, const char *fmt, ...) {
    if (msg != NULL && cap > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, cap, fmt, ap);
        va_end(ap);
    }
    return e;
}

/* ---- Types --------------------------------------------------------------------------------- */

static const toml_datum_t *tab_get(const toml_datum_t *t, const char *key) {
    if (t->type != TOML_TABLE)
        return NULL;
    for (int32_t i = 0; i < t->u.tab.size; ++i)
        if (strcmp(t->u.tab.key[i], key) == 0)
            return &t->u.tab.value[i];
    return NULL;
}

static bool num_of(const toml_datum_t *d, f32 *out) {
    if (d == NULL)
        return false;
    if (d->type == TOML_FP64)
        *out = (f32)d->u.fp64;
    else if (d->type == TOML_INT64)
        *out = (f32)d->u.int64;
    else
        return false;
    return true;
}

static bool str_of(const toml_datum_t *d, char *out, size_t cap) {
    if (d == NULL || d->type != TOML_STRING || (size_t)d->u.str.len >= cap)
        return false;
    memcpy(out, d->u.str.ptr, (size_t)d->u.str.len);
    out[d->u.str.len] = '\0';
    return true;
}

static bool finite(f32 v) {
    return v == v && v - v == 0.0f;
}

/* 0: missing, 1: a pair of finite numbers, -1: something else. */
static int pair_of(const toml_datum_t *d, f32 out[2]) {
    if (d == NULL)
        return 0;
    if (d->type != TOML_ARRAY || d->u.arr.size != 2)
        return -1;
    for (int i = 0; i < 2; ++i)
        if (!num_of(&d->u.arr.elem[i], &out[i]) || !finite(out[i]))
            return -1;
    return 1;
}

bool de__foliage_parse_type(const char *id, const char *text, size_t n, de_foliage_type *out,
                            char *msg, size_t cap) {
    memset(out, 0, sizeof *out);
    if (strlen(id) >= sizeof out->id) {
        fail(DE_FOLIAGE_INVALID, msg, cap, "foliage type id is too long");
        return false;
    }
    memcpy(out->id, id, strlen(id) + 1);
    if (n > 0x7fffffffu) {
        fail(DE_FOLIAGE_INVALID, msg, cap, "%s: the file is too large", id);
        return false;
    }
    toml_result_t tr = de__toml_parse(de_heap_allocator(), text, (int)n);
    if (!tr.ok) {
        fail(DE_FOLIAGE_INVALID, msg, cap, "%s: %s", id, tr.errmsg);
        de__toml_free(tr);
        return false;
    }
    const toml_datum_t *t = &tr.toptab;
    const char *missing = NULL, *bad = NULL;
    const toml_datum_t *d;
#define REQUIRE(key) \
    if (missing == NULL && bad == NULL && (d = tab_get(t, key)) == NULL) \
        missing = key;
    REQUIRE("mesh")
    if (missing == NULL && !str_of(d, out->mesh, sizeof out->mesh))
        bad = "mesh";
    if ((d = tab_get(t, "impostor")) != NULL && bad == NULL) {
        out->has_impostor = str_of(d, out->impostor, sizeof out->impostor);
        if (!out->has_impostor)
            bad = "impostor";
    }
    if ((d = tab_get(t, "impostorDistance")) != NULL && bad == NULL) {
        out->has_impostor_distance = num_of(d, &out->impostor_distance);
        if (!out->has_impostor_distance)
            bad = "impostorDistance";
    }
    REQUIRE("density")
    if (missing == NULL && bad == NULL && !num_of(d, &out->density))
        bad = "density";
    static const char *const k_pairs[4] = {"scale", "rotation", "slope", "height"};
    f32 *pairs[4] = {out->scale, out->rotation, out->slope, out->height};
    int pair_state[4] = {0, 0, 0, 0};
    for (u32 i = 0; i < 4 && missing == NULL && bad == NULL; ++i) {
        REQUIRE(k_pairs[i])
        if (missing == NULL)
            pair_state[i] = pair_of(d, pairs[i]);
    }
    REQUIRE("alignToNormal")
    if (missing == NULL && bad == NULL) {
        if (d->type == TOML_BOOLEAN)
            out->align_to_normal = d->u.boolean;
        else
            bad = "alignToNormal";
    }
    REQUIRE("minSpacing")
    if (missing == NULL && bad == NULL && !num_of(d, &out->min_spacing))
        bad = "minSpacing";
    REQUIRE("collision")
    if (missing == NULL && bad == NULL) {
        if (d->type == TOML_BOOLEAN)
            out->collision = d->u.boolean;
        else
            bad = "collision";
    }
    REQUIRE("cullDistance")
    if (missing == NULL && bad == NULL && !num_of(d, &out->cull_distance))
        bad = "cullDistance";
#undef REQUIRE
    de__toml_free(tr);
    if (missing != NULL) {
        fail(DE_FOLIAGE_INVALID, msg, cap, "%s: missing field `%s`", id, missing);
        return false;
    }
    if (bad != NULL) {
        fail(DE_FOLIAGE_INVALID, msg, cap, "%s: invalid type for `%s`", id, bad);
        return false;
    }
    for (u32 i = 0; i < 4; ++i)
        if (pair_state[i] != 1) {
            fail(DE_FOLIAGE_INVALID, msg, cap, "%s: %s needs [min, max]", id, k_pairs[i]);
            return false;
        }
    if (!(finite(out->density) && out->density >= 0.0f && out->min_spacing >= 0.0f)) {
        fail(DE_FOLIAGE_INVALID, msg, cap, "%s: density and minSpacing must be 0 or more", id);
        return false;
    }
    return true;
}

/* ---- Math ---------------------------------------------------------------------------------- */

typedef struct rng {
    u64 s;
} rng;

static rng rng_new(u64 seed) {
    return (rng){seed ^ 0x9e3779b97f4a7c15ull};
}

/* splitmix64, a float in [0, 1). */
static f32 rng_next(rng *r) {
    r->s += 0x9e3779b97f4a7c15ull;
    u64 z = r->s;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return (f32)(z >> 40) / 16777216.0f;
}

static bool within(f32 v, f32 lo, f32 hi) {
    return hi <= lo || (lo <= v && v <= hi);
}

static f32 lerpf(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

static f32 maxf(f32 a, f32 b) {
    return a > b ? a : b;
}

static f32 falloff(const f32 *curve, u32 n, f32 t) {
    if (!(t >= 0.0f && t < 1.0f))
        return 0.0f;
    if (n == 0)
        return 1.0f - t;
    if (n == 1)
        return curve[0];
    const f32 p = t * (f32)(n - 1);
    u32 i = (u32)de_dfloor(p);
    if (i > n - 2)
        i = n - 2;
    const f32 f = p - (f32)i;
    const f32 v = curve[i] * (1.0f - f) + curve[i + 1] * f;
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static u32 f32_bits(f32 v) {
    u32 b;
    memcpy(&b, &v, sizeof b);
    return b;
}

/* Value noise in [0, 1), smooth between integer lattice points. */
static f32 lattice(u32 seed, i32 ix, i32 iz) {
    rng r = rng_new((u64)seed ^ ((u64)(u32)ix << 32) ^ (u64)(u32)iz);
    return rng_next(&r);
}

static f32 noise(f32 x, f32 z, u32 seed) {
    const f32 x0 = de_dfloor(x), z0 = de_dfloor(z);
    const f32 fx = x - x0, fz = z - z0;
    const f32 sx = fx * fx * (3.0f - 2.0f * fx), sz = fz * fz * (3.0f - 2.0f * fz);
    const i32 ix = (i32)x0, iz = (i32)z0;
    const f32 a = lerpf(lattice(seed, ix, iz), lattice(seed, ix + 1, iz), sx);
    const f32 b = lerpf(lattice(seed, ix, iz + 1), lattice(seed, ix + 1, iz + 1), sx);
    return lerpf(a, b, sz);
}

static i32 cell_coord(f32 v) {
    return (i32)de_dfloor(v / CELL);
}

/* ---- Storage ------------------------------------------------------------------------------- */

#define MAX_CELL  DE_FOLIAGE_CELL_INSTANCES
#define MAX_LAYER DE_FOLIAGE_LAYER_INSTANCES

void de__foliage_store_init(de_foliage_store *st, const de_allocator *alloc) {
    memset(st, 0, sizeof *st);
    st->alloc = alloc != NULL ? alloc : de_heap_allocator();
}

void de__foliage_store_free(de_foliage_store *st) {
    for (u32 i = 0; i < st->cell_count; ++i)
        DE_FREE(st->alloc, st->cells[i].inst, sizeof(de_foliage_instance) * st->cells[i].cap);
    DE_FREE(st->alloc, st->cells, sizeof(de_foliage_cell) * st->cell_cap);
    const de_allocator *a = st->alloc;
    de__foliage_store_init(st, a);
}

/* The index of cell (x, z) in the sorted table, or where it would go (`*found` false). */
static u32 cell_find(const de_foliage_store *st, i32 x, i32 z, bool *found) {
    u32 lo = 0, hi = st->cell_count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        const de_foliage_cell *c = &st->cells[mid];
        if (c->z < z || (c->z == z && c->x < x))
            lo = mid + 1u;
        else
            hi = mid;
    }
    *found = lo < st->cell_count && st->cells[lo].x == x && st->cells[lo].z == z;
    return lo;
}

de_foliage_cell *de__foliage_store_cell(const de_foliage_store *st, i32 x, i32 z) {
    bool found;
    const u32 ci = cell_find(st, x, z, &found);
    return found ? &st->cells[ci] : NULL;
}

/* Inserts empty cell (x, z) at `ci`. False when out of memory. */
static bool cell_insert(de_foliage_store *st, u32 ci, i32 x, i32 z) {
    if (st->cell_count == st->cell_cap) {
        const u32 cap = st->cell_cap ? st->cell_cap * 2u : 16u;
        de_foliage_cell *n = (de_foliage_cell *)DE_REALLOC(
            st->alloc, st->cells, sizeof *n * st->cell_cap, sizeof *n * cap,
            DE_ALIGNOF(de_foliage_cell), DE_MEM_GENERAL);
        if (n == NULL)
            return false;
        st->cells = n;
        st->cell_cap = cap;
    }
    memmove(&st->cells[ci + 1], &st->cells[ci], (st->cell_count - ci) * sizeof st->cells[0]);
    st->cells[ci] = (de_foliage_cell){x, z, 0, 0, 0, NULL};
    st->cell_count++;
    return true;
}

static bool cell_reserve(de_foliage_store *st, de_foliage_cell *c, u32 n) {
    if (n <= c->cap)
        return true;
    u32 cap = c->cap ? c->cap : 16u;
    while (cap < n)
        cap *= 2u;
    de_foliage_instance *p = (de_foliage_instance *)DE_REALLOC(
        st->alloc, c->inst, sizeof *p * c->cap, sizeof *p * cap, DE_ALIGNOF(de_foliage_instance),
        DE_MEM_GENERAL);
    if (p == NULL)
        return false;
    c->inst = p;
    c->cap = cap;
    return true;
}

static void cell_drop(de_foliage_store *st, u32 ci) {
    DE_FREE(st->alloc, st->cells[ci].inst, sizeof(de_foliage_instance) * st->cells[ci].cap);
    memmove(&st->cells[ci], &st->cells[ci + 1], (st->cell_count - ci - 1) * sizeof st->cells[0]);
    st->cell_count--;
}

/* The cell (x, z) with room for `more` instances, made when missing; NULL when full or out of
 * memory (a cell made empty is dropped again). */
static de_foliage_cell *cell_for(de_foliage_store *st, i32 x, i32 z, u32 more) {
    if (more > MAX_LAYER - st->instance_count)
        return NULL;
    bool found;
    const u32 ci = cell_find(st, x, z, &found);
    if (!found && !cell_insert(st, ci, x, z))
        return NULL;
    de_foliage_cell *c = &st->cells[ci];
    if (more > MAX_CELL - c->count || !cell_reserve(st, c, c->count + more)) {
        if (c->count == 0)
            cell_drop(st, ci);
        return NULL;
    }
    return c;
}

static void inst_copy(de_foliage_instance *dst, const de_foliage_instance *in) {
    memset(dst, 0, sizeof *dst);
    dst->x = in->x;
    dst->y = in->y;
    dst->z = in->z;
    dst->yaw = in->yaw;
    dst->scale = in->scale;
    dst->rule = in->rule;
    dst->type = in->type;
    dst->nx = in->nx;
    dst->nz = in->nz;
}

bool de__foliage_store_append(de_foliage_store *st, i32 x, i32 z, u64 revision,
                              const de_foliage_instance *inst, u32 n) {
    if (n == 0)
        return true;
    de_foliage_cell *c = cell_for(st, x, z, n);
    if (c == NULL)
        return false;
    for (u32 k = 0; k < n; ++k)
        inst_copy(&c->inst[c->count + k], &inst[k]);
    c->count += n;
    if (revision > c->revision)
        c->revision = revision;
    st->instance_count += n;
    return true;
}

bool de__foliage_store_copy(de_foliage_store *dst, const de_foliage_store *src) {
    de__foliage_store_free(dst);
    for (u32 i = 0; i < src->cell_count; ++i) {
        const de_foliage_cell *c = &src->cells[i];
        if (!de__foliage_store_append(dst, c->x, c->z, c->revision, c->inst, c->count)) {
            de__foliage_store_free(dst);
            return false;
        }
    }
    return true;
}

bool de__foliage_valid(const de_foliage_store *st) {
    if (st->instance_count > MAX_LAYER || st->cell_count > st->cell_cap)
        return false;
    u32 total = 0;
    for (u32 i = 0; i < st->cell_count; ++i) {
        const de_foliage_cell *c = &st->cells[i];
        if (c->count == 0 || c->count > c->cap || c->count > MAX_CELL || c->inst == NULL)
            return false;
        if (i > 0) {
            const de_foliage_cell *p = &st->cells[i - 1];
            if (p->z > c->z || (p->z == c->z && p->x >= c->x))
                return false;
        }
        total += c->count;
        if (total > MAX_LAYER)
            return false;
    }
    return total == st->instance_count;
}

const de_foliage_instance *de__foliage_cell(const de_foliage_store *st, i32 x, i32 z, u32 *n) {
    const de_foliage_cell *c = de__foliage_store_cell(st, x, z);
    *n = c != NULL ? c->count : 0;
    return c != NULL ? c->inst : NULL;
}

static bool inst_equal(const de_foliage_instance *a, const de_foliage_instance *b) {
    return f32_bits(a->x) == f32_bits(b->x) && f32_bits(a->y) == f32_bits(b->y) &&
           f32_bits(a->z) == f32_bits(b->z) && f32_bits(a->yaw) == f32_bits(b->yaw) &&
           f32_bits(a->scale) == f32_bits(b->scale) && a->rule == b->rule && a->type == b->type;
}

bool de__foliage_cell_equal(const de_foliage_store *a, const de_foliage_store *b, i32 x, i32 z) {
    u32 na, nb;
    const de_foliage_instance *ia = de__foliage_cell(a, x, z, &na);
    const de_foliage_instance *ib = de__foliage_cell(b, x, z, &nb);
    if (na != nb)
        return false;
    for (u32 i = 0; i < na; ++i)
        if (!inst_equal(&ia[i], &ib[i]))
            return false;
    return true;
}

/* Removes instance `k` of cell `ci`; drops the cell when it empties. */
static void inst_remove(de_foliage_store *st, u32 ci, u32 k) {
    de_foliage_cell *c = &st->cells[ci];
    memmove(&c->inst[k], &c->inst[k + 1], (c->count - k - 1) * sizeof c->inst[0]);
    c->count--;
    st->instance_count--;
    if (c->count == 0)
        cell_drop(st, ci);
}

/* Appends `in` to its cell; false (nothing placed) when there is no room. */
static bool place(de_foliage_store *st, const de_foliage_instance *in) {
    de_foliage_cell *c = cell_for(st, cell_coord(in->x), cell_coord(in->z), 1);
    if (c == NULL)
        return false;
    inst_copy(&c->inst[c->count++], in);
    st->instance_count++;
    return true;
}

bool de__foliage_set_cell(de_foliage_store *st, i32 x, i32 z, const de_foliage_instance *inst,
                          u32 n) {
    bool found;
    u32 ci = cell_find(st, x, z, &found);
    const u32 old = found ? st->cells[ci].count : 0;
    if (n > MAX_CELL || st->instance_count - old + n > MAX_LAYER)
        return false;
    if (n == 0) {
        if (found) {
            st->instance_count -= old;
            cell_drop(st, ci);
        }
        return true;
    }
    if (!found && !cell_insert(st, ci, x, z))
        return false;
    de_foliage_cell *c = &st->cells[ci];
    if (!cell_reserve(st, c, n)) {
        if (!found)
            cell_drop(st, ci);
        return false;
    }
    /* Kept in cell (x, z) whatever their positions say, as the restore data has them. */
    for (u32 k = 0; k < n; ++k)
        inst_copy(&c->inst[k], &inst[k]);
    c->count = n;
    st->instance_count = st->instance_count - old + n;
    return true;
}

/* Whether an instance of `type` at (x, z) keeps `spacing` from every other one of that type.
 * Only the cells within `reach` of the candidate's cell count (as in the mock); of those, the ones
 * whose span misses [x - spacing, x + spacing] (and the same along z, with a margin for rounding)
 * hold nothing closer, so they are not read. */
static bool room(const de_foliage_store *st, f32 x, f32 z, u32 type, f32 spacing) {
    const i32 cx = cell_coord(x), cz = cell_coord(z);
    const i32 reach = (i32)de_dceil(spacing / CELL);
    const f32 margin = spacing + 0.01f;
    const i32 x0 = cell_coord(x - margin) > cx - reach ? cell_coord(x - margin) : cx - reach;
    const i32 x1 = cell_coord(x + margin) < cx + reach ? cell_coord(x + margin) : cx + reach;
    const i32 z0 = cell_coord(z - margin) > cz - reach ? cell_coord(z - margin) : cz - reach;
    const i32 z1 = cell_coord(z + margin) < cz + reach ? cell_coord(z + margin) : cz + reach;
    const f32 s2 = spacing * spacing;
    for (i32 gz = z0; gz <= z1; ++gz) {
        for (i32 gx = x0; gx <= x1; ++gx) {
            const de_foliage_cell *c = de__foliage_store_cell(st, gx, gz);
            if (c == NULL)
                continue;
            for (u32 k = 0; k < c->count; ++k) {
                const de_foliage_instance *i = &c->inst[k];
                if (i->type != type)
                    continue;
                const f32 dx = i->x - x, dz = i->z - z;
                if (dx * dx + dz * dz < s2)
                    return false;
            }
        }
    }
    return true;
}

void de__foliage_transform(const de_foliage_instance *i, f32 out[10]) {
    const f32 h = i->yaw * (DE_PI / 180.0f) * 0.5f;
    f32 s, c;
    de_dsincos(h, &s, &c);
    out[0] = i->x;
    out[1] = i->y;
    out[2] = i->z;
    /* DENG-822: alignToNormal tilts +y onto the stored normal after the yaw: q = tilt * yaw, with
     * tilt = normalize(up x n, 1 + up . n). Upright instances (nx = nz = 0) keep the yaw alone. */
    f32 tx = 0.0f, tz = 0.0f, tw = 1.0f;
    if (i->nx != 0 || i->nz != 0) {
        const f32 x = (f32)i->nx / 127.0f, z = (f32)i->nz / 127.0f;
        const f32 yy = 1.0f - x * x - z * z;
        const f32 y = yy > 0.0f ? de_dsqrt(yy) : 0.0f;
        const f32 n = de_dsqrt(z * z + x * x + (1.0f + y) * (1.0f + y));
        tx = z / n;
        tz = -x / n;
        tw = (1.0f + y) / n;
    }
    out[3] = tx * c - tz * s;
    out[4] = tw * s;
    out[5] = tx * s + tz * c;
    out[6] = tw * c;
    out[7] = out[8] = out[9] = i->scale;
}

/* ---- Surfaces ------------------------------------------------------------------------------ */

typedef struct hit {
    const de_foliage_surface *src; /**< the surface hit */
    f32 y, slope;
    const f32 *weights;
    u32 weight_count;
    f32 w[DE_TERRAIN_LAYERS];
} hit;

static bool surface_hit(const de_foliage_surface *s, f32 x, f32 z, hit *out) {
    out->src = s;
    if (s->terrain == NULL) {
        out->y = (f32)s->oy;
        out->slope = 0.0f;
        out->weight_count = 0;
        return true;
    }
    de_terrain_sample smp;
    if (!de__terrain_sample_at(s->terrain, s->ox, s->oz, (f64)x, (f64)z, &smp) || smp.hole)
        return false;
    out->y = (f32)s->oy + smp.height;
    out->slope = smp.slope;
    memcpy(out->w, smp.weights, sizeof out->w);
    out->weight_count = DE_TERRAIN_LAYERS;
    return true;
}

static i8 quantise(f32 v) {
    const f32 q = v * 127.0f + (v >= 0.0f ? 0.5f : -0.5f);
    return (i8)(q > 127.0f ? 127 : q < -127.0f ? -127 : (i32)q);
}

/* DENG-822: the normal of `s` at (x, z), its x and z times 127 (de_foliage_instance.nx, nz), from
 * central differences one grid spacing apart (one-sided at the edge of the grid). A plane is flat:
 * 0, 0. */
static void surface_normal(const de_foliage_surface *s, f32 x, f32 z, i8 *nx, i8 *nz) {
    *nx = *nz = 0;
    if (s == NULL || s->terrain == NULL)
        return;
    const f32 e = de__terrain_spacing(s->terrain);
    if (!(e > 0.0f))
        return;
    f32 h[4];
    f32 span[2] = {0.0f, 0.0f};
    de_terrain_sample smp;
    if (!de__terrain_sample_at(s->terrain, s->ox, s->oz, (f64)x, (f64)z, &smp))
        return;
    const f32 h0 = smp.height;
    static const f32 k_dx[4] = {1.0f, -1.0f, 0.0f, 0.0f}, k_dz[4] = {0.0f, 0.0f, 1.0f, -1.0f};
    for (u32 i = 0; i < 4; ++i) {
        const bool ok = de__terrain_sample_at(s->terrain, s->ox, s->oz, (f64)(x + k_dx[i] * e),
                                              (f64)(z + k_dz[i] * e), &smp);
        h[i] = ok ? smp.height : h0;
        span[i / 2] += ok ? e : 0.0f;
    }
    const f32 gx = span[0] > 0.0f ? (h[0] - h[1]) / span[0] : 0.0f;
    const f32 gz = span[1] > 0.0f ? (h[2] - h[3]) / span[1] : 0.0f;
    const f32 len = de_dsqrt(gx * gx + 1.0f + gz * gz);
    *nx = quantise(-gx / len);
    *nz = quantise(-gz / len);
}

static bool highest(const de_foliage_surface *s, u32 n, f32 x, f32 z, hit *out) {
    bool any = false;
    for (u32 i = 0; i < n; ++i) {
        hit h;
        if (surface_hit(&s[i], x, z, &h) && (!any || h.y > out->y)) {
            *out = h;
            any = true;
        }
    }
    return any;
}

/* ---- Ops ----------------------------------------------------------------------------------- */

static de_foliage_error dabs_check(const de_foliage_stroke *s, char *msg, size_t cap) {
    if (s->point_count == 0 || s->point_count % 4 != 0 || !finite(s->radius) ||
        !(s->radius > 0.0f) || !finite(s->strength))
        return fail(DE_FOLIAGE_INVALID, msg, cap,
                    "a foliage stroke needs [x, y, z, pressure] dabs and a radius above 0");
    return DE_FOLIAGE_OK;
}

static de_foliage_error type_of(const de_foliage_env *env, u32 slot, de_foliage_type *out,
                                char *msg, size_t cap) {
    if (env == NULL || env->type_of == NULL)
        return fail(DE_FOLIAGE_INVALID, msg, cap, "palette entry %u is empty", slot);
    return env->type_of(env->user, slot, out, msg, cap);
}

de_foliage_error de__foliage_paint(de_foliage_store *st, const de_foliage_env *env,
                                   const de_foliage_stroke *s, const de_foliage_surface *surfaces,
                                   u32 surface_count, char *msg, size_t cap) {
    if (!s->has_type)
        return fail(DE_FOLIAGE_INVALID, msg, cap, "foliage.paint needs type");
    const u32 index = s->type;
    de_foliage_type ft;
    de_foliage_error e = type_of(env, index, &ft, msg, cap);
    if (e != DE_FOLIAGE_OK)
        return e;
    if (surface_count == 0)
        return fail(DE_FOLIAGE_INVALID, msg, cap, "foliage.paint needs at least one surface");
    if ((e = dabs_check(s, msg, cap)) != DE_FOLIAGE_OK)
        return e;
    const f32 density = s->has_density ? s->density : ft.density;
    const u64 seed = (u64)s->seed;
    const u32 dab_count = s->point_count / 4;
    for (u32 d = 0; d < dab_count; ++d) {
        const f32 cx = s->points[4 * d], cz = s->points[4 * d + 2],
                  pressure = s->points[4 * d + 3];
        const f32 peak = maxf(density * s->strength * pressure, 0.0f);
        const f32 area = DE_PI * s->radius * s->radius;
        const f32 nf = de_dceil(peak * area);
        const u32 n = !(nf > 0.0f) ? 0u
                      : nf >= (f32)DE_FOLIAGE_MAX_CANDIDATES ? DE_FOLIAGE_MAX_CANDIDATES
                                                              : (u32)nf;
        rng r = rng_new(seed ^ (((u64)d + 1u) << 32) ^ (u64)index);
        for (u32 k = 0; k < n; ++k) {
            const f32 rad = s->radius * de_dsqrt(rng_next(&r));
            const f32 a = rng_next(&r) * DE_TAU;
            f32 sa, ca;
            de_dsincos(a, &sa, &ca);
            const f32 x = cx + rad * ca, z = cz + rad * sa;
            const f32 w = s->strength * falloff(s->falloff, s->falloff_count, rad / s->radius) *
                          pressure;
            const f32 keep = rng_next(&r), yaw = rng_next(&r), scale = rng_next(&r);
            if (keep >= w)
                continue;
            hit h;
            if (!highest(surfaces, surface_count, x, z, &h))
                continue;
            if (!within(h.slope, ft.slope[0], ft.slope[1]) ||
                !within(h.y, ft.height[0], ft.height[1]))
                continue;
            const f32 spacing = maxf(ft.min_spacing, 0.5f / de_dsqrt(maxf(density * w, 1e-6f)));
            if (!room(st, x, z, index, spacing))
                continue;
            de_foliage_instance in = {x, h.y, z,
                                      lerpf(ft.rotation[0], ft.rotation[1], yaw),
                                      lerpf(ft.scale[0], ft.scale[1], scale), 0u,
                                      (u8)index, 0, 0};
            if (ft.align_to_normal)
                surface_normal(h.src, x, z, &in.nx, &in.nz);
            (void)place(st, &in); /* no room: skipped */
        }
    }
    return DE_FOLIAGE_OK;
}

de_foliage_error de__foliage_erase(de_foliage_store *st, const de_foliage_stroke *s, char *msg,
                                   size_t cap) {
    const de_foliage_error e = dabs_check(s, msg, cap);
    if (e != DE_FOLIAGE_OK)
        return e;
    const u64 seed = (u64)s->seed;
    const u32 dab_count = s->point_count / 4;
    for (u32 ci = st->cell_count; ci-- > 0;) {
        for (u32 k = st->cells[ci].count; k-- > 0;) {
            const de_foliage_instance *i = &st->cells[ci].inst[k];
            if (s->has_type && s->type != i->type)
                continue;
            f32 w = 0.0f;
            for (u32 d = 0; d < dab_count; ++d) {
                const f32 dx = i->x - s->points[4 * d], dz = i->z - s->points[4 * d + 2];
                const f32 dist = de_dsqrt(dx * dx + dz * dz);
                w = maxf(w, s->strength * falloff(s->falloff, s->falloff_count, dist / s->radius) *
                                s->points[4 * d + 3]);
            }
            rng r = rng_new(seed ^ ((u64)f32_bits(i->x) << 32) ^ (u64)f32_bits(i->z));
            if (rng_next(&r) < w)
                inst_remove(st, ci, k); /* drops the cell at its last instance (k == 0) */
        }
    }
    return DE_FOLIAGE_OK;
}

static void remove_rule(de_foliage_store *st, u32 id) {
    for (u32 ci = st->cell_count; ci-- > 0;)
        for (u32 k = st->cells[ci].count; k-- > 0;)
            if (st->cells[ci].inst[k].rule == id)
                inst_remove(st, ci, k);
}

de_foliage_error de__foliage_scatter(de_foliage_store *st,
                                     const de_foliage_rule rules[DE_FOLIAGE_RULES],
                                     const de_foliage_env *env, const u32 *ids, u32 id_count,
                                     bool clear, char *msg, size_t cap) {
    de_foliage_rule chosen[64];
    u32 n = 0;
    if (ids != NULL) {
        for (u32 k = 0; k < id_count; ++k) {
            const de_foliage_rule *found = NULL;
            for (u32 r = 0; r < DE_FOLIAGE_RULES && found == NULL; ++r)
                if (rules[r].id != 0 && rules[r].id == ids[k])
                    found = &rules[r];
            if (found == NULL)
                return fail(DE_FOLIAGE_INVALID, msg, cap, "no scatter rule %u", ids[k]);
            if (n < sizeof chosen / sizeof chosen[0])
                chosen[n++] = *found;
        }
    } else {
        for (u32 r = 0; r < DE_FOLIAGE_RULES; ++r)
            if (rules[r].id != 0 && rules[r].enabled)
                chosen[n++] = rules[r];
    }
    for (u32 k = 0; k < n; ++k)
        remove_rule(st, chosen[k].id);
    if (clear)
        return DE_FOLIAGE_OK;
    for (u32 k = 0; k < n; ++k) {
        const de_foliage_rule *r = &chosen[k];
        if (!r->enabled)
            continue;
        de_foliage_type ft;
        de_foliage_error e = type_of(env, r->type, &ft, msg, cap);
        if (e != DE_FOLIAGE_OK)
            return e;
        de_foliage_surface surf;
        char sub[192];
        sub[0] = '\0';
        e = env != NULL && env->surface_of != NULL
                ? env->surface_of(env->user, r->terrain, &surf, sub, sizeof sub)
                : DE_FOLIAGE_NOT_FOUND;
        if (e != DE_FOLIAGE_OK)
            return fail(DE_FOLIAGE_NOT_FOUND, msg, cap, "rule %u: terrain %llu", r->id,
                        (unsigned long long)r->terrain.bits);
        if (surf.terrain == NULL)
            return fail(DE_FOLIAGE_INVALID, msg, cap, "rule %u: entity %llu is not a terrain",
                        r->id, (unsigned long long)r->terrain.bits);
        const f32 w = r->area_max.x - r->area_min.x, d = r->area_max.y - r->area_min.y;
        if (!(w > 0.0f) || !(d > 0.0f))
            continue;
        const f32 nf = de_dceil(maxf(r->density, 0.0f) * w * d);
        const u32 count = !(nf > 0.0f) ? 0u
                          : nf >= (f32)DE_FOLIAGE_MAX_CANDIDATES ? DE_FOLIAGE_MAX_CANDIDATES
                                                                  : (u32)nf;
        rng g = rng_new((u64)r->seed ^ ((u64)r->id << 32));
        for (u32 c = 0; c < count; ++c) {
            const f32 x = r->area_min.x + rng_next(&g) * w;
            const f32 z = r->area_min.y + rng_next(&g) * d;
            const f32 yaw = rng_next(&g), scale = rng_next(&g);
            hit h;
            if (!surface_hit(&surf, x, z, &h))
                continue;
            const bool weight_ok =
                r->layer < 0 ||
                ((u32)r->layer < h.weight_count && h.w[r->layer] >= r->min_weight);
            const bool noise_ok = r->noise_scale <= 0.0f ||
                                  noise(x / r->noise_scale, z / r->noise_scale, r->seed) >=
                                      r->noise_threshold;
            if (!weight_ok || !noise_ok || !within(h.slope, r->min_slope, r->max_slope) ||
                !within(h.y, r->min_height, r->max_height) ||
                !within(h.slope, ft.slope[0], ft.slope[1]) ||
                !within(h.y, ft.height[0], ft.height[1]))
                continue;
            if (!room(st, x, z, r->type, ft.min_spacing))
                continue;
            de_foliage_instance in = {x, h.y, z,
                                      lerpf(ft.rotation[0], ft.rotation[1], yaw),
                                      lerpf(ft.scale[0], ft.scale[1], scale), r->id,
                                      r->type, 0, 0};
            if (ft.align_to_normal)
                surface_normal(&surf, x, z, &in.nx, &in.nz);
            (void)place(st, &in); /* no room: skipped */
        }
    }
    return DE_FOLIAGE_OK;
}

void de__foliage_realign(de_foliage_store *st, const de_foliage_env *env,
                         const de_foliage_surface *surfaces, u32 surface_count) {
    /* Per palette slot: 0 not read yet, 1 upright, 2 aligned. */
    u8 align[DE_FOLIAGE_TYPES] = {0};
    char msg[8];
    for (u32 k = 0, ci = 0; ci < st->cell_count; k = 0, ++ci) {
      for (; k < st->cells[ci].count; ++k) {
        de_foliage_instance *in = &st->cells[ci].inst[k];
        if (in->nx != 0 || in->nz != 0 || in->type >= DE_FOLIAGE_TYPES)
            continue;
        if (align[in->type] == 0) {
            de_foliage_type ft;
            align[in->type] =
                type_of(env, in->type, &ft, msg, sizeof msg) == DE_FOLIAGE_OK && ft.align_to_normal
                    ? 2
                    : 1;
        }
        if (align[in->type] != 2)
            continue;
        /* The terrain the instance stands on: the one whose height there is nearest its y. */
        const de_foliage_surface *best = NULL;
        f32 best_d = 0.05f;
        for (u32 s = 0; s < surface_count; ++s) {
            hit h;
            if (surfaces[s].terrain == NULL || !surface_hit(&surfaces[s], in->x, in->z, &h))
                continue;
            const f32 d = h.y > in->y ? h.y - in->y : in->y - h.y;
            if (d <= best_d) {
                best_d = d;
                best = &surfaces[s];
            }
        }
        if (best != NULL)
            surface_normal(best, in->x, in->z, &in->nx, &in->nz);
      }
    }
}
