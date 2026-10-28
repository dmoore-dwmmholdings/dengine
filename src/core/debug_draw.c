/* debug_draw.c: debug draw (de_debug.h, DENG-572): categories, lines and shapes with lifetimes.
 *
 * Shapes are stored as lines. A mutex guards the store, so any thread may draw; the main thread
 * ages and reads it once per frame (de_draw_update). Shipping compiles every call to nothing. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* snprintf */
#endif
#include <dengine/de_debug.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "platform/de_atomic.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#if !defined(DE_SHIPPING)

#define DD_MAX_CATEGORIES 64u
#define DD_MAX_LINES      65536u

typedef struct dd_category {
    char name[48];
    u32 rgb;
    bool on, game;
    u32 shapes, shapes_now;
} dd_category;

typedef struct dd_line {
    de_dvec3 a, b;
    de_vec4 color;
    f32 life;
    u32 category; /* index + 1 */
} dd_line;

static struct {
    de_atomic_u32 state; /* 0 uninit, 1 initializing, 2 ready */
    de_mutex lock;
    dd_category cats[DD_MAX_CATEGORIES];
    u32 cat_count;
    dd_line *lines;
    u32 line_count;
    bool paused;
} g_dd;

static void dd_init(void) {
    u32 s = de_atomic_load_u32(&g_dd.state, DE_ATOMIC_ACQUIRE);
    if (s == 2)
        return;
    u32 expected = 0;
    if (de_atomic_cas_u32(&g_dd.state, &expected, 1, DE_ATOMIC_ACQ_REL, DE_ATOMIC_ACQUIRE)) {
        de_mutex_init(&g_dd.lock);
        g_dd.lines = (dd_line *)DE_ALLOC(de_heap_allocator(), sizeof(dd_line) * DD_MAX_LINES, 16,
                                         DE_MEM_GENERAL);
        de_atomic_store_u32(&g_dd.state, 2, DE_ATOMIC_RELEASE);
        return;
    }
    while (de_atomic_load_u32(&g_dd.state, DE_ATOMIC_ACQUIRE) != 2)
        de_sleep_ns(1000);
}

de_draw_category de_draw_category_register(const char *name, u32 rgb, bool game) {
    dd_init();
    de_draw_category c = {0};
    if (name == NULL || name[0] == '\0')
        return c;
    de_mutex_lock(&g_dd.lock);
    for (u32 i = 0; i < g_dd.cat_count; ++i)
        if (strcmp(g_dd.cats[i].name, name) == 0) {
            c.id = i + 1;
            de_mutex_unlock(&g_dd.lock);
            return c;
        }
    if (g_dd.cat_count < DD_MAX_CATEGORIES) {
        dd_category *k = &g_dd.cats[g_dd.cat_count++];
        memset(k, 0, sizeof *k);
        snprintf(k->name, sizeof k->name, "%s", name);
        k->rgb = rgb & 0xFFFFFFu;
        k->on = true;
        k->game = game;
        c.id = g_dd.cat_count;
    }
    de_mutex_unlock(&g_dd.lock);
    return c;
}

static void push(u32 cat, de_dvec3 a, de_dvec3 b, de_vec4 color, f32 seconds) {
    if (g_dd.line_count < DD_MAX_LINES && g_dd.lines != NULL)
        g_dd.lines[g_dd.line_count++] = (dd_line){a, b, color, seconds, cat};
}

static bool begin(de_draw_category c) {
    dd_init();
    if (c.id == 0 || c.id > DD_MAX_CATEGORIES)
        return false;
    de_mutex_lock(&g_dd.lock);
    if (c.id > g_dd.cat_count) {
        de_mutex_unlock(&g_dd.lock);
        return false;
    }
    ++g_dd.cats[c.id - 1].shapes_now;
    return true;
}

void de_draw_line(de_draw_category c, de_dvec3 a, de_dvec3 b, de_vec4 color, f32 seconds) {
    if (!begin(c))
        return;
    push(c.id, a, b, color, seconds);
    de_mutex_unlock(&g_dd.lock);
}

void de_draw_arrow(de_draw_category c, de_dvec3 from, de_dvec3 to, de_vec4 color, f32 seconds) {
    if (!begin(c))
        return;
    push(c.id, from, to, color, seconds);
    /* Two head strokes, a fifth of the length, back from the tip. */
    const f64 dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    const f64 len = sqrt(dx * dx + dy * dy + dz * dz);
    if (len > 0) {
        const f64 h = len * 0.2;
        /* A side vector: any axis not parallel to the shaft. */
        f64 sx = -dy, sy = dx, sz = 0;
        if (fabs(sx) + fabs(sy) < 1e-9) {
            sx = 0;
            sy = -dz;
            sz = dy;
        }
        const f64 sl = sqrt(sx * sx + sy * sy + sz * sz);
        sx /= sl;
        sy /= sl;
        sz /= sl;
        const f64 bx = to.x - dx / len * h, by = to.y - dy / len * h, bz = to.z - dz / len * h;
        push(c.id, to, (de_dvec3){bx + sx * h * 0.5, by + sy * h * 0.5, bz + sz * h * 0.5}, color, seconds);
        push(c.id, to, (de_dvec3){bx - sx * h * 0.5, by - sy * h * 0.5, bz - sz * h * 0.5}, color, seconds);
    }
    de_mutex_unlock(&g_dd.lock);
}

void de_draw_box(de_draw_category c, de_dvec3 mn, de_dvec3 mx, de_vec4 color, f32 seconds) {
    if (!begin(c))
        return;
    const de_dvec3 p[8] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
                           {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
    static const u8 e[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (u32 i = 0; i < 12; ++i)
        push(c.id, p[e[i][0]], p[e[i][1]], color, seconds);
    de_mutex_unlock(&g_dd.lock);
}

void de_draw_sphere(de_draw_category c, de_dvec3 o, f32 r, de_vec4 color, f32 seconds) {
    if (!begin(c))
        return;
    enum { SEG = 24 };
    for (u32 axis = 0; axis < 3; ++axis)
        for (u32 i = 0; i < SEG; ++i) {
            const f64 a0 = 6.283185307179586 * i / SEG, a1 = 6.283185307179586 * (i + 1) / SEG;
            de_dvec3 p0 = o, p1 = o;
            const f64 c0 = cos(a0) * r, s0 = sin(a0) * r, c1 = cos(a1) * r, s1 = sin(a1) * r;
            if (axis == 0) {
                p0.y += c0, p0.z += s0, p1.y += c1, p1.z += s1;
            } else if (axis == 1) {
                p0.x += c0, p0.z += s0, p1.x += c1, p1.z += s1;
            } else {
                p0.x += c0, p0.y += s0, p1.x += c1, p1.y += s1;
            }
            push(c.id, p0, p1, color, seconds);
        }
    de_mutex_unlock(&g_dd.lock);
}

void de_draw_text(de_draw_category c, de_dvec3 at, const char *text, de_vec4 color, f32 seconds) {
    (void)text;
    if (!begin(c))
        return;
    /* A small cross marks the label's anchor. */
    const f64 k = 0.05;
    push(c.id, (de_dvec3){at.x - k, at.y, at.z}, (de_dvec3){at.x + k, at.y, at.z}, color, seconds);
    push(c.id, (de_dvec3){at.x, at.y, at.z - k}, (de_dvec3){at.x, at.y, at.z + k}, color, seconds);
    de_mutex_unlock(&g_dd.lock);
}

static dd_category *find(const char *name) {
    for (u32 i = 0; i < g_dd.cat_count; ++i)
        if (strcmp(g_dd.cats[i].name, name) == 0)
            return &g_dd.cats[i];
    return NULL;
}

void de_draw_set_enabled(const char *category, bool on) {
    dd_init();
    de_mutex_lock(&g_dd.lock);
    dd_category *k = category ? find(category) : NULL;
    if (k != NULL)
        k->on = on;
    de_mutex_unlock(&g_dd.lock);
}

void de_draw_set_color(const char *category, u32 rgb) {
    dd_init();
    de_mutex_lock(&g_dd.lock);
    dd_category *k = category ? find(category) : NULL;
    if (k != NULL)
        k->rgb = rgb & 0xFFFFFFu;
    de_mutex_unlock(&g_dd.lock);
}

void de_draw_set_paused(bool paused) {
    dd_init();
    de_mutex_lock(&g_dd.lock);
    g_dd.paused = paused;
    de_mutex_unlock(&g_dd.lock);
}

bool de_draw_paused(void) {
    dd_init();
    return g_dd.paused;
}

u32 de_draw_categories(de_draw_category_info *out, u32 cap) {
    dd_init();
    de_mutex_lock(&g_dd.lock);
    const u32 n = g_dd.cat_count;
    for (u32 i = 0; out != NULL && i < n && i < cap; ++i)
        out[i] = (de_draw_category_info){g_dd.cats[i].name, g_dd.cats[i].rgb, g_dd.cats[i].on,
                                         g_dd.cats[i].game, g_dd.cats[i].shapes};
    de_mutex_unlock(&g_dd.lock);
    return n;
}

u32 de_draw_update(f32 dt, de_draw_line_item *out, u32 cap) {
    dd_init();
    de_mutex_lock(&g_dd.lock);
    u32 n = 0;
    for (u32 i = 0; i < g_dd.line_count; ++i) {
        const dd_line *l = &g_dd.lines[i];
        const dd_category *k = &g_dd.cats[l->category - 1];
        if (out != NULL && n < cap && k->on) {
            de_vec4 col = l->color;
            if (col.w <= 0)
                col = (de_vec4){(f32)((k->rgb >> 16) & 0xFF) / 255.0f, (f32)((k->rgb >> 8) & 0xFF) / 255.0f,
                                (f32)(k->rgb & 0xFF) / 255.0f, 1.0f};
            out[n++] = (de_draw_line_item){l->a, l->b, col};
        }
    }
    if (!g_dd.paused) {
        u32 w = 0;
        for (u32 i = 0; i < g_dd.line_count; ++i) {
            dd_line l = g_dd.lines[i];
            l.life -= dt;
            if (l.life > 0)
                g_dd.lines[w++] = l;
        }
        g_dd.line_count = w;
        for (u32 i = 0; i < g_dd.cat_count; ++i) {
            g_dd.cats[i].shapes = g_dd.cats[i].shapes_now;
            g_dd.cats[i].shapes_now = 0;
        }
    }
    de_mutex_unlock(&g_dd.lock);
    return n;
}

#else /* DE_SHIPPING */

de_draw_category de_draw_category_register(const char *name, u32 rgb, bool game) {
    (void)name, (void)rgb, (void)game;
    return (de_draw_category){0};
}
void de_draw_line(de_draw_category c, de_dvec3 a, de_dvec3 b, de_vec4 color, f32 seconds) {
    (void)c, (void)a, (void)b, (void)color, (void)seconds;
}
void de_draw_arrow(de_draw_category c, de_dvec3 a, de_dvec3 b, de_vec4 color, f32 seconds) {
    (void)c, (void)a, (void)b, (void)color, (void)seconds;
}
void de_draw_box(de_draw_category c, de_dvec3 a, de_dvec3 b, de_vec4 color, f32 seconds) {
    (void)c, (void)a, (void)b, (void)color, (void)seconds;
}
void de_draw_sphere(de_draw_category c, de_dvec3 o, f32 r, de_vec4 color, f32 seconds) {
    (void)c, (void)o, (void)r, (void)color, (void)seconds;
}
void de_draw_text(de_draw_category c, de_dvec3 at, const char *t, de_vec4 color, f32 seconds) {
    (void)c, (void)at, (void)t, (void)color, (void)seconds;
}
void de_draw_set_enabled(const char *category, bool on) {
    (void)category, (void)on;
}
void de_draw_set_color(const char *category, u32 rgb) {
    (void)category, (void)rgb;
}
void de_draw_set_paused(bool paused) {
    (void)paused;
}
bool de_draw_paused(void) {
    return false;
}
u32 de_draw_categories(de_draw_category_info *out, u32 cap) {
    (void)out, (void)cap;
    return 0;
}
u32 de_draw_update(f32 dt, de_draw_line_item *out, u32 cap) {
    (void)dt, (void)out, (void)cap;
    return 0;
}

#endif
