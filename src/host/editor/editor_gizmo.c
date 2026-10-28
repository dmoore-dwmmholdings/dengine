/* editor_gizmo.c: the transform gizmo and the scene overlays of editor views (DENG-650).
 *
 * The engine draws the gizmo, hit-tests it and drives the drag (DIDE-R142); the IDE sets the
 * tool, space, pivot, snap and colours (gizmo.setTool) and the selection (gizmo.setSelection),
 * forwards the view's pointer and keys, and commits each drag's gizmo.dragEnd ops as one undo
 * step. Everything is drawn on the CPU into the frame the view hands the IDE (editor_views.c),
 * after the scene: the grid, the selected entities' outlines, the frames of the scene's cameras,
 * then the gizmo.
 *
 * Handles, as in Unity and Godot:
 *   move      an arrow per axis, a square per plane, a centre square (the view plane);
 *   rotate    a ring per axis (the half facing the camera) and a view ring;
 *   scale     an axis line with a box per (local) axis, a centre box (uniform);
 *   universal move's handles and rotate's rings;
 *   rect      the selection's rectangle in its plane (sprites: the sprite), its corners and edges
 *             resize it (scale and position, the opposite side stays), inside moves it.
 * A handle whose axis points at the camera, and a plane or ring seen edge-on, is not shown, so a
 * side or top 2D view shows the two in-plane axes, their plane and the view ring. The pointer
 * highlights the handle under it; a press elsewhere is the IDE's (pick, box selection).
 *
 * Drags follow the pointer exactly: an axis drag moves to the point of the axis nearest the
 * pointer's ray, a plane drag to the ray's hit on the plane, a ring drag turns by the angle the
 * pointer swept about the centre. Snapping (gizmo.setTool's steps, or with Ctrl/Cmd 0.25 m,
 * 15 deg, 0.1) puts the first entity's pivot on the grid along the axes a move drags, and turns
 * and scales by whole steps; digits typed during an axis drag set the exact distance, angle or
 * factor. A drag writes
 * the entities' DeTransform while it runs (gizmo.dragUpdate previews it), puts it back at the end
 * and sends the result as gizmo.dragEnd ops, which the IDE applies through edit.*. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* snprintf */
#endif
#include "host/editor/editor_gizmo.h"

#include <dengine/de_ecs.h>
#include <dengine/de_math.h>
#include <dengine/de_physics.h>
#include <dengine/de_render.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GZ_MAX 256u

/* ---------- Vectors ---------- */

typedef de_dvec3 v3;

static v3 V(f64 x, f64 y, f64 z) { return (v3){x, y, z}; }
static v3 add(v3 a, v3 b) { return V(a.x + b.x, a.y + b.y, a.z + b.z); }
static v3 sub(v3 a, v3 b) { return V(a.x - b.x, a.y - b.y, a.z - b.z); }
static v3 mul(v3 a, f64 s) { return V(a.x * s, a.y * s, a.z * s); }
static f64 dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 cross(v3 a, v3 b) { return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static f64 len(v3 a) { return sqrt(dot(a, a)); }
static v3 nrm(v3 a) {
    const f64 l = len(a);
    return l > 1e-12 ? mul(a, 1.0 / l) : V(0, 0, 0);
}
static f64 comp(v3 a, u32 i) { return i == 0 ? a.x : i == 1 ? a.y : a.z; }
static v3 unit(u32 i) { return V(i == 0, i == 1, i == 2); }

static v3 qrot(de_quat q, v3 v) {
    const f64 x = q.x, y = q.y, z = q.z, w = q.w;
    const f64 tx = 2 * (y * v.z - z * v.y), ty = 2 * (z * v.x - x * v.z), tz = 2 * (x * v.y - y * v.x);
    return V(v.x + w * tx + (y * tz - z * ty), v.y + w * ty + (z * tx - x * tz), v.z + w * tz + (x * ty - y * tx));
}
static de_quat qconj(de_quat q) { return (de_quat){-q.x, -q.y, -q.z, q.w}; }
static de_quat qmul(de_quat a, de_quat b) {
    return (de_quat){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static de_quat qaxis(v3 axis, f64 rad) {
    const v3 a = nrm(axis);
    const f64 s = sin(rad * 0.5);
    return (de_quat){(f32)(a.x * s), (f32)(a.y * s), (f32)(a.z * s), (f32)cos(rad * 0.5)};
}

/* ---------- The camera ---------- */

static de_quat cam_q(const de__ed_cam *c) { return (de_quat){c->rot[0], c->rot[1], c->rot[2], c->rot[3]}; }
static v3 cam_pos(const de__ed_cam *c) { return V(c->pos[0], c->pos[1], c->pos[2]); }
static v3 cam_fwd(const de__ed_cam *c) { return qrot(cam_q(c), V(0, 1, 0)); }
static bool ortho(const de__ed_cam *c) { return !(c->fov_y > 0); }
static f64 cam_near(const de__ed_cam *c) { return c->near > 0 ? c->near : 0.1; }
static f64 aspect(const de__ed_cam *c) { return (f64)c->w / (f64)(c->h ? c->h : 1); }
static f64 half_h(const de__ed_cam *c) { return (c->ortho_h > 0 ? c->ortho_h : 10.0) * 0.5; }

/* World to camera space: x right, y forward (depth), z up. */
static v3 to_cam(const de__ed_cam *c, v3 p) { return qrot(qconj(cam_q(c)), sub(p, cam_pos(c))); }

static void cam_to_px(const de__ed_cam *c, v3 l, f64 *x, f64 *y) {
    f64 nx, ny;
    if (ortho(c)) {
        nx = l.x / (half_h(c) * aspect(c));
        ny = l.z / half_h(c);
    } else {
        const f64 t = tan(c->fov_y * 0.5), d = l.y > 1e-9 ? l.y : 1e-9;
        nx = l.x / (d * t * aspect(c));
        ny = l.z / (d * t);
    }
    *x = (nx * 0.5 + 0.5) * c->w;
    *y = (0.5 - ny * 0.5) * c->h;
}

/* World point to pixels; false in front of a perspective camera's near plane. An orthographic
 * (2D) view draws its overlays whatever their depth: a 2D scene's camera often sits behind the
 * editor's, and depth means nothing on screen there. */
static bool proj(const de__ed_cam *c, v3 p, f64 *x, f64 *y) {
    const v3 l = to_cam(c, p);
    if (!ortho(c) && l.y < cam_near(c))
        return false;
    cam_to_px(c, l, x, y);
    return true;
}

/* The pointer's ray: origin and unit direction. */
static void ray(const de__ed_cam *c, f64 x, f64 y, v3 *o, v3 *d) {
    const de_quat q = cam_q(c);
    const v3 r = qrot(q, V(1, 0, 0)), f = qrot(q, V(0, 1, 0)), u = qrot(q, V(0, 0, 1));
    const f64 nx = x / c->w * 2.0 - 1.0, ny = 1.0 - y / c->h * 2.0;
    if (ortho(c)) {
        *o = add(cam_pos(c), add(mul(r, nx * half_h(c) * aspect(c)), mul(u, ny * half_h(c))));
        *d = f;
    } else {
        const f64 t = tan(c->fov_y * 0.5);
        *o = cam_pos(c);
        *d = nrm(add(f, add(mul(r, nx * t * aspect(c)), mul(u, ny * t))));
    }
}

/* Pixels per metre at depth `d`. */
static f64 ppm(const de__ed_cam *c, f64 d) {
    if (ortho(c))
        return c->h / (2.0 * half_h(c));
    return c->h / (2.0 * (d > 1e-6 ? d : 1e-6) * tan(c->fov_y * 0.5));
}

/* The direction from the camera to `p` (the view axis there). */
static v3 view_dir(const de__ed_cam *c, v3 p) { return ortho(c) ? cam_fwd(c) : nrm(sub(p, cam_pos(c))); }

/* The ray's hit on the plane through `o` with normal `n`; false when parallel or behind. */
static bool ray_plane(v3 ro, v3 rd, v3 o, v3 n, v3 *hit) {
    const f64 den = dot(rd, n);
    if (fabs(den) < 1e-6)
        return false;
    const f64 s = dot(sub(o, ro), n) / den;
    if (s < 0 && fabs(s) > 1e6)
        return false;
    *hit = add(ro, mul(rd, s));
    return true;
}

/* The parameter of the point of the line o + a t (a unit) nearest the ray. */
static bool ray_axis(v3 ro, v3 rd, v3 o, v3 a, f64 *t) {
    const v3 w0 = sub(o, ro);
    const f64 b = dot(a, rd), d = dot(a, w0), e = dot(rd, w0), den = 1.0 - b * b;
    if (den < 1e-6)
        return false;
    *t = (b * e - d) / den;
    return true;
}

/* ---------- Drawing (sRGB RGBA8) ---------- */

typedef struct rgb {
    u8 r, g, b;
} rgb;

static void blend(const de__ed_canvas *cv, i32 x, i32 y, rgb c, f32 a) {
    if (x < 0 || y < 0 || (u32)x >= cv->w || (u32)y >= cv->h)
        return;
    u8 *p = cv->map + (size_t)y * cv->pitch + (size_t)x * 4u;
    if (a >= 0.999f) {
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
        return;
    }
    p[0] = (u8)(p[0] + (c.r - p[0]) * a);
    p[1] = (u8)(p[1] + (c.g - p[1]) * a);
    p[2] = (u8)(p[2] + (c.b - p[2]) * a);
}

/* Clips a segment to the canvas (plus a margin); false when it is outside. */
static bool clip(const de__ed_canvas *cv, f64 *x0, f64 *y0, f64 *x1, f64 *y1) {
    const f64 lo[2] = {-4, -4}, hi[2] = {cv->w + 4.0, cv->h + 4.0};
    f64 t0 = 0, t1 = 1;
    const f64 d[2] = {*x1 - *x0, *y1 - *y0}, s[2] = {*x0, *y0};
    for (u32 i = 0; i < 2; ++i) {
        const f64 p[2] = {-d[i], d[i]}, q[2] = {s[i] - lo[i], hi[i] - s[i]};
        for (u32 k = 0; k < 2; ++k) {
            if (fabs(p[k]) < 1e-12) {
                if (q[k] < 0)
                    return false;
                continue;
            }
            const f64 r = q[k] / p[k];
            if (p[k] < 0) {
                if (r > t1)
                    return false;
                if (r > t0)
                    t0 = r;
            } else {
                if (r < t0)
                    return false;
                if (r < t1)
                    t1 = r;
            }
        }
    }
    *x1 = s[0] + d[0] * t1;
    *y1 = s[1] + d[1] * t1;
    *x0 = s[0] + d[0] * t0;
    *y0 = s[1] + d[1] * t0;
    return true;
}

static void seg(const de__ed_canvas *cv, f64 x0, f64 y0, f64 x1, f64 y1, rgb c, f32 a, u32 width) {
    if (!clip(cv, &x0, &y0, &x1, &y1))
        return;
    const f64 dx = x1 - x0, dy = y1 - y0;
    const f64 n = fmax(fabs(dx), fabs(dy));
    const bool horiz = fabs(dx) >= fabs(dy);
    const i32 lo = -(i32)((width - 1) / 2), hi = (i32)(width / 2);
    for (i32 i = 0; i <= (i32)ceil(n); ++i) {
        const f64 t = n > 0 ? i / n : 0;
        const i32 x = (i32)floor(x0 + dx * t + 0.5), y = (i32)floor(y0 + dy * t + 0.5);
        for (i32 k = lo; k <= hi; ++k)
            blend(cv, horiz ? x : x + k, horiz ? y + k : y, c, a);
    }
}

/* A world segment, clipped to the near plane. */
static void wseg(const de__ed_cam *c, const de__ed_canvas *cv, v3 p0, v3 p1, rgb col, f32 a, u32 width) {
    v3 l0 = to_cam(c, p0), l1 = to_cam(c, p1);
    const f64 n = ortho(c) ? -1e30 : cam_near(c);
    if (l0.y < n && l1.y < n)
        return;
    if (l0.y < n)
        l0 = add(l0, mul(sub(l1, l0), (n - l0.y) / (l1.y - l0.y)));
    else if (l1.y < n)
        l1 = add(l1, mul(sub(l0, l1), (n - l1.y) / (l0.y - l1.y)));
    f64 x0, y0, x1, y1;
    cam_to_px(c, l0, &x0, &y0);
    cam_to_px(c, l1, &x1, &y1);
    seg(cv, x0, y0, x1, y1, col, a, width);
}

static f64 edge(f64 ax, f64 ay, f64 bx, f64 by, f64 px, f64 py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }

static void tri(const de__ed_canvas *cv, const f64 p[6], rgb c, f32 a) {
    const f64 minx = fmax(0, floor(fmin(p[0], fmin(p[2], p[4])))), maxx = fmin(cv->w - 1.0, ceil(fmax(p[0], fmax(p[2], p[4]))));
    const f64 miny = fmax(0, floor(fmin(p[1], fmin(p[3], p[5])))), maxy = fmin(cv->h - 1.0, ceil(fmax(p[1], fmax(p[3], p[5]))));
    const f64 area = edge(p[0], p[1], p[2], p[3], p[4], p[5]);
    if (fabs(area) < 1e-9)
        return;
    for (f64 y = miny; y <= maxy; ++y)
        for (f64 x = minx; x <= maxx; ++x) {
            const f64 px = x + 0.5, py = y + 0.5;
            const f64 w0 = edge(p[2], p[3], p[4], p[5], px, py) / area, w1 = edge(p[4], p[5], p[0], p[1], px, py) / area,
                      w2 = edge(p[0], p[1], p[2], p[3], px, py) / area;
            if (w0 >= 0 && w1 >= 0 && w2 >= 0)
                blend(cv, (i32)x, (i32)y, c, a);
        }
}

static void quad(const de__ed_canvas *cv, const f64 p[8], rgb c, f32 a) {
    const f64 t0[6] = {p[0], p[1], p[2], p[3], p[4], p[5]}, t1[6] = {p[0], p[1], p[4], p[5], p[6], p[7]};
    tri(cv, t0, c, a);
    tri(cv, t1, c, a);
}

static void box(const de__ed_canvas *cv, f64 cx, f64 cy, f64 half, rgb c, f32 a) {
    for (i32 y = (i32)floor(cy - half); y <= (i32)ceil(cy + half); ++y)
        for (i32 x = (i32)floor(cx - half); x <= (i32)ceil(cx + half); ++x)
            blend(cv, x, y, c, a);
}

static void ring_px(const de__ed_canvas *cv, f64 cx, f64 cy, f64 r, rgb c, f32 a, u32 width) {
    enum { N = 96 };
    for (u32 i = 0; i < N; ++i) {
        const f64 a0 = 2 * DE_PI * i / N, a1 = 2 * DE_PI * (i + 1) / N;
        seg(cv, cx + cos(a0) * r, cy + sin(a0) * r, cx + cos(a1) * r, cy + sin(a1) * r, c, a, width);
    }
}

/* Distance from (px, py) to the segment (x0, y0)-(x1, y1). */
static f64 dist_seg(f64 px, f64 py, f64 x0, f64 y0, f64 x1, f64 y1) {
    const f64 dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    f64 t = l2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / l2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    const f64 ex = x0 + dx * t - px, ey = y0 + dy * t - py;
    return sqrt(ex * ex + ey * ey);
}

static bool inside(const f64 p[8], f64 x, f64 y) {
    bool pos = false, neg = false;
    for (u32 i = 0; i < 4; ++i) {
        const u32 j = (i + 1) % 4;
        const f64 e = edge(p[i * 2], p[i * 2 + 1], p[j * 2], p[j * 2 + 1], x, y);
        pos |= e > 0;
        neg |= e < 0;
    }
    return !(pos && neg);
}

/* ---------- State ---------- */

enum {
    H_NONE,
    H_MX, H_MY, H_MZ,       /* move along an axis */
    H_MYZ, H_MXZ, H_MXY,    /* move in a plane (normal: x, y, z) */
    H_MC,                   /* move in the view plane */
    H_RX, H_RY, H_RZ, H_RV, /* rotate about an axis, about the view axis */
    H_SX, H_SY, H_SZ, H_SC, /* scale along a local axis, uniformly */
    H_RIN,                  /* rect: inside */
    H_RL, H_RR, H_RB, H_RT, /* rect: an edge */
    H_RLB, H_RRB, H_RLT, H_RRT,
    H_COUNT
};

enum { C_X, C_Y, C_Z, C_HOVER, C_SELECTED, C_COUNT };

typedef struct gz_ent {
    u64 e;
    de_transform local0, world0, parent; /* parent: the parent's world transform (or identity) */
    bool child;
} gz_ent;

typedef struct gz_layout {
    bool ok;
    v3 o;     /* the gizmo's centre */
    v3 ax[3]; /* its axes */
    v3 dir;   /* the view direction at the centre */
    f64 size; /* the handles' length in metres */
    /* rect */
    bool rect;
    v3 rp, ru, rv;   /* the rectangle's origin (the first entity) and in-plane axes */
    f64 r[4];        /* umin, umax, vmin, vmax (metres from rp) */
    de_vec2 pivot;   /* the first entity's sprite pivot (0.5 without one) */
} gz_layout;

typedef struct ed_gz {
    char tool[16], space[8], pivot[8];
    f32 snap[3];
    rgb col[C_COUNT];
    u64 sel[GZ_MAX];
    u32 sel_count;
    u32 hover_view;
    i32 hover;
    /* The drag. */
    bool drag, moved, ctrl;
    bool shift;   /* Shift: on the surface under the pointer */
    bool vkey;    /* V held: snap to the nearest point (a bounds corner, an origin) */
    bool align;   /* surfaceAlign: turn +Z onto the surface's normal */
    de__editor *ed;
    u32 view;
    i32 h;
    f32 x0, y0, x, y;
    gz_layout L;  /* at the press */
    v3 hit0;
    f64 t0;
    f64 ang, ang_prev;
    bool ang_screen; /* the ring is edge-on: the angle follows the pointer's travel */
    gz_ent ents[GZ_MAX];
    u32 n;
    char typed[24];
} ed_gz;

static ed_gz G = {
    .tool = "move",
    .space = "world",
    .pivot = "center",
    .col = {{232, 72, 85}, {98, 200, 92}, {66, 133, 244}, {255, 214, 10}, {255, 159, 10}},
};

static const f32 k_default_steps[3] = {0.25f, 15.0f, 0.1f};

static bool tool_is(const char *t) { return strcmp(G.tool, t) == 0; }
static bool tool_drags(void) {
    return tool_is("move") || tool_is("rotate") || tool_is("scale") || tool_is("universal") || tool_is("rect");
}

static bool parse_hex(de_ep_str s, rgb *out) {
    if (s.len != 7 || s.ptr[0] != '#')
        return false;
    char b[8];
    memcpy(b, s.ptr, 7);
    b[7] = '\0';
    char *end = NULL;
    const unsigned long v = strtoul(b + 1, &end, 16);
    if (end == NULL || *end != '\0')
        return false;
    *out = (rgb){(u8)(v >> 16), (u8)(v >> 8), (u8)v};
    return true;
}

void de__ed_gizmo_set_tool(const de_ep_gizmo_tool_params *q) {
    snprintf(G.tool, sizeof G.tool, "%.*s", (int)q->tool.len, q->tool.ptr);
    snprintf(G.space, sizeof G.space, "%.*s", (int)q->space.len, q->space.ptr);
    snprintf(G.pivot, sizeof G.pivot, "%.*s", (int)q->pivot.len, q->pivot.ptr);
    G.align = false;
    for (u32 i = 0; i < q->snap.count; ++i)
        if (de_ep_str_eq(q->snap.items[i].key, "surfaceAlign"))
            G.align = q->snap.items[i].value > 0;
    static const char *const keys[3] = {"move", "rotate", "scale"};
    for (u32 k = 0; k < 3; ++k) {
        G.snap[k] = 0;
        for (u32 i = 0; i < q->snap.count; ++i)
            if (de_ep_str_eq(q->snap.items[i].key, keys[k]))
                G.snap[k] = q->snap.items[i].value;
    }
    if (q->has_colors) {
        static const char *const names[C_COUNT] = {"x", "y", "z", "hover", "selected"};
        for (u32 k = 0; k < C_COUNT; ++k)
            for (u32 i = 0; i < q->colors.count; ++i)
                if (de_ep_str_eq(q->colors.items[i].key, names[k]))
                    (void)parse_hex(q->colors.items[i].value, &G.col[k]);
    }
    G.hover = H_NONE;
}

void de__ed_gizmo_set_selection(const u64 *entities, u32 count) {
    G.sel_count = 0;
    for (u32 i = 0; i < count && i < GZ_MAX; ++i)
        G.sel[G.sel_count++] = entities[i];
    G.hover = H_NONE;
}

u32 de__ed_show_flags(const de_ep_list_str *names) {
    u32 f = 0;
    for (u32 i = 0; i < names->count; ++i) {
        const de_ep_str s = names->items[i];
        if (de_ep_str_eq(s, "grid"))
            f |= DE__ED_SHOW_GRID;
        else if (de_ep_str_eq(s, "gizmos"))
            f |= DE__ED_SHOW_GIZMOS;
        else if (de_ep_str_eq(s, "selectionOutline"))
            f |= DE__ED_SHOW_OUTLINE;
        else if (de_ep_str_eq(s, "icons.cameras"))
            f |= DE__ED_SHOW_CAMERAS;
        else if (de_ep_str_eq(s, "icons.lights"))
            f |= DE__ED_SHOW_LIGHTS;
        else if (de_ep_str_eq(s, "colliders"))
            f |= DE__ED_SHOW_COLLIDERS;
    }
    return f;
}

/* ---------- The selection in the world ---------- */

static bool ent_of(de_world *w, u64 bits, gz_ent *out) {
    const de_builtin_component_ids b = de_builtin_components(w);
    const de_entity e = {bits};
    const DeTransform *l = (const DeTransform *)de_entity_get_const(w, e, b.transform);
    if (l == NULL)
        return false;
    const DeGlobalTransform *g = (const DeGlobalTransform *)de_entity_get_const(w, e, b.global_transform);
    const DeChildOf *co = (const DeChildOf *)de_entity_get_const(w, e, b.child_of);
    out->e = bits;
    out->local0 = *l;
    out->child = co != NULL && de_entity_alive(w, co->parent);
    out->parent = de_transform_identity();
    if (out->child) {
        const DeGlobalTransform *pg = (const DeGlobalTransform *)de_entity_get_const(w, co->parent, b.global_transform);
        if (pg != NULL)
            out->parent = *pg;
    }
    /* A root's world transform is its local one (exactly: no round trip through the global). */
    out->world0 = !out->child ? *l : g != NULL ? *g : de_transform_mul(&out->parent, l);
    return true;
}

static const DeSprite *sprite_of(de_world *w, u64 bits) {
    const de_component c = de_component_find(w, "DeSprite");
    return c.id != 0 ? (const DeSprite *)de_entity_get_const(w, (de_entity){bits}, c) : NULL;
}

/* The rectangle of an entity in its 2D plane: a sprite's quad, or a 1 m square. Its v axis is
 * the local axis (Z for side views, Y for top views) more across the view. */
static void rect_of(de_world *w, const de__ed_cam *c, const gz_ent *g, gz_layout *L) {
    const DeSprite *s = sprite_of(w, g->e);
    const de_transform *t = &g->world0;
    const v3 f = cam_fwd(c);
    const v3 lz = qrot(t->rotation, V(0, 0, 1)), ly = qrot(t->rotation, V(0, 1, 0));
    const bool side = fabs(dot(lz, f)) <= fabs(dot(ly, f));
    L->rp = t->position;
    L->ru = qrot(t->rotation, V(1, 0, 0));
    L->rv = side ? lz : ly;
    const f64 su = fabs(t->scale.x), sv = fabs(side ? t->scale.z : t->scale.y);
    const f64 w0 = s != NULL ? s->size.x * su : su, h0 = s != NULL ? s->size.y * sv : sv;
    L->pivot = s != NULL ? s->pivot : (de_vec2){0.5f, 0.5f};
    L->r[0] = -L->pivot.x * w0;
    L->r[1] = L->r[0] + w0;
    L->r[2] = -L->pivot.y * h0;
    L->r[3] = L->r[2] + h0;
    L->rect = true;
}

/* Where the gizmo is and how big, from the selection's current transforms. */
static gz_layout layout(de_world *w, const de__ed_cam *c) {
    gz_layout L;
    memset(&L, 0, sizeof L);
    gz_ent first;
    u32 n = 0;
    v3 sum = V(0, 0, 0);
    for (u32 i = 0; i < G.sel_count; ++i) {
        gz_ent g;
        if (!ent_of(w, G.sel[i], &g))
            continue;
        if (n++ == 0)
            first = g;
        sum = add(sum, g.world0.position);
    }
    if (n == 0)
        return L;
    L.o = strcmp(G.pivot, "center") == 0 ? mul(sum, 1.0 / n) : first.world0.position;
    de_quat frame = {0, 0, 0, 1};
    if (tool_is("scale") || strcmp(G.space, "local") == 0)
        frame = first.world0.rotation;
    else if (strcmp(G.space, "parent") == 0)
        frame = first.parent.rotation;
    for (u32 i = 0; i < 3; ++i)
        L.ax[i] = nrm(qrot(frame, unit(i)));
    L.dir = view_dir(c, L.o);
    const f64 depth = to_cam(c, L.o).y;
    if (!ortho(c) && depth < cam_near(c))
        return L;
    f64 px = c->h * 0.12;
    px = px < 64 ? 64 : px > 200 ? 200 : px;
    L.size = px / ppm(c, depth);
    if (tool_is("rect"))
        rect_of(w, c, &first, &L);
    L.ok = true;
    return L;
}

/* Which handles a tool shows, and whether the view lets one be used. */
static bool shown(const gz_layout *L, i32 h) {
    const bool mv = tool_is("move") || tool_is("universal"), rot = tool_is("rotate") || tool_is("universal"),
               sc = tool_is("scale"), rc = tool_is("rect");
    if (h >= H_MX && h <= H_MZ)
        return mv && fabs(dot(L->ax[h - H_MX], L->dir)) < 0.985;
    if (h >= H_MYZ && h <= H_MXY)
        return mv && fabs(dot(L->ax[h - H_MYZ], L->dir)) > 0.2;
    if (h == H_MC)
        return mv;
    if (h >= H_RX && h <= H_RZ)
        return rot && fabs(dot(L->ax[h - H_RX], L->dir)) > 0.12;
    if (h == H_RV)
        return rot;
    if (h >= H_SX && h <= H_SZ)
        return sc && fabs(dot(L->ax[h - H_SX], L->dir)) < 0.985;
    if (h == H_SC)
        return sc;
    return rc && L->rect && h >= H_RIN;
}

/* The ring of handle `h` (H_RX..H_RV): centre, the two in-plane axes, radius. */
static void ring_of(const gz_layout *L, i32 h, v3 *u, v3 *v, f64 *r) {
    v3 a = h == H_RV ? L->dir : L->ax[h - H_RX];
    v3 t = fabs(a.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0);
    *u = nrm(cross(a, t));
    *v = nrm(cross(a, *u));
    *r = L->size * (h == H_RV ? 1.1 : 0.9);
}

/* The rectangle's corners (in order: left-bottom, right-bottom, right-top, left-top) in px. */
static bool rect_px(const de__ed_cam *c, const gz_layout *L, f64 p[8]) {
    const f64 us[4] = {L->r[0], L->r[1], L->r[1], L->r[0]}, vs[4] = {L->r[2], L->r[2], L->r[3], L->r[3]};
    for (u32 i = 0; i < 4; ++i)
        if (!proj(c, add(L->rp, add(mul(L->ru, us[i]), mul(L->rv, vs[i]))), &p[i * 2], &p[i * 2 + 1]))
            return false;
    return true;
}

/* The handle under (x, y), or H_NONE. */
static i32 hit_test(const de__ed_cam *c, const gz_layout *L, f64 x, f64 y) {
    if (!L->ok)
        return H_NONE;
    f64 ox, oy;
    if (!proj(c, L->o, &ox, &oy))
        return H_NONE;
    i32 best = H_NONE;
    f64 best_d = 1e30;
    const f64 S = L->size;
    /* Bias: the smaller, inner handles win near a tie. */
#define TRY(h, d, reach, bias)                                                                      \
    do {                                                                                           \
        if ((d) <= (reach) && (d) + (bias) < best_d) {                                             \
            best = (h);                                                                            \
            best_d = (d) + (bias);                                                                 \
        }                                                                                          \
    } while (0)
    if (tool_is("rect")) {
        f64 p[8];
        if (!L->rect || !rect_px(c, L, p))
            return H_NONE;
        static const i32 corner[4] = {H_RLB, H_RRB, H_RRT, H_RLT};
        static const i32 side[4] = {H_RB, H_RR, H_RT, H_RL};
        for (u32 i = 0; i < 4; ++i) {
            const u32 j = (i + 1) % 4;
            TRY(corner[i], hypot(p[i * 2] - x, p[i * 2 + 1] - y), 9.0, -3.0);
            TRY(side[i], dist_seg(x, y, p[i * 2], p[i * 2 + 1], p[j * 2], p[j * 2 + 1]), 6.0, 0.0);
        }
        if (best == H_NONE && inside(p, x, y))
            best = H_RIN;
        return best;
    }
    if (shown(L, H_MC))
        TRY(H_MC, fmax(fabs(x - ox), fabs(y - oy)), 9.0, -4.0);
    if (shown(L, H_SC))
        TRY(H_SC, fmax(fabs(x - ox), fabs(y - oy)), 10.0, -4.0);
    for (u32 k = 0; k < 3; ++k) {
        f64 ex, ey;
        if (shown(L, H_MX + (i32)k) && proj(c, add(L->o, mul(L->ax[k], S)), &ex, &ey))
            TRY(H_MX + (i32)k, dist_seg(x, y, ox, oy, ex, ey), 8.0, 0.0);
        if (shown(L, H_SX + (i32)k) && proj(c, add(L->o, mul(L->ax[k], S)), &ex, &ey))
            TRY(H_SX + (i32)k, dist_seg(x, y, ox, oy, ex, ey), 8.0, 0.0);
        if (shown(L, H_MYZ + (i32)k)) {
            const v3 a = L->ax[(k + 1) % 3], b = L->ax[(k + 2) % 3];
            f64 q[8];
            const f64 lo = S * 0.2, hi = S * 0.42;
            const v3 pts[4] = {add(L->o, add(mul(a, lo), mul(b, lo))), add(L->o, add(mul(a, hi), mul(b, lo))),
                               add(L->o, add(mul(a, hi), mul(b, hi))), add(L->o, add(mul(a, lo), mul(b, hi)))};
            bool ok = true;
            for (u32 i = 0; i < 4 && ok; ++i)
                ok = proj(c, pts[i], &q[i * 2], &q[i * 2 + 1]);
            if (ok && inside(q, x, y))
                TRY(H_MYZ + (i32)k, 0.0, 1.0, -2.0);
        }
    }
    for (i32 h = H_RX; h <= H_RV; ++h) {
        if (!shown(L, h))
            continue;
        v3 u, v;
        f64 r;
        ring_of(L, h, &u, &v, &r);
        enum { N = 72 };
        f64 px0 = 0, py0 = 0;
        bool prev = false;
        for (u32 i = 0; i <= N; ++i) {
            const f64 a = 2 * DE_PI * i / N;
            const v3 off = add(mul(u, cos(a) * r), mul(v, sin(a) * r));
            f64 px, py;
            /* The back half of an axis ring is behind the gizmo: not grabbed. */
            const bool front = h == H_RV || dot(off, L->dir) <= 1e-9;
            const bool vis = front && proj(c, add(L->o, off), &px, &py);
            if (vis && prev)
                TRY(h, dist_seg(x, y, px0, py0, px, py), 7.0, 1.0);
            prev = vis;
            if (vis) {
                px0 = px;
                py0 = py;
            }
        }
    }
#undef TRY
    return best;
}

/* ---------- The drag ---------- */

static f64 snap_to(f64 v, f64 step) { return step > 0 ? floor(v / step + 0.5) * step : v; }

static f64 step_of(u32 k) {
    if (G.snap[k] > 0)
        return G.snap[k];
    return G.ctrl ? k_default_steps[k] : 0;
}

static bool typed(f64 *v) {
    if (G.typed[0] == '\0')
        return false;
    char *end = NULL;
    const f64 x = strtod(G.typed, &end);
    if (end == G.typed)
        return false;
    *v = x;
    return true;
}

static v3 plane_normal(const gz_layout *L, i32 h) {
    if (h >= H_MYZ && h <= H_MXY)
        return L->ax[h - H_MYZ];
    if (h >= H_RX && h <= H_RZ)
        return L->ax[h - H_RX];
    if (h >= H_RIN)
        return nrm(cross(L->ru, L->rv));
    return L->dir; /* H_MC, H_RV */
}

/* ---------- Surface placement (Shift) and vertex snapping (V) ---------- */

static bool dragged(u64 e) {
    for (u32 i = 0; i < G.n; ++i)
        if (G.ents[i].e == e)
            return true;
    return false;
}

typedef struct surf_scan {
    v3 ro, rd;
    f64 best;
    v3 n;
} surf_scan;

/* A vertex of a kept mesh, in world space. */
static v3 geo_vertex(const de__ed_mesh_geo *g, u32 i) {
    const f32 *p = g->positions + 3u * (u64)i;
    const f32 *m = g->model;
    return V(g->origin.x + (f64)m[0] * p[0] + (f64)m[3] * p[1] + (f64)m[6] * p[2] + m[9],
             g->origin.y + (f64)m[1] * p[0] + (f64)m[4] * p[1] + (f64)m[7] * p[2] + m[10],
             g->origin.z + (f64)m[2] * p[0] + (f64)m[5] * p[1] + (f64)m[8] * p[2] + m[11]);
}

/* The ray against a mesh's triangles (both sides, Moller-Trumbore): the nearest hit and its
 * face's normal, turned to face the ray. */
static void surf_mesh(surf_scan *s, const de__ed_mesh_geo *g) {
    for (u32 t = 0; t < g->triangle_count; ++t) {
        const u32 *k = g->triangles + 3u * (u64)t;
        if (k[0] >= g->vertex_count || k[1] >= g->vertex_count || k[2] >= g->vertex_count)
            continue;
        const v3 a = geo_vertex(g, k[0]);
        const v3 e1 = sub(geo_vertex(g, k[1]), a), e2 = sub(geo_vertex(g, k[2]), a);
        const v3 pv = cross(s->rd, e2);
        const f64 det = dot(e1, pv);
        if (fabs(det) < 1e-18)
            continue;
        const f64 inv = 1.0 / det;
        const v3 tv = sub(s->ro, a);
        const f64 u = dot(tv, pv) * inv;
        if (u < 0 || u > 1)
            continue;
        const v3 qv = cross(tv, e1);
        const f64 v = dot(s->rd, qv) * inv;
        if (v < 0 || u + v > 1)
            continue;
        const f64 d = dot(e2, qv) * inv;
        if (d < 0 || d >= s->best)
            continue;
        const v3 n = nrm(cross(e1, e2));
        s->best = d;
        s->n = dot(n, s->rd) > 0 ? mul(n, -1.0) : n;
    }
}

/* The ray against a mesh: its triangles when the renderer kept them, else its bounds (slabs): the
 * entry distance and the face it enters through. */
static void surf_box(u64 e, de_dvec3 lo, de_dvec3 hi, const de__ed_mesh_geo *geo, void *user) {
    surf_scan *s = (surf_scan *)user;
    if (dragged(e))
        return;
    f64 t0 = -1e30, t1 = 1e30;
    u32 axis = 0;
    f64 sign = 1;
    for (u32 k = 0; k < 3; ++k) {
        const f64 o = comp(s->ro, k), d = comp(s->rd, k), l = comp(lo, k), h = comp(hi, k);
        if (fabs(d) < 1e-12) {
            if (o < l || o > h)
                return;
            continue;
        }
        f64 a = (l - o) / d, b = (h - o) / d;
        f64 face = -1;
        if (a > b) {
            const f64 t = a;
            a = b;
            b = t;
            face = 1;
        }
        if (a > t0) {
            t0 = a;
            axis = k;
            sign = face;
        }
        if (b < t1)
            t1 = b;
    }
    if (t1 < 0 || t0 > t1 || t0 >= s->best)
        return;
    if (geo != NULL) {
        surf_mesh(s, geo);
        return;
    }
    if (t0 < 0)
        return;
    s->best = t0;
    s->n = mul(unit(axis), sign);
}

/* Where the pointer's ray meets the scene, besides the dragged entities: a mesh (its triangles,
 * or its bounds), a sprite, else the grid's plane. */
static bool surface_hit(de__editor *ed, const de__ed_cam *c, f64 x, f64 y, v3 *point, v3 *normal) {
    surf_scan s;
    ray(c, x, y, &s.ro, &s.rd);
    s.best = 1e30;
    s.n = V(0, 0, 1);
    de__ed_each_bounds(ed, surf_box, &s);
    u64 sprites[16];
    const u32 ns = de__ed_sprite_pick(ed, c, (f32)x, (f32)y, sprites, 16);
    for (u32 i = 0; i < ns; ++i) {
        gz_ent g;
        if (dragged(sprites[i]) || !ent_of(ed->e->world, sprites[i], &g))
            continue;
        gz_layout L;
        memset(&L, 0, sizeof L);
        rect_of(ed->e->world, c, &g, &L);
        const v3 n = nrm(cross(L.ru, L.rv));
        v3 hit;
        if (ray_plane(s.ro, s.rd, L.rp, n, &hit)) {
            const f64 t = dot(sub(hit, s.ro), s.rd);
            if (t >= 0 && t < s.best) {
                s.best = t;
                s.n = dot(n, s.rd) > 0 ? mul(n, -1.0) : n;
            }
        }
        break; /* the nearest sprite */
    }
    if (s.best < 1e29) {
        *point = add(s.ro, mul(s.rd, s.best));
        *normal = s.n;
        return true;
    }
    f64 p[3];
    f32 nn[3];
    if (!de__ed_view_point(ed, c, (f32)x, (f32)y, 0, p, nn))
        return false;
    *point = V(p[0], p[1], p[2]);
    *normal = V(nn[0], nn[1], nn[2]);
    return true;
}

typedef struct snap_scan {
    const de__ed_cam *c;
    f64 x, y, best;
    v3 at;
} snap_scan;

static void snap_point(snap_scan *s, v3 p) {
    f64 px, py;
    if (!proj(s->c, p, &px, &py))
        return;
    const f64 d = hypot(px - s->x, py - s->y);
    if (d < s->best) {
        s->best = d;
        s->at = p;
    }
}

/* Meshes with more vertices than this snap to their bounds' corners (projecting them all on
 * every move would be slow). */
#define SNAP_MAX_VERTICES 65536u

static void snap_box(u64 e, de_dvec3 lo, de_dvec3 hi, const de__ed_mesh_geo *geo, void *user) {
    if (dragged(e))
        return;
    if (geo != NULL && geo->vertex_count <= SNAP_MAX_VERTICES) {
        for (u32 i = 0; i < geo->vertex_count; ++i)
            snap_point((snap_scan *)user, geo_vertex(geo, i));
        return;
    }
    for (u32 k = 0; k < 8; ++k)
        snap_point((snap_scan *)user, V(k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z));
}

static void snap_origin_chunk(const de_query_chunk *ch, void *user) {
    const DeGlobalTransform *ts = (const DeGlobalTransform *)ch->columns[0];
    for (u32 r = 0; r < ch->count; ++r)
        if (!dragged(ch->entities[r].bits) && !de__ed_views_owns(ch->entities[r].bits))
            snap_point((snap_scan *)user, ts[r].position);
}

/* The point nearest the pointer (within reach on screen) that vertex snapping reaches: mesh
 * vertices (the corners of the bounds of meshes without kept triangles) and entities' origins. */
static bool vertex_near(de__editor *ed, const de__ed_cam *c, f64 x, f64 y, v3 *out) {
    snap_scan s = {c, x, y, 24.0, V(0, 0, 0)};
    de__ed_each_bounds(ed, snap_box, &s);
    de_world *w = ed->e->world;
    const de_builtin_component_ids b = de_builtin_components(w);
    if (b.global_transform.id != 0) {
        const de_query_term terms[1] = {{b.global_transform, DE_READ}};
        const de_query_desc qd = {terms, 1, NULL, 0};
        const de_query q = de_query_create(w, &qd);
        de_query_each(w, q, snap_origin_chunk, &s);
        de_query_destroy(w, q);
    }
    if (s.best >= 24.0)
        return false;
    *out = s.at;
    return true;
}

/* The rotation that turns +Z onto `n`. */
static de_quat onto_normal(v3 n) {
    n = nrm(n);
    if (n.z > 0.999999)
        return (de_quat){0, 0, 0, 1};
    if (n.z < -0.999999)
        return (de_quat){1, 0, 0, 0};
    return qaxis(cross(V(0, 0, 1), n), acos(n.z));
}

/* Sets up the drag's reference (where the press grabbed the handle). */
static void begin_ref(const de__ed_cam *c) {
    const gz_layout *L = &G.L;
    const i32 h = G.h;
    v3 ro, rd;
    ray(c, G.x0, G.y0, &ro, &rd);
    G.t0 = 0;
    G.hit0 = L->o;
    G.ang = G.ang_prev = 0;
    G.ang_screen = false;
    if ((h >= H_MX && h <= H_MZ) || (h >= H_SX && h <= H_SZ)) {
        const v3 a = L->ax[h >= H_SX ? h - H_SX : h - H_MX];
        if (!ray_axis(ro, rd, L->o, a, &G.t0))
            G.t0 = 0;
    } else if (h >= H_RX && h <= H_RV) {
        const v3 n = plane_normal(L, h);
        G.ang_screen = fabs(dot(rd, n)) < 0.08 || !ray_plane(ro, rd, L->o, n, &G.hit0);
    } else if (h != H_SC) {
        const v3 o = h >= H_RIN ? L->rp : L->o;
        if (!ray_plane(ro, rd, o, plane_normal(L, h), &G.hit0))
            G.hit0 = o;
    }
}

/* The world transform `w` of an entity as its local transform. */
static de_transform local_of(const gz_ent *g, const de_transform *w, de_vec3 local_scale) {
    de_transform t = *w;
    if (g->child) {
        const de_transform inv = de_transform_inverse(&g->parent);
        t.position = de_transform_point(&inv, w->position);
        t.rotation = de_quat_normalize(qmul(qconj(g->parent.rotation), w->rotation));
    }
    t.scale = local_scale;
    return t;
}

/* The new local transforms for the pointer at (G.x, G.y). */
static void solve(const de__ed_cam *c, de_transform *out) {
    const gz_layout *L = &G.L;
    const i32 h = G.h;
    v3 ro, rd;
    ray(c, G.x, G.y, &ro, &rd);
    v3 move = V(0, 0, 0);
    de_quat turn = {0, 0, 0, 1};
    de_quat surface_turn = {0, 0, 0, 1}; /* Shift with surfaceAlign: onto the surface's normal */
    f64 factor = 1.0;
    i32 scale_axis = -1; /* -1: uniform */
    bool rect_resize = false;
    f64 r[4];
    memcpy(r, L->r, sizeof r);

    if ((h >= H_MX && h <= H_MZ) || (h >= H_MYZ && h <= H_MC) || h == H_RIN) {
        /* The axes the drag moves along: the handle's axis, its plane's two, or (the centre, the
         * rect) the frame's in-plane ones. */
        v3 axes[3];
        u32 na = 0;
        if (h >= H_MX && h <= H_MZ) {
            f64 t = G.t0;
            (void)ray_axis(ro, rd, L->o, L->ax[h - H_MX], &t);
            move = mul(L->ax[h - H_MX], t - G.t0);
            axes[na++] = L->ax[h - H_MX];
        } else {
            v3 hit;
            const v3 o = h == H_RIN ? L->rp : L->o;
            const v3 n = plane_normal(L, h);
            if (ray_plane(ro, rd, o, n, &hit))
                move = sub(hit, G.hit0);
            if (h == H_RIN) {
                axes[na++] = L->ru;
                axes[na++] = L->rv;
            } else {
                for (u32 k = 0; k < 3; ++k)
                    if (fabs(dot(L->ax[k], n)) < 0.5)
                        axes[na++] = L->ax[k];
            }
        }
        f64 exact = 0;
        const f64 st = step_of(0);
        if (na == 1 && typed(&exact)) {
            move = mul(axes[0], exact);
        } else if (st > 0 && G.n > 0) {
            /* Grid snapping: the first entity's pivot lands on the grid, the rest keep their
             * offsets (as Unity and Godot snap). */
            const v3 to = add(G.ents[0].world0.position, move);
            for (u32 k = 0; k < na; ++k) {
                const f64 at = dot(to, axes[k]);
                move = add(move, mul(axes[k], snap_to(at, st) - at));
            }
        }
        /* Shift: the first entity onto the surface under the pointer (the rest keep their
         * offsets), turned to its normal with surfaceAlign. V: onto the nearest point. */
        v3 hit, n;
        if (G.shift && G.ed != NULL && G.n > 0 && surface_hit(G.ed, c, G.x, G.y, &hit, &n)) {
            move = sub(hit, G.ents[0].world0.position);
            if (G.align)
                surface_turn = onto_normal(n);
        }
        if (G.vkey && G.ed != NULL && G.n > 0 && vertex_near(G.ed, c, G.x, G.y, &hit))
            move = sub(hit, G.ents[0].world0.position);
        /* In 2D, along the drag's axes only: an entity keeps its depth (its draw order). */
        if ((G.shift || G.vkey) && de__ed_cam_flat(c)) {
            v3 kept = V(0, 0, 0);
            for (u32 k = 0; k < na; ++k)
                kept = add(kept, mul(axes[k], dot(move, axes[k])));
            move = kept;
        }
    } else if (h >= H_RX && h <= H_RV) {
        const v3 n = plane_normal(L, h);
        v3 hit;
        if (!G.ang_screen && ray_plane(ro, rd, L->o, n, &hit)) {
            const v3 a0 = sub(G.hit0, L->o), a1 = sub(hit, L->o);
            const f64 now = atan2(dot(n, cross(a0, a1)), dot(a0, a1));
            f64 d = now - G.ang_prev;
            while (d > DE_PI)
                d -= 2 * DE_PI;
            while (d < -DE_PI)
                d += 2 * DE_PI;
            G.ang += d;
            G.ang_prev = now;
        } else {
            G.ang = (G.x - G.x0) * 0.01;
        }
        f64 deg = snap_to(G.ang * 180.0 / DE_PI, step_of(1));
        (void)typed(&deg);
        turn = qaxis(n, deg * DE_PI / 180.0);
    } else if (h >= H_SX && h <= H_SZ) {
        scale_axis = h - H_SX;
        f64 t = G.t0;
        (void)ray_axis(ro, rd, L->o, L->ax[scale_axis], &t);
        const f64 t0 = fabs(G.t0) > L->size * 0.05 ? G.t0 : (G.t0 < 0 ? -1 : 1) * L->size * 0.05;
        factor = t / t0;
    } else if (h == H_SC) {
        const f64 px = c->h * 0.12 < 64 ? 64 : c->h * 0.12;
        factor = 1.0 + ((G.x - G.x0) - (G.y - G.y0)) / px;
    } else if (h > H_RIN) {
        rect_resize = true;
        v3 hit;
        if (ray_plane(ro, rd, L->rp, plane_normal(L, h), &hit)) {
            const v3 d = sub(hit, L->rp);
            const f64 st = step_of(0);
            const f64 pu = snap_to(dot(d, L->ru), st), pv = snap_to(dot(d, L->rv), st);
            const f64 min = 0.01;
            if (h == H_RL || h == H_RLB || h == H_RLT)
                r[0] = fmin(pu, r[1] - min);
            if (h == H_RR || h == H_RRB || h == H_RRT)
                r[1] = fmax(pu, r[0] + min);
            if (h == H_RB || h == H_RLB || h == H_RRB)
                r[2] = fmin(pv, r[3] - min);
            if (h == H_RT || h == H_RLT || h == H_RRT)
                r[3] = fmax(pv, r[2] + min);
        }
    }
    if (h >= H_SX && h <= H_SC) {
        factor = 1.0 + snap_to(factor - 1.0, step_of(2));
        (void)typed(&factor);
        if (fabs(factor) < 1e-3)
            factor = factor < 0 ? -1e-3 : 1e-3;
    }

    const bool about_center = strcmp(G.pivot, "center") == 0;
    for (u32 i = 0; i < G.n; ++i) {
        const gz_ent *g = &G.ents[i];
        de_transform w = g->world0;
        de_vec3 ls = g->local0.scale;
        w.position = add(w.position, move);
        if (surface_turn.w < 1.0f)
            w.rotation = de_quat_normalize(qmul(surface_turn, g->world0.rotation));
        if (h >= H_RX && h <= H_RV) {
            w.rotation = de_quat_normalize(qmul(turn, g->world0.rotation));
            if (about_center)
                w.position = add(L->o, qrot(turn, sub(g->world0.position, L->o)));
        } else if (h >= H_SX && h <= H_SC) {
            if (scale_axis < 0) {
                ls = (de_vec3){(f32)(ls.x * factor), (f32)(ls.y * factor), (f32)(ls.z * factor)};
                if (about_center)
                    w.position = add(L->o, mul(sub(g->world0.position, L->o), factor));
            } else {
                /* Along the first entity's local axis: each entity's matching local axis. */
                if (scale_axis == 0)
                    ls.x = (f32)(ls.x * factor);
                else if (scale_axis == 1)
                    ls.y = (f32)(ls.y * factor);
                else
                    ls.z = (f32)(ls.z * factor);
                if (about_center) {
                    const v3 a = L->ax[scale_axis], rel = sub(g->world0.position, L->o);
                    w.position = add(g->world0.position, mul(a, dot(rel, a) * (factor - 1.0)));
                }
            }
        } else if (rect_resize) {
            /* The opposite side stays: scale by the new size, move so it does. */
            const f64 w0 = L->r[1] - L->r[0], h0 = L->r[3] - L->r[2];
            const f64 fu = (r[1] - r[0]) / w0, fv = (r[3] - r[2]) / h0;
            const v3 lz = qrot(g->world0.rotation, V(0, 0, 1));
            const bool side = fabs(dot(lz, L->rv)) > 0.5;
            ls.x = (f32)(ls.x * fu);
            if (side)
                ls.z = (f32)(ls.z * fv);
            else
                ls.y = (f32)(ls.y * fv);
            if (i == 0) {
                const f64 du = r[0] + L->pivot.x * (r[1] - r[0]), dv = r[2] + L->pivot.y * (r[3] - r[2]);
                w.position = add(L->rp, add(mul(L->ru, du), mul(L->rv, dv)));
            }
        }
        out[i] = local_of(g, &w, ls);
    }
}

static void set_local(de_world *w, const de_transform *ts) {
    const de_builtin_component_ids b = de_builtin_components(w);
    for (u32 i = 0; i < G.n; ++i) {
        DeTransform *t = (DeTransform *)de_entity_get(w, (de_entity){G.ents[i].e}, b.transform);
        if (t != NULL)
            *t = ts[i];
    }
}

/* A transform as a "DeTransform" component value, raw. */
static de_ep_raw transform_raw(de__editor *ed, de_ep_arena *a, const de_transform *t) {
    const de_type_id type = de_type_find(ed->e->types, "DeTransform");
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    de__ed_write_object(ed, &mw, type, t);
    return de__ed_raw_end(a, &mw);
}

static void send_update(de__editor *ed, const de_transform *now) {
    de_ep_arena a = {ed->arena, ed->arena_cap, 0};
    de_ep_gizmo_drag_update d;
    memset(&d, 0, sizeof d);
    u64 ids[GZ_MAX];
    for (u32 i = 0; i < G.n; ++i)
        ids[i] = G.ents[i].e;
    d.entities.items = ids;
    d.entities.count = G.n;
    const de_type_id type = de_type_find(ed->e->types, "DeTransform");
    u32 fc = 0;
    const de_field_info *fs = de_type_fields(ed->e->types, type, &fc);
    d.preview.items = (de_ep_field_change *)de__ed_alloc(&a, sizeof(de_ep_field_change) * G.n);
    for (u32 i = 0; i < G.n && d.preview.items != NULL; ++i) {
        de_ep_field_change *c = &d.preview.items[d.preview.count++];
        c->entity = G.ents[i].e;
        c->component = de_ep_cstr("DeTransform");
        c->fields.items = (de_ep_kv_raw *)de__ed_alloc(&a, sizeof(de_ep_kv_raw) * 3);
        c->fields.count = 0;
        for (u32 f = 0; f < fc && f < 3 && c->fields.items != NULL; ++f) {
            mpack_writer_t mw;
            de__ed_raw_begin(&a, &mw);
            de__ed_write_field(ed, &mw, type, f, &fs[f], (const u8 *)&now[i] + fs[f].offset);
            c->fields.items[c->fields.count].key = de_ep_cstr(fs[f].name);
            c->fields.items[c->fields.count++].value = de__ed_raw_end(&a, &mw);
        }
    }
    mpack_writer_t nw;
    const u64 seq = de__ed_note_begin(ed, &nw);
    de_ep_write_notify_gizmo_drag_update(&nw, seq, &d);
    de__ed_send(ed, &nw, DE_EP_KIND_NOTIFICATION);
}

/* Ends the drag: the entities go back to where they started and `now` (NULL: cancelled, or a
 * press that never moved) goes to the IDE as the ops to commit. */
static void finish(de__editor *ed, const de_transform *now) {
    de_transform start[GZ_MAX];
    for (u32 i = 0; i < G.n; ++i)
        start[i] = G.ents[i].local0;
    set_local(ed->e->world, start);
    G.drag = false;
    de_ep_arena a = {ed->arena, ed->arena_cap, 0};
    de_ep_gizmo_drag_end d;
    memset(&d, 0, sizeof d);
    if (now != NULL) {
        d.ops.items = (de_ep_edit_op *)de__ed_alloc(&a, sizeof(de_ep_edit_op) * G.n);
        for (u32 i = 0; i < G.n && d.ops.items != NULL; ++i) {
            de_ep_edit_op *o = &d.ops.items[d.ops.count++];
            memset(o, 0, sizeof *o);
            o->op = de_ep_cstr("set");
            o->has_entity = true;
            o->entity = G.ents[i].e;
            o->has_path = true;
            o->path = de_ep_cstr("DeTransform");
            o->has_value = true;
            o->value = transform_raw(ed, &a, &now[i]);
        }
    }
    mpack_writer_t nw;
    const u64 seq = de__ed_note_begin(ed, &nw);
    de_ep_write_notify_gizmo_drag_end(&nw, seq, &d);
    de__ed_send(ed, &nw, DE_EP_KIND_NOTIFICATION);
}

static bool has_mod(const de_ep_input_event *e, const char *m) {
    if (!e->has_modifiers)
        return false;
    for (u32 i = 0; i < e->modifiers.count; ++i)
        if (de_ep_str_eq(e->modifiers.items[i], m))
            return true;
    return false;
}

static void drag_to(de__editor *ed, const de__ed_cam *c) {
    de_transform now[GZ_MAX];
    solve(c, now);
    set_local(ed->e->world, now);
    send_update(ed, now);
}

void de__ed_gizmo_input(de__editor *ed, u32 view, const de__ed_cam *c, const de_ep_input_event *e, f32 x,
                        f32 y) {
    de_world *w = ed->e->world;
    G.ed = ed;
    if (de_ep_str_eq(e->type, "key")) {
        /* V, down or up, whether or not a drag is on. */
        if (e->has_code && de_ep_str_eq(e->code, "KeyV")) {
            G.vkey = !(e->has_action && de_ep_str_eq(e->action, "up"));
            if (G.drag && G.moved && G.view == view)
                drag_to(ed, c);
            return;
        }
        if (!G.drag || !e->has_key || (e->has_action && de_ep_str_eq(e->action, "up")))
            return;
        const de_ep_str k = e->key;
        const size_t n = strlen(G.typed);
        if (de_ep_str_eq(k, "Backspace")) {
            if (n > 0)
                G.typed[n - 1] = '\0';
        } else if (de_ep_str_eq(k, "Escape")) {
            finish(ed, NULL);
            return;
        } else if (k.len == 1 && ((k.ptr[0] >= '0' && k.ptr[0] <= '9') || k.ptr[0] == '.' || k.ptr[0] == '-') &&
                   n + 1 < sizeof G.typed) {
            G.typed[n] = k.ptr[0];
            G.typed[n + 1] = '\0';
        } else {
            return;
        }
        if (G.view == view) {
            G.moved = true;
            drag_to(ed, c);
        }
        return;
    }
    if (!de_ep_str_eq(e->type, "pointer") || !e->has_action)
        return;
    const de_ep_str act = e->action;
    if (!G.drag) {
        if (!tool_drags() || G.sel_count == 0) {
            G.hover = H_NONE;
            return;
        }
        const gz_layout L = layout(w, c);
        const i32 h = hit_test(c, &L, x, y);
        if (de_ep_str_eq(act, "move")) {
            G.hover = h;
            G.hover_view = view;
            return;
        }
        if (!de_ep_str_eq(act, "down") || (e->has_button && e->button != 0) || h == H_NONE)
            return;
        G.n = 0;
        for (u32 i = 0; i < G.sel_count && G.n < GZ_MAX; ++i)
            if (ent_of(w, G.sel[i], &G.ents[G.n]))
                ++G.n;
        if (G.n == 0)
            return;
        G.drag = true;
        G.moved = false;
        G.view = view;
        G.h = h;
        G.L = L;
        G.x0 = G.x = x;
        G.y0 = G.y = y;
        G.typed[0] = '\0';
        G.ctrl = has_mod(e, "ctrl") || has_mod(e, "meta");
        G.shift = has_mod(e, "shift");
        begin_ref(c);
        return;
    }
    if (G.view != view)
        return;
    G.x = x;
    G.y = y;
    G.ctrl = has_mod(e, "ctrl") || has_mod(e, "meta");
    G.shift = has_mod(e, "shift");
    if (de_ep_str_eq(act, "move")) {
        if (!G.moved && fabs(x - G.x0) + fabs(y - G.y0) < 2.0f)
            return;
        G.moved = true;
        drag_to(ed, c);
    } else if (de_ep_str_eq(act, "up")) {
        if (!G.moved) {
            finish(ed, NULL);
            return;
        }
        de_transform now[GZ_MAX];
        solve(c, now);
        finish(ed, now);
    } else if (de_ep_str_eq(act, "cancel")) {
        finish(ed, NULL);
    }
}

bool de__ed_view_point(de__editor *ed, const de__ed_cam *c, f32 x, f32 y, u64 entity, f64 point[3],
                       f32 normal[3]) {
    v3 ro, rd;
    ray(c, x, y, &ro, &rd);
    const v3 f = cam_fwd(c);
    v3 o = V(0, 0, 0), n = V(0, 0, 1);
    u32 k = 0;
    for (u32 i = 1; i < 3; ++i)
        if (fabs(comp(f, i)) > fabs(comp(f, k)))
            k = i;
    /* A 2D view places on its plane through the origin (the depth a 2D scene draws at), not at
     * the depth of whatever is under the cursor (a background). */
    const bool flat = ortho(c) && fabs(comp(f, k)) > 0.995;
    gz_ent g;
    if (flat) {
        n = unit(k);
    } else if (entity != 0 && ed->e->world != NULL && ent_of(ed->e->world, entity, &g)) {
        o = g.world0.position;
        n = f;
        if (sprite_of(ed->e->world, entity) != NULL) {
            gz_layout L;
            memset(&L, 0, sizeof L);
            rect_of(ed->e->world, c, &g, &L);
            n = nrm(cross(L.ru, L.rv));
        }
    }
    v3 hit;
    if (!ray_plane(ro, rd, o, n, &hit))
        return false;
    if (dot(n, f) > 0)
        n = mul(n, -1.0);
    point[0] = hit.x;
    point[1] = hit.y;
    point[2] = hit.z;
    normal[0] = (f32)n.x;
    normal[1] = (f32)n.y;
    normal[2] = (f32)n.z;
    return true;
}

typedef struct origin_scan {
    const de__ed_cam *c;
    f64 x0, y0, x1, y1;
    const u64 *skip;
    u32 skip_count;
    u64 *out;
    u32 n, cap;
} origin_scan;

static void origin_chunk(const de_query_chunk *ch, void *user) {
    origin_scan *s = (origin_scan *)user;
    const DeGlobalTransform *ts = (const DeGlobalTransform *)ch->columns[0];
    for (u32 r = 0; r < ch->count && s->n < s->cap; ++r) {
        f64 x, y;
        if (!proj(s->c, ts[r].position, &x, &y) || x < s->x0 || x > s->x1 || y < s->y0 || y > s->y1)
            continue;
        bool skip = false;
        for (u32 k = 0; k < s->skip_count && !skip; ++k)
            skip = s->skip[k] == ch->entities[r].bits;
        if (!skip)
            s->out[s->n++] = ch->entities[r].bits;
    }
}

u32 de__ed_origin_pick(de__editor *ed, const de__ed_cam *c, f32 x0, f32 y0, f32 x1, f32 y1,
                       const u64 *skip, u32 skip_count, u64 *out, u32 cap) {
    de_world *w = ed->e->world;
    const de_builtin_component_ids b = w != NULL ? de_builtin_components(w) : (de_builtin_component_ids){0};
    if (w == NULL || b.global_transform.id == 0 || cap == 0)
        return 0;
    origin_scan s = {c, fmin(x0, x1), fmin(y0, y1), fmax(x0, x1), fmax(y0, y1), skip, skip_count, out, 0, cap};
    const de_query_term terms[1] = {{b.global_transform, DE_READ}};
    const de_query_desc qd = {terms, 1, NULL, 0};
    const de_query q = de_query_create(w, &qd);
    de_query_each(w, q, origin_chunk, &s);
    de_query_destroy(w, q);
    return s.n;
}

bool de__ed_cam_flat(const de__ed_cam *c) {
    const v3 f = cam_fwd(c);
    return ortho(c) && (fabs(f.x) > 0.995 || fabs(f.y) > 0.995 || fabs(f.z) > 0.995);
}

typedef struct sprite_hit {
    u64 e;
    f64 depth;
} sprite_hit;

typedef struct sprite_scan {
    const de__ed_cam *c;
    v3 ro, rd;
    sprite_hit *hits;
    u32 n, cap;
} sprite_scan;

static void sprite_chunk(const de_query_chunk *ch, void *user) {
    sprite_scan *s = (sprite_scan *)user;
    const DeSprite *sp = (const DeSprite *)ch->columns[0];
    const DeGlobalTransform *ts = (const DeGlobalTransform *)ch->columns[1];
    for (u32 r = 0; r < ch->count && s->n < s->cap; ++r) {
        gz_ent g;
        memset(&g, 0, sizeof g);
        g.e = ch->entities[r].bits;
        g.world0 = ts[r];
        /* rect_of reads the sprite from the world; the chunk has it, so lay it out here. */
        const de_transform *t = &ts[r];
        const v3 f = cam_fwd(s->c);
        const v3 lz = qrot(t->rotation, V(0, 0, 1)), ly = qrot(t->rotation, V(0, 1, 0));
        const bool side = fabs(dot(lz, f)) <= fabs(dot(ly, f));
        const v3 u = qrot(t->rotation, V(1, 0, 0)), v = side ? lz : ly;
        const f64 w0 = sp[r].size.x * fabs(t->scale.x), h0 = sp[r].size.y * fabs(side ? t->scale.z : t->scale.y);
        const f64 u0 = -sp[r].pivot.x * w0, v0 = -sp[r].pivot.y * h0;
        v3 hit;
        if (!ray_plane(s->ro, s->rd, t->position, nrm(cross(u, v)), &hit))
            continue;
        const v3 d = sub(hit, t->position);
        const f64 pu = dot(d, u), pv = dot(d, v);
        if (pu < u0 || pu > u0 + w0 || pv < v0 || pv > v0 + h0)
            continue;
        s->hits[s->n++] = (sprite_hit){g.e, dot(sub(hit, s->ro), s->rd)};
    }
}

static int cmp_hit(const void *a, const void *b) {
    const f64 x = ((const sprite_hit *)a)->depth, y = ((const sprite_hit *)b)->depth;
    return x < y ? -1 : x > y;
}

u32 de__ed_sprite_pick(de__editor *ed, const de__ed_cam *c, f32 x, f32 y, u64 *out, u32 cap) {
    de_world *w = ed->e->world;
    if (w == NULL || cap == 0)
        return 0;
    const de_component comp = de_component_find(w, "DeSprite");
    const de_builtin_component_ids b = de_builtin_components(w);
    if (comp.id == 0 || b.global_transform.id == 0)
        return 0;
    sprite_hit hits[256];
    sprite_scan s = {c, V(0, 0, 0), V(0, 0, 0), hits, 0, 256};
    ray(c, x, y, &s.ro, &s.rd);
    const de_query_term terms[2] = {{comp, DE_READ}, {b.global_transform, DE_READ}};
    const de_query_desc qd = {terms, 2, NULL, 0};
    const de_query q = de_query_create(w, &qd);
    de_query_each(w, q, sprite_chunk, &s);
    de_query_destroy(w, q);
    qsort(hits, s.n, sizeof hits[0], cmp_hit);
    const u32 n = s.n < cap ? s.n : cap;
    for (u32 i = 0; i < n; ++i)
        out[i] = hits[i].e;
    return n;
}

void de__ed_gizmo_reset(de__editor *ed) {
    if (G.drag && ed->e->world != NULL) {
        de_transform start[GZ_MAX];
        for (u32 i = 0; i < G.n; ++i)
            start[i] = G.ents[i].local0;
        set_local(ed->e->world, start);
    }
    G.drag = false;
    G.hover = H_NONE;
    G.sel_count = 0;
}

/* ---------- Drawing the overlays ---------- */

static rgb grey(u8 v) { return (rgb){v, v, v}; }

static void draw_grid(const de__ed_cam *c, const de__ed_canvas *cv) {
    const v3 f = cam_fwd(c), p = cam_pos(c);
    u32 k = 0;
    for (u32 i = 1; i < 3; ++i)
        if (fabs(comp(f, i)) > fabs(comp(f, k)))
            k = i;
    /* A 2D (orthographic, axis-aligned) view: the plane it looks at. Otherwise the ground. */
    const bool flat = ortho(c) && fabs(comp(f, k)) > 0.995;
    if (!flat) {
        k = 2;
        if (fabs(f.z) < 0.02 && ortho(c))
            return;
    }
    const u32 i = (k + 1) % 3, j = (k + 2) % 3;
    const f64 plane = flat ? comp(p, k) + comp(f, k) * (cam_near(c) + 1.0) : 0.0;
    const f64 dist = flat ? 1.0 : fmax(1.0, fabs(comp(p, k) - plane));
    const f64 px_m = ppm(c, dist);
    f64 step = pow(10.0, ceil(log10(10.0 / px_m)));
    if (step < 1e-3)
        step = 1e-3;
    const f64 half = flat ? fmax(half_h(c) * aspect(c), half_h(c)) * 1.1 + step : step * 150.0;
    const f64 ci = comp(p, i), cj = comp(p, j);
    const i64 a0 = (i64)floor((ci - half) / step), a1 = (i64)ceil((ci + half) / step);
    const i64 b0 = (i64)floor((cj - half) / step), b1 = (i64)ceil((cj + half) / step);
    /* Minor lines fade in as they spread out. */
    const f32 minor = (f32)fmin(1.0, (step * px_m - 10.0) / 30.0 + 0.35) * 0.08f;
    for (u32 axis = 0; axis < 2; ++axis) {
        const u32 along = axis == 0 ? j : i, across = axis == 0 ? i : j;
        const i64 lo = axis == 0 ? a0 : b0, hi = axis == 0 ? a1 : b1;
        const f64 from = axis == 0 ? cj - half : ci - half, to = axis == 0 ? cj + half : ci + half;
        for (i64 n = lo; n <= hi; ++n) {
            f64 pa[3], pb[3];
            pa[k] = pb[k] = plane;
            pa[across] = pb[across] = (f64)n * step;
            pa[along] = from;
            pb[along] = to;
            const v3 A = V(pa[0], pa[1], pa[2]), B = V(pb[0], pb[1], pb[2]);
            if (n == 0)
                wseg(c, cv, A, B, G.col[along == 0 ? C_X : along == 1 ? C_Y : C_Z], 0.55f, 1);
            else if (n % 10 == 0)
                wseg(c, cv, A, B, grey(200), 0.17f, 1);
            else if (minor > 0.01f)
                wseg(c, cv, A, B, grey(200), minor, 1);
        }
    }
}

/* The frames of the scene's cameras (what each sees), as Unity and Godot draw them. */
typedef struct cam_draw {
    const de__ed_cam *c;
    const de__ed_canvas *cv;
    const u64 *skip;
    u32 skip_count;
    f64 aspect;
} cam_draw;

/* A light line with a dark halo: readable on a bright sky and on a dark scene. */
static void frame_seg(const cam_draw *d, v3 a, v3 b) {
    wseg(d->c, d->cv, a, b, grey(20), 0.45f, 3);
    wseg(d->c, d->cv, a, b, grey(245), 0.95f, 1);
}

static void cam_chunk(const de_query_chunk *ch, void *user) {
    const cam_draw *d = (const cam_draw *)user;
    const DeCamera *cams = (const DeCamera *)ch->columns[0];
    const DeGlobalTransform *ts = (const DeGlobalTransform *)ch->columns[1];
    for (u32 r = 0; r < ch->count; ++r) {
        bool skip = false;
        for (u32 s = 0; s < d->skip_count && !skip; ++s)
            skip = d->skip[s] == ch->entities[r].bits;
        if (skip)
            continue;
        const DeCamera *cam = &cams[r];
        const de_transform *t = &ts[r];
        /* A DeCamera looks down its local -Z with +Y up. */
        const v3 fw = qrot(t->rotation, V(0, 0, -1)), up = qrot(t->rotation, V(0, 1, 0)), rt = qrot(t->rotation, V(1, 0, 0));
        if (cam->projection == DE_PROJ_ORTHOGRAPHIC) {
            const f64 hh = cam->ortho_height * 0.5, hw = hh * d->aspect;
            v3 p[4];
            for (u32 i = 0; i < 4; ++i) {
                const f64 sx = (i == 0 || i == 3) ? -hw : hw, sy = i < 2 ? -hh : hh;
                p[i] = add(t->position, add(mul(fw, cam->near_z), add(mul(rt, sx), mul(up, sy))));
            }
            for (u32 i = 0; i < 4; ++i)
                frame_seg(d, p[i], p[(i + 1) % 4]);
        } else {
            const f64 depth = 1.5, th = tan(cam->fov_y * 0.5) * depth, tw = th * d->aspect;
            v3 p[4];
            for (u32 i = 0; i < 4; ++i) {
                const f64 sx = (i == 0 || i == 3) ? -tw : tw, sy = i < 2 ? -th : th;
                p[i] = add(t->position, add(mul(fw, depth), add(mul(rt, sx), mul(up, sy))));
            }
            for (u32 i = 0; i < 4; ++i) {
                frame_seg(d, p[i], p[(i + 1) % 4]);
                frame_seg(d, t->position, p[i]);
            }
        }
    }
}

static void draw_cameras(de__editor *ed, const de__ed_cam *c, const de__ed_canvas *cv, const u64 *skip,
                         u32 skip_count) {
    de_world *w = ed->e->world;
    const de_component cam = de_component_find(w, "DeCamera");
    const de_builtin_component_ids b = de_builtin_components(w);
    if (cam.id == 0 || b.global_transform.id == 0)
        return;
    const de_query_term terms[2] = {{cam, DE_READ}, {b.global_transform, DE_READ}};
    const de_query_desc qd = {terms, 2, NULL, 0};
    const de_query q = de_query_create(w, &qd);
    u32 ow = 16, oh = 9;
    if (ed->e->renderer != NULL)
        de_renderer_output_size(ed->e->renderer, &ow, &oh);
    cam_draw d = {c, cv, skip, skip_count, oh ? (f64)ow / oh : 16.0 / 9.0};
    de_query_each(w, q, cam_chunk, &d);
    de_query_destroy(w, q);
}

/* ---------- Icons: cameras and lights ---------- */

#define ICON_REACH 11.0 /* px: a click this close to an icon selects its entity */

/* `data`: the entity's DeCamera or DeLight. */
typedef void (*icon_fn)(u64 e, const de_transform *t, bool light, const void *data, void *user);

typedef struct icon_walk {
    icon_fn fn;
    void *user;
    const u64 *skip;
    u32 skip_count;
    bool light;
} icon_walk;

static void icon_chunk(const de_query_chunk *ch, void *user) {
    const icon_walk *w = (const icon_walk *)user;
    const DeGlobalTransform *ts = (const DeGlobalTransform *)ch->columns[1];
    for (u32 r = 0; r < ch->count; ++r) {
        bool skip = false;
        for (u32 s = 0; s < w->skip_count && !skip; ++s)
            skip = w->skip[s] == ch->entities[r].bits;
        const void *data = w->light ? (const void *)&((const DeLight *)ch->columns[0])[r]
                                    : (const void *)&((const DeCamera *)ch->columns[0])[r];
        if (!skip)
            w->fn(ch->entities[r].bits, &ts[r], w->light, data, w->user);
    }
}

/* Calls `fn` for each camera (`show` has DE__ED_SHOW_CAMERAS) and light (DE__ED_SHOW_LIGHTS). */
static void icons_each(de__editor *ed, u32 show, const u64 *skip, u32 skip_count, icon_fn fn, void *user) {
    de_world *w = ed->e->world;
    const de_builtin_component_ids b = de_builtin_components(w);
    if (b.global_transform.id == 0)
        return;
    static const char *const names[2] = {"DeCamera", "DeLight"};
    static const u32 flags[2] = {DE__ED_SHOW_CAMERAS, DE__ED_SHOW_LIGHTS};
    for (u32 k = 0; k < 2; ++k) {
        const de_component comp = de_component_find(w, names[k]);
        if (!(show & flags[k]) || comp.id == 0)
            continue;
        const de_query_term terms[2] = {{comp, DE_READ}, {b.global_transform, DE_READ}};
        const de_query_desc qd = {terms, 2, NULL, 0};
        const de_query q = de_query_create(w, &qd);
        icon_walk walk = {fn, user, skip, skip_count, k == 1};
        de_query_each(w, q, icon_chunk, &walk);
        de_query_destroy(w, q);
    }
}

static bool selected(u64 e) {
    for (u32 i = 0; i < G.sel_count; ++i)
        if (G.sel[i] == e)
            return true;
    return false;
}

typedef struct icon_draw {
    const de__ed_cam *c;
    const de__ed_canvas *cv;
} icon_draw;

/* A selected light's reach, as Unity draws it: a point light's range (three circles), a spot's
 * cone, a directional light's rays. The light points along its local -Z, as cameras do. */
static void draw_light_shape(const icon_draw *d, const de_transform *t, const DeLight *l) {
    const rgb col = {255, 221, 120};
    const v3 o = t->position, fw = qrot(t->rotation, V(0, 0, -1)), up = qrot(t->rotation, V(0, 1, 0)),
             rt = qrot(t->rotation, V(1, 0, 0));
    enum { N = 64 };
    if (l->kind == DE_LIGHT_POINT || l->kind == DE_LIGHT_RECT) {
        const f64 r = l->range > 0 ? l->range : 10.0;
        const v3 ax[3][2] = {{rt, up}, {rt, fw}, {up, fw}};
        for (u32 k = 0; k < 3; ++k)
            for (u32 i = 0; i < N; ++i) {
                const f64 a0 = 2 * DE_PI * i / N, a1 = 2 * DE_PI * (i + 1) / N;
                wseg(d->c, d->cv, add(o, add(mul(ax[k][0], cos(a0) * r), mul(ax[k][1], sin(a0) * r))),
                     add(o, add(mul(ax[k][0], cos(a1) * r), mul(ax[k][1], sin(a1) * r))), col, 0.8f, 1);
            }
    } else if (l->kind == DE_LIGHT_SPOT) {
        const f64 len = l->range > 0 ? l->range : 10.0, rad = tan(l->spot_outer) * len;
        const v3 c = add(o, mul(fw, len));
        for (u32 i = 0; i < N; ++i) {
            const f64 a0 = 2 * DE_PI * i / N, a1 = 2 * DE_PI * (i + 1) / N;
            wseg(d->c, d->cv, add(c, add(mul(rt, cos(a0) * rad), mul(up, sin(a0) * rad))),
                 add(c, add(mul(rt, cos(a1) * rad), mul(up, sin(a1) * rad))), col, 0.8f, 1);
        }
        for (u32 i = 0; i < 4; ++i) {
            const f64 a = DE_PI * 0.5 * i;
            wseg(d->c, d->cv, o, add(c, add(mul(rt, cos(a) * rad), mul(up, sin(a) * rad))), col, 0.8f, 1);
        }
    } else {
        /* Directional: parallel rays from a disc. */
        const f64 rad = 0.5, len = 2.0;
        for (u32 i = 0; i < 8; ++i) {
            const f64 a = DE_PI * 0.25 * i;
            const v3 s = add(o, add(mul(rt, cos(a) * rad), mul(up, sin(a) * rad)));
            wseg(d->c, d->cv, s, add(s, mul(fw, len)), col, 0.8f, 1);
        }
    }
}

static void draw_icon(u64 e, const de_transform *t, bool light, const void *data, void *user) {
    const icon_draw *d = (const icon_draw *)user;
    if (light && selected(e))
        draw_light_shape(d, t, (const DeLight *)data);
    f64 x, y;
    if (!proj(d->c, t->position, &x, &y))
        return;
    const rgb col = selected(e) ? G.col[C_SELECTED] : light ? (rgb){255, 221, 120} : grey(245);
    const de__ed_canvas *cv = d->cv;
    if (light) {
        /* A sun: a disc and eight rays. */
        for (u32 pass = 0; pass < 2; ++pass) {
            const rgb cc = pass == 0 ? grey(20) : col;
            const u32 w = pass == 0 ? 4 : 2;
            const f32 a = pass == 0 ? 0.45f : 1.0f;
            ring_px(cv, x, y, 4.5, cc, a, w);
            for (u32 i = 0; i < 8; ++i) {
                const f64 an = DE_PI * i / 4.0;
                seg(cv, x + cos(an) * 7.5, y + sin(an) * 7.5, x + cos(an) * 10.5, y + sin(an) * 10.5, cc, a, w);
            }
        }
        box(cv, x, y, 2.5, col, 1.0f);
        return;
    }
    /* A camera: a body and a lens. */
    const f64 body[8] = {x - 9, y - 5, x + 3, y - 5, x + 3, y + 5, x - 9, y + 5};
    const f64 lens[6] = {x + 3, y, x + 9, y - 5, x + 9, y + 5};
    for (u32 i = 0; i < 4; ++i)
        seg(cv, body[i * 2], body[i * 2 + 1], body[((i + 1) % 4) * 2], body[((i + 1) % 4) * 2 + 1], grey(20), 0.45f, 4);
    quad(cv, body, col, 0.95f);
    tri(cv, lens, col, 0.95f);
}

typedef struct icon_hit {
    const de__ed_cam *c;
    f64 x, y, best;
    u64 hit;
} icon_hit;

static void hit_icon(u64 e, const de_transform *t, bool light, const void *data, void *user) {
    (void)light;
    (void)data;
    icon_hit *h = (icon_hit *)user;
    f64 x, y;
    if (!proj(h->c, t->position, &x, &y))
        return;
    const f64 d = hypot(x - h->x, y - h->y);
    if (d <= ICON_REACH && d < h->best) {
        h->best = d;
        h->hit = e;
    }
}

u64 de__ed_icon_pick(de__editor *ed, const de__ed_cam *c, f32 x, f32 y, u32 show, const u64 *skip,
                     u32 skip_count) {
    if (ed->e->world == NULL)
        return 0;
    icon_hit h = {c, x, y, 1e30, 0};
    icons_each(ed, show, skip, skip_count, hit_icon, &h);
    return h.hit;
}

/* ---------- 2D colliders ---------- */

typedef struct coll_draw {
    const de__ed_cam *c;
    const de__ed_canvas *cv;
    bool all;
} coll_draw;

/* A collider's outline in its body's plane: the entity's local X and, as for sprites, the local
 * axis (Z side-on, Y top-down) more across the view. */
static void draw_collider(const coll_draw *d, const DeCollider2D *k, const de_transform *t, bool lit) {
    const v3 f = cam_fwd(d->c);
    const v3 lz = qrot(t->rotation, V(0, 0, 1)), ly = qrot(t->rotation, V(0, 1, 0));
    const bool side = fabs(dot(lz, f)) <= fabs(dot(ly, f));
    const v3 u = mul(qrot(t->rotation, V(1, 0, 0)), t->scale.x);
    const v3 v = mul(side ? lz : ly, side ? t->scale.z : t->scale.y);
    const rgb col = lit ? (rgb){145, 244, 140} : (rgb){110, 200, 110};
    const f32 a = lit ? 1.0f : 0.7f;
    const u32 width = lit ? 2 : 1;
#define P2(x, y) add(t->position, add(mul(u, (x)), mul(v, (y))))
    switch (k->kind) {
    case DE_SHAPE2D_CIRCLE:
    case DE_SHAPE2D_CAPSULE: {
        /* A capsule: two half circles joined, along v by half_extents.y. */
        const f64 r = k->radius, hh = k->kind == DE_SHAPE2D_CAPSULE ? k->half_extents.y : 0.0;
        enum { N = 48 };
        for (u32 i = 0; i < N; ++i) {
            const f64 a0 = 2 * DE_PI * i / N, a1 = 2 * DE_PI * (i + 1) / N;
            const f64 c0 = sin(a0) >= 0 ? hh : -hh, c1 = sin(a1) >= 0 ? hh : -hh;
            wseg(d->c, d->cv, P2(cos(a0) * r, sin(a0) * r + c0), P2(cos(a1) * r, sin(a1) * r + c1), col, a, width);
        }
        if (hh > 0) {
            wseg(d->c, d->cv, P2(r, -hh), P2(r, hh), col, a, width);
            wseg(d->c, d->cv, P2(-r, -hh), P2(-r, hh), col, a, width);
        }
        break;
    }
    case DE_SHAPE2D_BOX: {
        const f64 hx = k->half_extents.x, hy = k->half_extents.y;
        const v3 p[4] = {P2(-hx, -hy), P2(hx, -hy), P2(hx, hy), P2(-hx, hy)};
        for (u32 i = 0; i < 4; ++i)
            wseg(d->c, d->cv, p[i], p[(i + 1) % 4], col, a, width);
        break;
    }
    default: {
        /* Polygon (closed), chain and segment (open): the points. */
        const u32 n = k->point_count < 8 ? k->point_count : 8;
        const bool closed = k->kind == DE_SHAPE2D_POLYGON;
        for (u32 i = 0; i + 1 < n + (closed ? 1u : 0u); ++i) {
            const de_vec2 p0 = k->points[i], p1 = k->points[(i + 1) % n];
            wseg(d->c, d->cv, P2(p0.x, p0.y), P2(p1.x, p1.y), col, a, width);
        }
        break;
    }
    }
#undef P2
}

static void coll_chunk(const de_query_chunk *ch, void *user) {
    const coll_draw *d = (const coll_draw *)user;
    const DeCollider2D *ks = (const DeCollider2D *)ch->columns[0];
    const DeGlobalTransform *ts = (const DeGlobalTransform *)ch->columns[1];
    for (u32 r = 0; r < ch->count; ++r) {
        const bool lit = selected(ch->entities[r].bits);
        if (lit || d->all)
            draw_collider(d, &ks[r], &ts[r], lit);
    }
}

static void draw_colliders(de__editor *ed, const de__ed_cam *c, const de__ed_canvas *cv, bool all) {
    if (!all && G.sel_count == 0)
        return;
    de_world *w = ed->e->world;
    const de_component comp = de_component_find(w, "DeCollider2D");
    const de_builtin_component_ids b = de_builtin_components(w);
    if (comp.id == 0 || b.global_transform.id == 0)
        return;
    const de_query_term terms[2] = {{comp, DE_READ}, {b.global_transform, DE_READ}};
    const de_query_desc qd = {terms, 2, NULL, 0};
    const de_query q = de_query_create(w, &qd);
    coll_draw d = {c, cv, all};
    de_query_each(w, q, coll_chunk, &d);
    de_query_destroy(w, q);
}

static void draw_outlines(de__editor *ed, const de__ed_cam *c, const de__ed_canvas *cv) {
    de_world *w = ed->e->world;
    for (u32 i = 0; i < G.sel_count; ++i) {
        gz_ent g;
        if (!ent_of(w, G.sel[i], &g))
            continue;
        if (sprite_of(w, g.e) != NULL) {
            gz_layout L;
            memset(&L, 0, sizeof L);
            rect_of(w, c, &g, &L);
            f64 p[8];
            if (rect_px(c, &L, p))
                for (u32 k = 0; k < 4; ++k) {
                    const u32 n = (k + 1) % 4;
                    seg(cv, p[k * 2], p[k * 2 + 1], p[n * 2], p[n * 2 + 1], G.col[C_SELECTED], 1.0f, 2);
                }
            continue;
        }
        de_dvec3 lo, hi;
        if (de__ed_entity_bounds(ed, g.e, &lo, &hi)) {
            /* A mesh: its bounding box. */
            v3 p[8];
            for (u32 k = 0; k < 8; ++k)
                p[k] = V(k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z);
            static const u8 edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                            {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (u32 k = 0; k < 12; ++k)
                wseg(c, cv, p[edges[k][0]], p[edges[k][1]], G.col[C_SELECTED], 1.0f, 2);
            continue;
        }
        f64 x, y;
        if (proj(c, g.world0.position, &x, &y)) {
            seg(cv, x - 6, y, x, y - 6, G.col[C_SELECTED], 1.0f, 2);
            seg(cv, x, y - 6, x + 6, y, G.col[C_SELECTED], 1.0f, 2);
            seg(cv, x + 6, y, x, y + 6, G.col[C_SELECTED], 1.0f, 2);
            seg(cv, x, y + 6, x - 6, y, G.col[C_SELECTED], 1.0f, 2);
        }
    }
}

/* A handle's colour: its axis', or the hover colour when it is highlighted. */
static rgb hcol(i32 h, i32 lit, u32 axis) { return h == lit ? G.col[C_HOVER] : G.col[axis]; }

static void draw_gizmo(const de__ed_cam *c, const de__ed_canvas *cv, const gz_layout *L, i32 lit, bool dragging) {
    f64 ox, oy;
    if (!L->ok || !proj(c, L->o, &ox, &oy))
        return;
    const f64 S = L->size;
    /* While dragging, only the handle in use. */
#define SHOW(h) (shown(L, (h)) && (!dragging || lit == (h)))
    if (tool_is("rect")) {
        f64 p[8];
        if (!L->rect || !rect_px(c, L, p))
            return;
        static const i32 side[4] = {H_RB, H_RR, H_RT, H_RL};
        static const i32 corner[4] = {H_RLB, H_RRB, H_RRT, H_RLT};
        for (u32 i = 0; i < 4; ++i) {
            const u32 j = (i + 1) % 4;
            const rgb col = side[i] == lit ? G.col[C_HOVER] : grey(245);
            seg(cv, p[i * 2], p[i * 2 + 1], p[j * 2], p[j * 2 + 1], col, 0.95f, side[i] == lit ? 2 : 1);
        }
        for (u32 i = 0; i < 4; ++i) {
            const u32 j = (i + 1) % 4;
            box(cv, p[i * 2], p[i * 2 + 1], 4.5, grey(30), 0.9f);
            box(cv, p[i * 2], p[i * 2 + 1], 3.5, corner[i] == lit ? G.col[C_HOVER] : grey(245), 1.0f);
            const f64 mx = (p[i * 2] + p[j * 2]) * 0.5, my = (p[i * 2 + 1] + p[j * 2 + 1]) * 0.5;
            box(cv, mx, my, 3.0, side[i] == lit ? G.col[C_HOVER] : grey(245), 0.9f);
        }
        /* The pivot. */
        f64 px, py;
        if (proj(c, L->rp, &px, &py))
            ring_px(cv, px, py, 4.0, lit == H_RIN ? G.col[C_HOVER] : grey(245), 1.0f, 2);
        return;
    }
    /* Rotation rings: the back half dim, behind the rest. */
    for (i32 h = H_RX; h <= H_RV; ++h) {
        if (!SHOW(h))
            continue;
        v3 u, v;
        f64 r;
        ring_of(L, h, &u, &v, &r);
        const rgb col = h == H_RV ? (h == lit ? G.col[C_HOVER] : grey(235)) : hcol(h, lit, (u32)(h - H_RX));
        enum { N = 96 };
        for (u32 i = 0; i < N; ++i) {
            const f64 a0 = 2 * DE_PI * i / N, a1 = 2 * DE_PI * (i + 1) / N;
            const v3 o0 = add(mul(u, cos(a0) * r), mul(v, sin(a0) * r)), o1 = add(mul(u, cos(a1) * r), mul(v, sin(a1) * r));
            const bool front = h == H_RV || dot(add(o0, o1), L->dir) <= 1e-9;
            wseg(c, cv, add(L->o, o0), add(L->o, o1), col, front ? 1.0f : 0.25f, front ? (h == lit ? 3 : 2) : 1);
        }
    }
    if (dragging && lit >= H_RX && lit <= H_RV && fabs(G.ang) > 1e-6) {
        /* The angle swept, as a fan. */
        v3 u, v;
        f64 r;
        ring_of(L, lit, &u, &v, &r);
        const v3 a0 = nrm(sub(G.hit0, L->o));
        const v3 n = plane_normal(L, lit);
        const u32 steps = (u32)fmin(180, fabs(G.ang) * 30 + 2);
        for (u32 i = 0; i < steps; ++i) {
            const v3 p0 = add(L->o, mul(qrot(qaxis(n, G.ang * i / steps), a0), r));
            const v3 p1 = add(L->o, mul(qrot(qaxis(n, G.ang * (i + 1) / steps), a0), r));
            f64 t[6];
            if (proj(c, L->o, &t[0], &t[1]) && proj(c, p0, &t[2], &t[3]) && proj(c, p1, &t[4], &t[5]))
                tri(cv, t, G.col[C_HOVER], 0.25f);
        }
    }
    /* Plane squares. */
    for (u32 k = 0; k < 3; ++k) {
        const i32 h = H_MYZ + (i32)k;
        if (!SHOW(h))
            continue;
        const v3 a = L->ax[(k + 1) % 3], b = L->ax[(k + 2) % 3];
        const f64 lo = S * 0.2, hi = S * 0.42;
        const v3 pts[4] = {add(L->o, add(mul(a, lo), mul(b, lo))), add(L->o, add(mul(a, hi), mul(b, lo))),
                           add(L->o, add(mul(a, hi), mul(b, hi))), add(L->o, add(mul(a, lo), mul(b, hi)))};
        f64 q[8];
        bool ok = true;
        for (u32 i = 0; i < 4 && ok; ++i)
            ok = proj(c, pts[i], &q[i * 2], &q[i * 2 + 1]);
        if (!ok)
            continue;
        const rgb col = hcol(h, lit, k);
        quad(cv, q, col, h == lit ? 0.55f : 0.3f);
        for (u32 i = 0; i < 4; ++i)
            seg(cv, q[i * 2], q[i * 2 + 1], q[((i + 1) % 4) * 2], q[((i + 1) % 4) * 2 + 1], col, 0.9f, 1);
    }
    /* Axes: arrows (move), lines with boxes (scale). */
    for (u32 k = 0; k < 3; ++k) {
        const bool mv = SHOW(H_MX + (i32)k), sc = SHOW(H_SX + (i32)k);
        if (!mv && !sc)
            continue;
        const i32 h = mv ? H_MX + (i32)k : H_SX + (i32)k;
        f64 ex, ey;
        if (!proj(c, add(L->o, mul(L->ax[k], S)), &ex, &ey))
            continue;
        const rgb col = hcol(h, lit, k);
        seg(cv, ox, oy, ex, ey, col, 1.0f, h == lit ? 3 : 2);
        const f64 dx = ex - ox, dy = ey - oy, l = hypot(dx, dy);
        if (l < 1e-6)
            continue;
        const f64 ux = dx / l, uy = dy / l;
        if (mv) {
            const f64 t[6] = {ex + ux * 14, ey + uy * 14, ex - uy * 6, ey + ux * 6, ex + uy * 6, ey - ux * 6};
            tri(cv, t, col, 1.0f);
        } else {
            box(cv, ex, ey, 5.0, col, 1.0f);
        }
    }
    /* The centre: a view-plane move square or the uniform scale box. */
    if (SHOW(H_MC)) {
        const rgb col = lit == H_MC ? G.col[C_HOVER] : grey(240);
        seg(cv, ox - 6, oy - 6, ox + 6, oy - 6, col, 1.0f, 2);
        seg(cv, ox + 6, oy - 6, ox + 6, oy + 6, col, 1.0f, 2);
        seg(cv, ox + 6, oy + 6, ox - 6, oy + 6, col, 1.0f, 2);
        seg(cv, ox - 6, oy + 6, ox - 6, oy - 6, col, 1.0f, 2);
    }
    if (SHOW(H_SC))
        box(cv, ox, oy, 6.0, lit == H_SC ? G.col[C_HOVER] : grey(240), 1.0f);
#undef SHOW
    if (dragging && G.typed[0] != '\0') {
        /* The typed value shows in the IDE (at the pointer); here, a mark at the centre. */
        ring_px(cv, ox, oy, 3.0, G.col[C_HOVER], 1.0f, 2);
    }
}

void de__ed_gizmo_draw(de__editor *ed, u32 view, const de__ed_cam *c, const de__ed_canvas *cv, u32 show,
                       const u64 *skip, u32 skip_count) {
    if (ed->e->world == NULL || c->w == 0 || c->h == 0)
        return;
    if (show & DE__ED_SHOW_GRID)
        draw_grid(c, cv);
    if (show & DE__ED_SHOW_CAMERAS)
        draw_cameras(ed, c, cv, skip, skip_count);
    if (show & DE__ED_SHOW_OUTLINE)
        draw_outlines(ed, c, cv);
    draw_colliders(ed, c, cv, (show & DE__ED_SHOW_COLLIDERS) != 0);
    icon_draw idr = {c, cv};
    icons_each(ed, show, skip, skip_count, draw_icon, &idr);
    if (!(show & DE__ED_SHOW_GIZMOS) || !tool_drags() || G.sel_count == 0)
        return;
    if (G.drag && G.view == view) {
        /* The gizmo where it is now, the handle in use lit. */
        gz_layout L = layout(ed->e->world, c);
        memcpy(L.ax, G.L.ax, sizeof L.ax);
        draw_gizmo(c, cv, &L, G.h, true);
        return;
    }
    const gz_layout L = layout(ed->e->world, c);
    draw_gizmo(c, cv, &L, G.hover_view == view ? G.hover : H_NONE, false);
}
