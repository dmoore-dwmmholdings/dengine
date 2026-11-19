/* water.c: the buoyancy component and the CPU water model (DENG-776). See water.h. */
#include "world/water/water.h"

#include <dengine/de_reflect.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/* ---- Registration ------------------------------------------------------------------------- */

#define AUTHORED DE_FIELD_DEFAULT
#define HIDDEN   (DE_FIELD_DEFAULT | DE_FIELD_EDITOR_HIDDEN)

static void construct_buoyancy(void *obj) {
    DeBuoyancy *b = (DeBuoyancy *)obj;
    memset(b, 0, sizeof *b);
    /* As the mock engine: the four bottom corners of a 1 m box. */
    b->count = 4;
    b->probes[0] = (de_vec3){-0.5f, -0.5f, -0.5f};
    b->probes[1] = (de_vec3){0.5f, -0.5f, -0.5f};
    b->probes[2] = (de_vec3){-0.5f, -0.5f, 0.5f};
    b->probes[3] = (de_vec3){0.5f, -0.5f, 0.5f};
    b->radius = 0.25f;
}

de_result de__water_register(de_world *w, de_component *buoyancy) {
    if (w == NULL || buoyancy == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__water_register: NULL argument");
    *buoyancy = de_component_find(w, DE_BUOYANCY_COMPONENT);
    if (DE_HANDLE_VALID(*buoyancy))
        return DE_OK;
    static const de_field_info k_fields[] = {
        DE_FIELD_EX(DeBuoyancy, count, DE_FK_U32, HIDDEN,
                    {0, DE_WATER_MAX_PROBES, 0, "Probes in use", 0, 0}),
        DE_FIELD_EX(DeBuoyancy, probes, DE_FK_FIXED_ARRAY, HIDDEN,
                    {0, 0, "m", "Probe centres in the body's local space", "vec3",
                     DE_WATER_MAX_PROBES}),
        DE_FIELD_EX(DeBuoyancy, radius, DE_FK_F32, AUTHORED,
                    {0.01, 10, "m", "Each probe is a sphere of this radius", 0, 0}),
    };
    de_type_desc d = DE_TYPE_DESC(DeBuoyancy, 1, k_fields);
    d.hooks.construct = construct_buoyancy;
    de_type_registry *reg = de_world_types(w);
    de_type_id t = de_type_find(reg, d.name);
    if (DE_HANDLE_VALID(t)) {
        const de_type_desc *cur = de_type_get(reg, t);
        if (cur == NULL || cur->size != d.size || cur->align != d.align ||
            cur->version != d.version || cur->field_count != d.field_count)
            return DE_FAIL(DE_ERR_ALREADY_EXISTS,
                           "de__water_register: the type registry has another type called %s",
                           d.name);
    } else {
        const de_result r = de_type_register_ex(reg, &d, &t, NULL);
        if (r != DE_OK)
            return DE_FAIL_WRAP(r, "de__water_register: registering %s", d.name);
    }
    *buoyancy = de_component_register(w, t, DE_COMP_SNAPSHOT | DE_COMP_SERIALIZE);
    if (!DE_HANDLE_VALID(*buoyancy))
        return DE_FAIL(DE_ERR_INTERNAL, "de__water_register: component %s", d.name);
    return DE_OK;
}

/* ---- Model -------------------------------------------------------------------------------- */

static f32 clampf(f32 v, f32 lo, f32 hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void de__water_body(const DeWater *c, de_vec3 origin, const de_spline_poly *river,
                    de_water_body *out) {
    memset(out, 0, sizeof *out);
    out->kind = c->kind;
    out->spectrum = c->spectrum;
    out->wind = c->wind;
    out->choppiness = clampf(c->choppiness, 0.0f, 2.0f);
    out->origin = origin;
    out->level = origin.y + c->surface_height;
    out->extent = c->extent;
    out->width = c->width > 0.0f ? c->width : 0.0f;
    out->flow_speed = c->flow_speed;
    out->river = c->kind == DE_WATER_RIVER && river != NULL && river->n >= 2 ? river : NULL;
}

f32 de__water_wave_height(const de_water_body *b) {
    if (b->kind != DE_WATER_OCEAN)
        return 0.0f;
    const f32 u2 = b->wind.x * b->wind.x + b->wind.y * b->wind.y;
    const f32 k = b->spectrum == DE_WAVE_JONSWAP    ? 1.3f
                  : b->spectrum == DE_WAVE_PHILLIPS ? 0.9f
                                                    : 1.0f;
    return k * 0.21f * u2 / DE_WATER_GRAVITY;
}

static f32 wave(const de_water_body *b, f32 x, f32 z, f32 t) {
    const f32 hs = de__water_wave_height(b);
    if (hs <= 0.0f)
        return b->level;
    f32 speed = hypotf(b->wind.x, b->wind.y);
    if (speed < 0.5f)
        speed = 0.5f;
    const f32 dir = atan2f(b->wind.y, b->wind.x);
    const f32 kp = DE_WATER_GRAVITY / ((1.14f * speed) * (1.14f * speed));
    /* (wavenumber / peak, share of the height, turn from the wind in rad) */
    static const f32 k_waves[4][3] = {
        {1.0f, 0.6f, 0.0f}, {1.5f, 0.25f, 0.3f}, {2.2f, 0.1f, -0.4f}, {3.1f, 0.05f, 0.6f}};
    f32 h = 0.0f;
    for (u32 i = 0; i < 4; ++i) {
        const f32 k = kp * k_waves[i][0], a = 0.5f * hs * k_waves[i][1];
        const f32 s = sinf(dir + k_waves[i][2]), c = cosf(dir + k_waves[i][2]);
        const f32 theta = k * (c * x + s * z) - sqrtf(DE_WATER_GRAVITY * k) * t;
        /* A second harmonic sharpens the crests with choppiness. */
        h += a * (cosf(theta) + 0.5f * b->choppiness * k * a * cosf(2.0f * theta));
    }
    return b->level + h;
}

/* The nearest point of the curve across the ground: distance, point, unit tangent (x, z). */
static f32 nearest(const de_spline_poly *p, f32 x, f32 z, de_vec3 *at, f32 tan[2]) {
    f32 best = INFINITY;
    *at = p->p[0];
    tan[0] = 1.0f;
    tan[1] = 0.0f;
    for (u32 i = 1; i < p->n; ++i) {
        const de_vec3 a = p->p[i - 1], b = p->p[i];
        const f32 dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz;
        const f32 t = l2 > 0.0f ? clampf(((x - a.x) * dx + (z - a.z) * dz) / l2, 0.0f, 1.0f) : 0.0f;
        const de_vec3 q = {a.x + dx * t, a.y + (b.y - a.y) * t, a.z + dz * t};
        const f32 d = hypotf(x - q.x, z - q.z);
        if (d < best) {
            const f32 l = l2 > 1e-12f ? sqrtf(l2) : 1e-6f;
            best = d;
            *at = q;
            tan[0] = dx / l;
            tan[1] = dz / l;
        }
    }
    return best;
}

bool de__water_surface_at(const de_water_body *b, f32 x, f32 z, f32 t, f32 *y) {
    switch (b->kind) {
    case DE_WATER_OCEAN:
        *y = wave(b, x, z, t);
        return true;
    case DE_WATER_LAKE:
        if (fabsf(x - b->origin.x) > b->extent.x * 0.5f ||
            fabsf(z - b->origin.z) > b->extent.z * 0.5f)
            return false;
        *y = b->level;
        return true;
    default: {
        if (b->river == NULL)
            return false;
        de_vec3 q;
        f32 tan[2];
        if (nearest(b->river, x, z, &q, tan) > b->width * 0.5f)
            return false;
        *y = q.y + b->level - b->origin.y;
        return true;
    }
    }
}

bool de__water_flow_map(const de_water_body *b, u32 resolution, f32 *flow, de_water_flow *out) {
    memset(out, 0, sizeof *out);
    if (b->kind != DE_WATER_RIVER || b->river == NULL || resolution < 2 ||
        resolution > DE_WATER_MAX_FLOW)
        return false;
    const de_spline_poly *p = b->river;
    const f32 pad = b->width * 0.5f;
    f32 lo[2] = {INFINITY, INFINITY}, hi[2] = {-INFINITY, -INFINITY};
    for (u32 i = 0; i < p->n; ++i) {
        lo[0] = fminf(lo[0], p->p[i].x - pad);
        lo[1] = fminf(lo[1], p->p[i].z - pad);
        hi[0] = fmaxf(hi[0], p->p[i].x + pad);
        hi[1] = fmaxf(hi[1], p->p[i].z + pad);
    }
    const f32 size[2] = {fmaxf(hi[0] - lo[0], 0.01f), fmaxf(hi[1] - lo[1], 0.01f)};
    const f32 texel = fmaxf(size[0], size[1]) / (f32)resolution;
    const u32 w = (u32)clampf(ceilf(size[0] / texel), 1.0f, (f32)resolution);
    const u32 h = (u32)clampf(ceilf(size[1] / texel), 1.0f, (f32)resolution);
    f32 max_speed = 0.0f;
    for (u32 j = 0; j < h; ++j)
        for (u32 i = 0; i < w; ++i) {
            const f32 x = lo[0] + ((f32)i + 0.5f) * texel, z = lo[1] + ((f32)j + 0.5f) * texel;
            de_vec3 q;
            f32 tan[2];
            const f32 d = nearest(p, x, z, &q, tan);
            const f32 s =
                pad > 0.0f && d <= pad ? b->flow_speed * (1.0f - (d / pad) * (d / pad)) : 0.0f;
            max_speed = fmaxf(max_speed, s);
            flow[2 * (j * w + i)] = tan[0] * s;
            flow[2 * (j * w + i) + 1] = tan[1] * s;
        }
    out->origin[0] = lo[0];
    out->origin[1] = lo[1];
    out->size[0] = size[0];
    out->size[1] = size[1];
    out->width = w;
    out->height = h;
    out->max_speed = max_speed;
    return true;
}

f32 de__water_probe_force(f32 radius, f32 depth) {
    if (!(radius > 0.0f))
        return 0.0f;
    const f32 share = clampf((depth + radius) / (2.0f * radius), 0.0f, 1.0f);
    const f32 volume = 4.0f / 3.0f * DE_PI * radius * radius * radius;
    return DE_WATER_DENSITY * DE_WATER_GRAVITY * volume * share;
}
