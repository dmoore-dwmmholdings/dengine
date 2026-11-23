/* editor_physics.c: physics.loadAsset and the mouse spring physics.drag.begin, physics.drag.update
 * and physics.drag.end (DENG-821; DIDE protocol 1.23, DIDE-397, capabilities physics.authoring
 * and physics.mouseSpring).
 *
 * physics.loadAsset parses a `.physmat.toml` or `.ragdoll.toml` asset (the IDE's unsaved text, or
 * the project file) and registers it in the physics world under its project-relative path, the
 * name DeCollider.material_asset and DeRagdoll.asset use. A ragdoll registered again respawns
 * every DeRagdoll that names it on the next physics.step.
 *
 * The mouse spring drags one dynamic body (a ragdoll bone included) for the connection:
 * - 3D: there is no force API, so a soft point constraint (Box2D's mouse joint: frequency `hertz`,
 *   damping ratio, force cap) is applied as one velocity impulse per physics tick, by the
 *   FixedUpdate system "editor.physics.drag" that runs before physics.step. It acts only while
 *   physics.step is enabled (Play and Simulate, and the frames of a session.step in Paused).
 * - 2D: a DE_JOINT2D_MOUSE joint from the world to the body. The ray meets the 2D plane (depth 0)
 *   and the body at that point is grabbed.
 * A drag ends on drag.end, on a new drag.begin, on Stop (Edit), when its body or entity goes and
 * when the connection drops.
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* snprintf */
#endif
#include "host/editor/editor_internal.h"

#include "ecs/ecs_internal.h"
#include "ecs/hot_reload_internal.h"
#include "ecs/system_internal.h"
#include <dengine/de_physics.h>
#include <dengine/de_platform.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PHYS_AUTHORING    "physics.authoring"
#define PHYS_MOUSE_SPRING "physics.mouseSpring"
#define PHYS_MAX_TEXT     (1u << 20)   /* physics.loadAsset text limit */
#define PHYS_MAX_HITS     64u          /* hits a drag.begin ray looks through */
#define PHYS_DRAG_SYSTEM  "editor.physics.drag"
#define PHYS_PI           3.14159265358979323846

/* The 2D world resource (src/physics/2d/ecs2d/attach2d.c): one pointer. */
typedef struct phys2d_resource {
    de_physics2d_world *world;
} phys2d_resource;

typedef struct phys_drag {
    bool live;
    bool is2d;
    u32 id;
    de_entity entity; /* the body's entity (the DeRagdoll owner for a bone) */
    de_body body;
    de_vec3 local;    /* the grab point in the body frame (from the body origin) */
    de_dvec3 target;  /* world space */
    f32 hertz, damping_ratio, max_force; /* max_force 0: 1000 times the mass */
    de_joint joint2d;
} phys_drag;

static struct {
    phys_drag drag;
    u32 last_id; /* drag ids are never reused in a run */
    de_world *system_world;
    de_system system;
} g_phys;

static de_world *world_of(de__editor *ed) {
    return ed->e != NULL ? ed->e->world : NULL;
}

static void *resource(de_world *w, const char *type) {
    if (w == NULL)
        return NULL;
    const de_type_id t = de_type_find(de_world_types(w), type);
    if (t.id == 0)
        return NULL;
    const de_resource_id r = de_resource_find(w, t);
    return DE_HANDLE_VALID(r) ? de_resource_get(w, r) : NULL;
}

static de_physics_world *phys3d(de_world *w) {
    const DePhysicsWorld *r = (const DePhysicsWorld *)resource(w, "DePhysicsWorld");
    return r != NULL ? r->world : NULL;
}

static de_physics2d_world *phys2d(de_world *w) {
    const phys2d_resource *r = (const phys2d_resource *)resource(w, "DePhysics2DWorld");
    return r != NULL ? r->world : NULL;
}

/* ---------- Drag lifetime ---------- */

static void drag_end(de__editor *ed) {
    if (g_phys.drag.live && g_phys.drag.is2d && DE_HANDLE_VALID(g_phys.drag.joint2d)) {
        de_physics2d_world *p = phys2d(world_of(ed));
        if (p != NULL)
            de_joint2d_destroy(p, g_phys.drag.joint2d);
    }
    memset(&g_phys.drag, 0, sizeof g_phys.drag);
}

/* Ends the drag when it no longer applies: Edit mode, or its entity or body went. */
static void drag_check(de__editor *ed) {
    if (!g_phys.drag.live)
        return;
    de_world *w = world_of(ed);
    if (ed->mode == DE_EP_WORLD_MODE_EDIT || w == NULL ||
        !de_entity_alive(w, g_phys.drag.entity)) {
        drag_end(ed);
        return;
    }
    if (g_phys.drag.is2d) {
        de_physics2d_world *p = phys2d(w);
        if (p == NULL || !de_body2d_valid(p, g_phys.drag.body) ||
            de_joint2d_is_broken(p, g_phys.drag.joint2d))
            drag_end(ed);
    } else {
        de_physics_world *p = phys3d(w);
        if (p == NULL || !de_body_valid(p, g_phys.drag.body))
            drag_end(ed);
    }
}

static void tick(de__editor *ed) {
    drag_check(ed);
}

static void reset(de__editor *ed) {
    drag_end(ed); /* the connection dropped */
}

/* ---------- The 3D spring ---------- */

static bool step_enabled(de_world *w) {
    const ecs_system_registry *reg = w->systems;
    for (u32 i = 0; reg != NULL && i < reg->slots.count; ++i) {
        const ecs_system_slot *slot = (const ecs_system_slot *)de_array_at(&reg->slots, i);
        if (slot->sys != NULL && slot->sys->name != NULL &&
            strcmp(slot->sys->name, DE_PHYSICS_STEP_SYSTEM) == 0)
            return de_system_enabled(w, slot->sys->handle);
    }
    return false;
}

static de_vec3 v3(f32 x, f32 y, f32 z) {
    return (de_vec3){x, y, z};
}
static de_vec3 v3_add(de_vec3 a, de_vec3 b) {
    return v3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static de_vec3 v3_scale(de_vec3 a, f32 s) {
    return v3(a.x * s, a.y * s, a.z * s);
}
static de_vec3 v3_cross(de_vec3 a, de_vec3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
/* Column-major 3x3 (element (r, c) is m[c * 3 + r]), in double for the solve. */
typedef struct m3d {
    f64 m[9];
} m3d;
static f64 m3_at(const m3d *a, int r, int c) {
    return a->m[c * 3 + r];
}
static bool m3_inverse(const m3d *a, m3d *out) {
    const f64 a00 = m3_at(a, 0, 0), a01 = m3_at(a, 0, 1), a02 = m3_at(a, 0, 2);
    const f64 a10 = m3_at(a, 1, 0), a11 = m3_at(a, 1, 1), a12 = m3_at(a, 1, 2);
    const f64 a20 = m3_at(a, 2, 0), a21 = m3_at(a, 2, 1), a22 = m3_at(a, 2, 2);
    const f64 c00 = a11 * a22 - a12 * a21, c01 = a02 * a21 - a01 * a22,
              c02 = a01 * a12 - a02 * a11;
    const f64 det = a00 * c00 + a10 * c01 + a20 * c02;
    if (!(fabs(det) > 1e-30) || !isfinite(det))
        return false;
    const f64 inv = 1.0 / det;
    const f64 r[3][3] = {{c00, c01, c02},
                         {a12 * a20 - a10 * a22, a00 * a22 - a02 * a20, a02 * a10 - a00 * a12},
                         {a10 * a21 - a11 * a20, a01 * a20 - a00 * a21, a00 * a11 - a01 * a10}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out->m[j * 3 + i] = r[i][j] * inv;
    return true;
}
static void m3_mul_v(const m3d *a, const f64 v[3], f64 out[3]) {
    for (int r = 0; r < 3; ++r)
        out[r] = m3_at(a, r, 0) * v[0] + m3_at(a, r, 1) * v[1] + m3_at(a, r, 2) * v[2];
}

/* World inverse inertia R * I^-1 * R^T from the local inertia (about the center of mass). */
static bool inv_inertia_world(const de_mat3 *local, de_quat q, m3d *out) {
    m3d l, li;
    for (int i = 0; i < 9; ++i)
        l.m[i] = local->m[i];
    if (!m3_inverse(&l, &li))
        return false;
    de_vec3 col[3];
    for (int c = 0; c < 3; ++c)
        col[c] = de_quat_rotate(q, v3(c == 0, c == 1, c == 2)); /* R's columns */
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            f64 s = 0;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    const f64 rri = (&col[i].x)[r], rcj = (&col[j].x)[c];
                    s += rri * m3_at(&li, i, j) * rcj;
                }
            out->m[c * 3 + r] = s;
        }
    return true;
}

/* One tick of the spring: the impulse of a soft point constraint between the grab point and the
 * target, capped at max_force * dt, applied to the body's velocities. */
static void spring_apply(de_physics_world *p, const phys_drag *d, f32 dt) {
    de_mass_props mp;
    de_transform t;
    de_vec3 v, w;
    if (!de_body_get_mass_props(p, d->body, &mp) || !(mp.mass > 0) ||
        !de_body_get_transform(p, d->body, &t) || !de_body_get_velocity(p, d->body, &v, &w))
        return;
    m3d iinv;
    if (!inv_inertia_world(&mp.inertia, t.rotation, &iinv))
        return;
    const de_vec3 rc = de_quat_rotate(t.rotation, mp.center);
    const de_vec3 ra = de_quat_rotate(t.rotation, d->local);
    const de_vec3 r = v3(ra.x - rc.x, ra.y - rc.y, ra.z - rc.z); /* center of mass to grab point */
    const f64 C[3] = {t.position.x + ra.x - d->target.x, t.position.y + ra.y - d->target.y,
                      t.position.z + ra.z - d->target.z};
    const de_vec3 vp = v3_add(v, v3_cross(w, r));
    const f64 m = mp.mass, h = dt;
    const f64 omega = 2.0 * PHYS_PI * d->hertz;
    const f64 k = m * omega * omega, c = 2.0 * m * d->damping_ratio * omega;
    const f64 hk = h * (c + h * k);
    const f64 gamma = hk > 0 ? 1.0 / hk : 0.0;
    const f64 beta = h * k * gamma;
    /* K = I/m - [r]x I^-1 [r]x + gamma I: the change of the grab point's velocity per impulse. */
    m3d K;
    for (int col = 0; col < 3; ++col) {
        const de_vec3 e = v3(col == 0, col == 1, col == 2);
        const de_vec3 rxe = v3_cross(r, e);
        const f64 a[3] = {rxe.x, rxe.y, rxe.z};
        f64 ia[3];
        m3_mul_v(&iinv, a, ia);
        const de_vec3 dw = v3((f32)ia[0], (f32)ia[1], (f32)ia[2]);
        const de_vec3 dv = v3_cross(dw, r);
        const f64 dvv[3] = {dv.x, dv.y, dv.z};
        for (int row = 0; row < 3; ++row)
            K.m[col * 3 + row] = dvv[row] + (row == col ? 1.0 / m + gamma : 0.0);
    }
    m3d Ki;
    if (!m3_inverse(&K, &Ki))
        return;
    const f64 rhs[3] = {-(vp.x + beta * C[0]), -(vp.y + beta * C[1]), -(vp.z + beta * C[2])};
    f64 P[3];
    m3_mul_v(&Ki, rhs, P);
    const f64 max_force = d->max_force > 0 ? d->max_force : 1000.0 * m;
    const f64 cap = max_force * h;
    const f64 len = sqrt(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
    if (!isfinite(len))
        return;
    if (len > cap)
        for (int i = 0; i < 3; ++i)
            P[i] *= cap / len;
    const de_vec3 imp = v3((f32)P[0], (f32)P[1], (f32)P[2]);
    const de_vec3 rxp = v3_cross(r, imp);
    const f64 a[3] = {rxp.x, rxp.y, rxp.z};
    f64 dw[3];
    m3_mul_v(&iinv, a, dw);
    const de_vec3 nv = v3_add(v, v3_scale(imp, (f32)(1.0 / m)));
    const de_vec3 nw = v3_add(w, v3((f32)dw[0], (f32)dw[1], (f32)dw[2]));
    de_body_set_velocity(p, d->body, nv, nw); /* wakes the body */
}

static void drag_system(de_system_ctx *ctx) {
    const phys_drag *d = &g_phys.drag;
    if (!d->live || d->is2d || !(ctx->dt > 0) || !step_enabled(ctx->world))
        return;
    de_physics_world *p = phys3d(ctx->world);
    if (p != NULL && de_body_valid(p, d->body))
        spring_apply(p, d, ctx->dt);
}

/* Adds the spring system to the world once (an engine system: a hot reload keeps it). */
static void ensure_system(de_world *w) {
    if (g_phys.system_world == w && de_system_enabled(w, g_phys.system))
        return;
    const char *before[] = {DE_PHYSICS_STEP_SYSTEM};
    const de_system_desc desc = {.name = PHYS_DRAG_SYSTEM,
                                 .schedule = DE_SCHEDULE_FIXED_UPDATE,
                                 .fn = drag_system,
                                 .flags = DE_SYSTEM_EXCLUSIVE | DE_SYSTEM_MAIN_THREAD,
                                 .before = before,
                                 .before_count = 1};
    const u32 owner = de__world_system_owner(w);
    de__world_set_system_owner(w, DE__SYSTEM_OWNER_ENGINE);
    de_system s = de_system_add(w, &desc);
    de__world_set_system_owner(w, owner);
    if (!DE_HANDLE_VALID(s)) {
        de_error_clear(); /* already there (a world the editor met before) */
        const ecs_system_registry *reg = w->systems;
        for (u32 i = 0; reg != NULL && i < reg->slots.count; ++i) {
            const ecs_system_slot *slot = (const ecs_system_slot *)de_array_at(&reg->slots, i);
            if (slot->sys != NULL && slot->sys->name != NULL &&
                strcmp(slot->sys->name, PHYS_DRAG_SYSTEM) == 0)
                s = slot->sys->handle;
        }
    }
    g_phys.system_world = w;
    g_phys.system = s;
}

/* ---------- physics.loadAsset ---------- */

static bool has_suffix(const char *s, const char *suffix) {
    const size_t n = strlen(s), k = strlen(suffix);
    return n >= k && strcmp(s + n - k, suffix) == 0;
}

/* A project-relative path: no absolute paths, "..", backslashes or drive letters. */
static bool safe_path(const char *path) {
    return path[0] != '\0' && path[0] != '/' && strchr(path, '\\') == NULL &&
           strchr(path, ':') == NULL && strstr(path, "..") == NULL;
}

typedef struct ragdoll_count {
    de_component comp;
    de_name name;
    u32 n;
} ragdoll_count;

static void count_ragdolls(const de_query_chunk *ch, void *user) {
    ragdoll_count *rc = (ragdoll_count *)user;
    const DeRagdoll *r = (const DeRagdoll *)ch->columns[0];
    for (u32 i = 0; i < ch->count; ++i)
        rc->n += r[i].asset.hash == rc->name.hash;
}

static u32 ragdolls_naming(de_world *w, de_name name) {
    ragdoll_count rc = {de_component_find(w, "DeRagdoll"), name, 0};
    if (!DE_HANDLE_VALID(rc.comp))
        return 0;
    const de_query_term terms[1] = {{rc.comp, DE_READ}};
    const de_query_desc qd = {terms, 1, NULL, 0};
    const de_query q = de_query_create(w, &qd);
    if (!DE_HANDLE_VALID(q)) {
        de_error_clear();
        return 0;
    }
    de_query_each(w, q, count_ragdolls, &rc);
    de_query_destroy(w, q);
    return rc.n;
}

static void load_asset(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                       mpack_writer_t *w) {
    DE__ED_PARAMS(physics_load_asset, de_ep_physics_load_asset_params, q);
    const char *path = de__ed_cstr(a, q.path);
    if (path == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "physics.loadAsset: out of memory");
        return;
    }
    de_ep_physics_load_asset r = {0};
    if (has_suffix(path, ".physmat.toml"))
        r.kind = DE_EP_PHYSICS_ASSET_KIND_MATERIAL;
    else if (has_suffix(path, ".ragdoll.toml"))
        r.kind = DE_EP_PHYSICS_ASSET_KIND_RAGDOLL;
    else {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "%s: not a .physmat.toml or .ragdoll.toml", path);
        return;
    }
    if (!safe_path(path)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "%s: not a project-relative path",
                     path);
        return;
    }
    const char *text = NULL;
    u64 len = 0;
    void *file = NULL;
    if (q.has_text) {
        if (q.text.len > PHYS_MAX_TEXT) {
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "%s: text is over 1 MiB", path);
            return;
        }
        text = q.text.ptr;
        len = q.text.len;
    } else {
        char vpath[1024];
        if (!de__ed_project_vpath(ed, path, vpath, sizeof vpath) ||
            de_file_read_all(vpath, de_heap_allocator(), &file, &len) != DE_OK) {
            de_error_clear();
            de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "no file %s", path);
            return;
        }
        if (len > PHYS_MAX_TEXT) {
            de_heap_allocator()->free(de_heap_allocator()->user, file, (size_t)len);
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "%s: file is over 1 MiB", path);
            return;
        }
        text = (const char *)file;
    }
    de_world *world = world_of(ed);
    de_physics_world *p = phys3d(world);
    const de_name name = de_name_from(path);
    de_result res;
    const char *what = "";
    if (r.kind == DE_EP_PHYSICS_ASSET_KIND_MATERIAL) {
        de_material_phys m;
        res = de_physics_material_parse(text != NULL ? text : "", (u32)len, &m);
        if (res == DE_OK && p != NULL)
            res = de_physics_material_register(p, name, &m);
        what = "material";
    } else {
        de_ragdoll_asset *ra = (de_ragdoll_asset *)malloc(sizeof *ra);
        res = ra != NULL ? de_ragdoll_asset_parse(text != NULL ? text : "", (u32)len, ra)
                         : DE_ERR_OUT_OF_MEMORY;
        if (res == DE_OK && p != NULL) {
            res = de_physics_ragdoll_asset_register(p, name, ra);
            if (res == DE_OK)
                r.respawned = ragdolls_naming(world, name);
        }
        free(ra);
        what = "ragdoll";
    }
    if (file != NULL)
        de_heap_allocator()->free(de_heap_allocator()->user, file, (size_t)len);
    if (res != DE_OK) {
        const de_error *e = de_error_last();
        char msg[256];
        snprintf(msg, sizeof msg, "%s", e != NULL && e->message[0] ? e->message
                                                                   : de_result_str(res));
        de_error_clear();
        const de_ep_error code = res == DE_ERR_OUT_OF_MEMORY ? DE_EP_ERR_BUSY
                                                             : DE_EP_ERR_INVALID_PARAMS;
        de__ed_error(w, req->id, code, "%s: bad %s asset: %s", path, what, msg);
        return;
    }
    if (p == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "%s: the game has no 3D physics world",
                     path);
        return;
    }
    de_ep_write_response_physics_load_asset(w, req->id, &r);
}

/* ---------- physics.drag.* ---------- */

static bool finite3(const de_ep_list_f64 *l) {
    return l->count == 3 && isfinite(l->items[0]) && isfinite(l->items[1]) &&
           isfinite(l->items[2]);
}

static bool finite3f(const de_ep_list_f32 *l) {
    return l->count == 3 && isfinite(l->items[0]) && isfinite(l->items[1]) &&
           isfinite(l->items[2]);
}

/* The bone of ragdoll owner `e` whose body is `b`; false when it is not a bone. */
static bool bone_of(de_world *w, de_physics_world *p, de_entity e, de_body b, u32 *bone) {
    const de_component comp = de_component_find(w, "DeRagdoll");
    const DeRagdoll *r =
        DE_HANDLE_VALID(comp) ? (const DeRagdoll *)de_entity_get_const(w, e, comp) : NULL;
    if (r == NULL || !de_ragdoll_valid(p, r->ragdoll))
        return false;
    const u32 n = de_ragdoll_bone_count(p, r->ragdoll);
    for (u32 i = 0; i < n; ++i)
        if (de_ragdoll_bone_body(p, r->ragdoll, i).id == b.id) {
            *bone = i;
            return true;
        }
    return false;
}

typedef struct grab {
    bool hit;
    de_entity entity;
    de_body body;
    bool has_bone;
    u32 bone;
    de_dvec3 point;
    f64 distance;
    de_vec3 local;
    bool is2d;
    de_physics2d_plane plane;
    de_dpoint2d point2d;
    de_vec2 local2d;
} grab;

static void grab3d(de_world *w, de_physics_world *p, de_dvec3 o, de_vec3 dir, f32 max,
                   const de_entity *only, grab *g) {
    static de_ray_hit hits[PHYS_MAX_HITS];
    const u32 n = de_physics_raycast_all_ex(p, o, dir, max, NULL, hits, PHYS_MAX_HITS);
    for (u32 i = 0; i < n; ++i) {
        const de_ray_hit *h = &hits[i];
        de_mass_props mp;
        if (only != NULL && h->entity.bits != only->bits)
            continue;
        if (h->entity.bits == 0 || !de_body_get_mass_props(p, h->body, &mp) || !(mp.mass > 0))
            continue; /* static or kinematic, or outside the ECS */
        de_transform t;
        if (!de_body_get_transform(p, h->body, &t))
            continue;
        g->hit = true;
        g->entity = h->entity;
        g->body = h->body;
        g->has_bone = bone_of(w, p, h->entity, h->body, &g->bone);
        g->point = h->point;
        g->distance = (f64)h->fraction * max;
        const de_vec3 d = v3((f32)(h->point.x - t.position.x), (f32)(h->point.y - t.position.y),
                             (f32)(h->point.z - t.position.z));
        g->local = de_quat_rotate(de_quat_conj(t.rotation), d);
        return;
    }
}

/* The 2D plane coordinates of world point `p`. */
static de_dpoint2d to_plane(de_physics2d_plane plane, de_dvec3 p) {
    return plane == DE_PHYSICS2D_PLANE_SIDE ? (de_dpoint2d){p.x, p.z} : (de_dpoint2d){p.x, p.y};
}

static void grab2d(de_physics2d_world *p, de_dvec3 o, de_vec3 dir, f32 max,
                   const de_entity *only, grab *g) {
    const de_physics2d_plane plane = de_physics2d_get_plane(p);
    const f64 od = plane == DE_PHYSICS2D_PLANE_SIDE ? o.y : o.z;
    const f64 dd = plane == DE_PHYSICS2D_PLANE_SIDE ? dir.y : dir.z;
    if (fabs(dd) < 1e-9)
        return; /* parallel to the plane */
    const f64 t = -od / dd;
    if (t < 0 || t > max)
        return;
    const de_dvec3 at = {o.x + dir.x * t, o.y + dir.y * t, o.z + dir.z * t};
    const de_dpoint2d q = to_plane(plane, at);
    de_ray_hit2d h;
    if (!de_physics2d_closest_point(p, q, 0.0f, NULL, &h) || h.entity.bits == 0 ||
        (only != NULL && h.entity.bits != only->bits))
        return;
    de_mass_props2d mp;
    de_transform2d bt;
    if (!de_body2d_get_mass_props(p, h.body, &mp) || !(mp.mass > 0) ||
        !de_body2d_get_transform(p, h.body, &bt))
        return;
    const f64 dx = q.x - bt.x, dy = q.y - bt.y;
    g->hit = true;
    g->is2d = true;
    g->plane = plane;
    g->entity = h.entity;
    g->body = h.body;
    g->point = at;
    g->point2d = q;
    g->distance = t;
    /* Into the body frame: rotate by the inverse rotation (c, -s). */
    g->local2d = (de_vec2){(f32)(bt.rotation.c * dx + bt.rotation.s * dy),
                           (f32)(-bt.rotation.s * dx + bt.rotation.c * dy)};
}

static void drag_begin(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                       mpack_writer_t *w) {
    drag_check(ed);
    if (ed->mode == DE_EP_WORLD_MODE_EDIT) {
        de__ed_error(w, req->id, DE_EP_ERR_WRONG_MODE,
                     "physics.drag.begin: the mouse spring needs Play, Simulate or Paused");
        return;
    }
    DE__ED_PARAMS(physics_drag_begin, de_ep_physics_drag_begin_params, q);
    const f32 hertz = q.has_hertz ? q.hertz : 5.0f;
    const f32 zeta = q.has_damping_ratio ? q.damping_ratio : 0.7f;
    if (!(hertz > 0) || !isfinite(hertz)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "hertz must be > 0");
        return;
    }
    if (!(zeta >= 0) || !isfinite(zeta)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "dampingRatio must be >= 0");
        return;
    }
    if (q.has_max_force && (!(q.max_force > 0) || !isfinite(q.max_force))) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "maxForce must be > 0");
        return;
    }
    if (!finite3(&q.origin) || !finite3f(&q.direction)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "origin and direction are finite [x, y, z]");
        return;
    }
    const de_dvec3 o = {q.origin.items[0], q.origin.items[1], q.origin.items[2]};
    de_vec3 dir = v3(q.direction.items[0], q.direction.items[1], q.direction.items[2]);
    const f64 dl = sqrt((f64)dir.x * dir.x + (f64)dir.y * dir.y + (f64)dir.z * dir.z);
    if (!(dl > 0) || !isfinite(dl)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "direction is zero");
        return;
    }
    dir = v3_scale(dir, (f32)(1.0 / dl));
    const f32 max = q.has_max_distance ? q.max_distance : 1000.0f;
    if (!(max > 0) || !isfinite(max)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "maxDistance must be > 0");
        return;
    }
    de_world *world = world_of(ed);
    const de_entity only = {q.entity};
    if (q.has_entity && (world == NULL || !de_entity_alive(world, only))) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "entity %llu", (unsigned long long)q.entity);
        return;
    }
    drag_end(ed); /* a new begin ends the drag before it */
    grab g;
    memset(&g, 0, sizeof g);
    de_physics_world *p3 = phys3d(world);
    de_physics2d_world *p2 = phys2d(world);
    if (p3 != NULL)
        grab3d(world, p3, o, dir, max, q.has_entity ? &only : NULL, &g);
    if (!g.hit && p2 != NULL)
        grab2d(p2, o, dir, max, q.has_entity ? &only : NULL, &g);
    de_ep_physics_drag_begin r;
    memset(&r, 0, sizeof r);
    if (g.hit) {
        phys_drag d;
        memset(&d, 0, sizeof d);
        d.is2d = g.is2d;
        d.entity = g.entity;
        d.body = g.body;
        d.local = g.local;
        d.target = g.point;
        d.hertz = hertz;
        d.damping_ratio = zeta;
        d.max_force = q.has_max_force ? q.max_force : 0.0f;
        bool ok = true;
        if (g.is2d) {
            de_mass_props2d mp;
            const f32 mass = de_body2d_get_mass_props(p2, g.body, &mp) ? mp.mass : 0.0f;
            const de_joint2d_desc jd = {.kind = DE_JOINT2D_MOUSE,
                                        .body_b = g.body,
                                        .local_anchor_b = g.local2d,
                                        .world_anchor_a = g.point2d,
                                        .hertz = hertz,
                                        .damping_ratio = zeta,
                                        .max_motor_force =
                                            q.has_max_force ? q.max_force : 1000.0f * mass};
            ok = de_joint2d_create(p2, &jd, &d.joint2d) == DE_OK;
            if (!ok)
                de_error_clear();
        } else {
            ensure_system(world);
        }
        if (ok) {
            d.live = true;
            d.id = ++g_phys.last_id;
            if (d.id == 0)
                d.id = ++g_phys.last_id;
            g_phys.drag = d;
            r.has_drag = true;
            r.drag = d.id;
            r.has_entity = true;
            r.entity = g.entity.bits;
            r.has_bone = g.has_bone;
            r.bone = g.bone;
            double *pt = (double *)de__ed_alloc(a, 3 * sizeof(double));
            if (pt != NULL) {
                pt[0] = g.point.x;
                pt[1] = g.point.y;
                pt[2] = g.point.z;
                r.has_point = true;
                r.point = (de_ep_list_f64){pt, 3};
            }
            r.has_distance = true;
            r.distance = (float)g.distance;
        }
    }
    de_ep_write_response_physics_drag_begin(w, req->id, &r);
}

static void drag_update(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                        mpack_writer_t *w) {
    drag_check(ed);
    if (ed->mode == DE_EP_WORLD_MODE_EDIT) {
        de__ed_error(w, req->id, DE_EP_ERR_WRONG_MODE,
                     "physics.drag.update: the mouse spring needs Play, Simulate or Paused");
        return;
    }
    DE__ED_PARAMS(physics_drag_update, de_ep_physics_drag_update_params, q);
    if (!g_phys.drag.live || g_phys.drag.id != q.drag) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "drag %u", (unsigned)q.drag);
        return;
    }
    if (!finite3(&q.target)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "target is a finite [x, y, z]");
        return;
    }
    const de_dvec3 t = {q.target.items[0], q.target.items[1], q.target.items[2]};
    g_phys.drag.target = t;
    if (g_phys.drag.is2d) {
        de_physics2d_world *p = phys2d(world_of(ed));
        if (p == NULL ||
            de_joint2d_mouse_set_target(p, g_phys.drag.joint2d,
                                        to_plane(de_physics2d_get_plane(p), t)) != DE_OK) {
            de_error_clear();
            drag_end(ed);
            de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "drag %u: its body went",
                         (unsigned)q.drag);
            return;
        }
    }
    de_ep_write_response_physics_drag_update(w, req->id, &(de_ep_empty){0});
}

static void drag_end_call(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                          mpack_writer_t *w) {
    drag_check(ed);
    DE__ED_PARAMS(physics_drag_end, de_ep_physics_drag_params, q);
    if (!g_phys.drag.live || g_phys.drag.id != q.drag) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "drag %u", (unsigned)q.drag);
        return;
    }
    drag_end(ed); /* the body keeps its velocity */
    de_ep_write_response_physics_drag_end(w, req->id, &(de_ep_empty){0});
}

void de__ed_physics_register(de__editor *ed) {
    memset(&g_phys.drag, 0, sizeof g_phys.drag);
    g_phys.system_world = NULL;
    de__ed_on(ed, DE_EP_METHOD_PHYSICS_LOAD_ASSET, load_asset);
    de__ed_on(ed, DE_EP_METHOD_PHYSICS_DRAG_BEGIN, drag_begin);
    de__ed_on(ed, DE_EP_METHOD_PHYSICS_DRAG_UPDATE, drag_update);
    de__ed_on(ed, DE_EP_METHOD_PHYSICS_DRAG_END, drag_end_call);
    de__ed_on_tick(ed, tick);
    de__ed_on_reset(ed, reset);
    de__ed_add_capability(ed, PHYS_AUTHORING);
    de__ed_add_capability(ed, PHYS_MOUSE_SPRING);
}
