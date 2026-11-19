/* editor_spline.c: spline.info, spline.sample and spline.instances (DENG-775; DIDE protocol 1.12,
 * capability world.splines).
 *
 * A spline is an entity with DeSpline (src/world/spline). The IDE edits the points through the
 * ordinary world edits and asks these methods for the curve as the engine evaluates it, so that
 * its overlay and the placed meshes match the engine. Every reply is in world space and carries
 * the entity revision it describes.
 *
 * Not here: spline.carve (an edit op on a terrain entity) needs world.terrain, which the engine
 * does not advertise yet. Rendering the placed meshes is DENG-583; spline.instances is what that
 * system will place, from the same de__spline_place().
 */
#include "host/editor/editor_internal.h"

#include "world/spline/spline.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static de_world *world_of(de__editor *ed) {
    return ed->e != NULL ? ed->e->world : NULL;
}

/* The spline components of the world, registered on first use (the world may be replaced). */
static bool ids_of(de_world *w, de_spline_ids *out) {
    return w != NULL && de__spline_register(w, out) == DE_OK;
}

de_transform de__ed_world_xf(de_world *w, de_entity e) {
    const de_builtin_component_ids b = de_builtin_components(w);
    const DeTransform *l = (const DeTransform *)de_entity_get_const(w, e, b.transform);
    if (l == NULL)
        return de_transform_identity();
    const DeChildOf *co = (const DeChildOf *)de_entity_get_const(w, e, b.child_of);
    if (co == NULL || !de_entity_alive(w, co->parent))
        return *l;
    const DeGlobalTransform *g =
        (const DeGlobalTransform *)de_entity_get_const(w, e, b.global_transform);
    if (g != NULL)
        return *g;
    const de_transform parent = de__ed_world_xf(w, co->parent);
    return de_transform_mul(&parent, l);
}

/* The spline of `bits`, or answers not_found and returns NULL. */
static const DeSpline *spline_of(de__editor *ed, mpack_writer_t *w, u64 id, u64 bits,
                                 const char *method, de_spline_ids *ids) {
    de_world *wd = world_of(ed);
    const de_entity e = {bits};
    const DeSpline *s = NULL;
    if (ids_of(wd, ids) && de_entity_alive(wd, e))
        s = (const DeSpline *)de_entity_get_const(wd, e, ids->spline);
    if (s == NULL)
        de__ed_error(w, id, DE_EP_ERR_NOT_FOUND, "%s: entity %llu has no " DE_SPLINE_COMPONENT,
                     method, (unsigned long long)bits);
    return s;
}

/* The polyline is 33 KB: built in the response arena, not on the stack. */
static de_spline_poly *poly_of(de__editor *ed, de_ep_arena *a, const DeSpline *s, u64 bits) {
    de_spline_poly *p = (de_spline_poly *)de__ed_alloc(a, sizeof *p);
    if (p == NULL)
        return NULL;
    const de_transform t = de__ed_world_xf(world_of(ed), (de_entity){bits});
    de__spline_poly(s, &t, p);
    return p;
}

static void spline_info(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                        mpack_writer_t *w) {
    DE__ED_PARAMS(spline_info, de_ep_spline_params, q);
    de_spline_ids ids;
    const DeSpline *s = spline_of(ed, w, req->id, q.entity, "spline.info", &ids);
    if (s == NULL)
        return;
    de_spline_poly *p = poly_of(ed, a, s, q.entity);
    if (p == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "spline.info: out of scratch memory");
        return;
    }
    static de_ep_str k_kinds[2];
    k_kinds[0] = de_ep_cstr("catmullRom");
    k_kinds[1] = de_ep_cstr("bezier");
    de_ep_spline_info r;
    memset(&r, 0, sizeof r);
    r.entity = q.entity;
    r.component = de_ep_cstr(DE_SPLINE_COMPONENT);
    r.mesh_component = de_ep_cstr(DE_SPLINE_MESHES_COMPONENT);
    r.kinds.items = k_kinds;
    r.kinds.count = 2;
    r.max_points = DE_SPLINE_MAX_POINTS;
    r.count = s->count < DE_SPLINE_MAX_POINTS ? s->count : DE_SPLINE_MAX_POINTS;
    r.length = de__spline_length(p);
    r.revision = de__ed_entity_rev(ed, (de_entity){q.entity});
    de_ep_write_response_spline_info(w, req->id, &r);
}

static void spline_sample(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                          mpack_writer_t *w) {
    DE__ED_PARAMS(spline_sample, de_ep_spline_sample_params, q);
    de_spline_ids ids;
    const DeSpline *s = spline_of(ed, w, req->id, q.entity, "spline.sample", &ids);
    if (s == NULL)
        return;
    if (!isfinite(q.step) || q.step < 0.01f) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "spline.sample: step must be a finite number of metres, at least 0.01");
        return;
    }
    de_spline_poly *p = poly_of(ed, a, s, q.entity);
    float *pos = (float *)de__ed_alloc(a, sizeof(float) * 3 * DE_SPLINE_MAX_OUT);
    float *dir = (float *)de__ed_alloc(a, sizeof(float) * 3 * DE_SPLINE_MAX_OUT);
    float *dist = (float *)de__ed_alloc(a, sizeof(float) * DE_SPLINE_MAX_OUT);
    if (p == NULL || pos == NULL || dir == NULL || dist == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "spline.sample: out of scratch memory");
        return;
    }
    const u32 n = de__spline_sample(p, q.step, pos, dir, dist);
    de_ep_spline_samples r;
    memset(&r, 0, sizeof r);
    r.revision = de__ed_entity_rev(ed, (de_entity){q.entity});
    r.length = de__spline_length(p);
    r.positions = (de_ep_list_f32){pos, 3 * n};
    r.tangents = (de_ep_list_f32){dir, 3 * n};
    r.distances = (de_ep_list_f32){dist, n};
    de_ep_write_response_spline_sample(w, req->id, &r);
}

static void spline_instances(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                             mpack_writer_t *w) {
    DE__ED_PARAMS(spline_instances, de_ep_spline_params, q);
    de_spline_ids ids;
    const DeSpline *s = spline_of(ed, w, req->id, q.entity, "spline.instances", &ids);
    if (s == NULL)
        return;
    de_ep_spline_instances r;
    memset(&r, 0, sizeof r);
    r.revision = de__ed_entity_rev(ed, (de_entity){q.entity});
    r.mesh = de_ep_cstr("");
    const DeSplineMeshes *m = (const DeSplineMeshes *)de_entity_get_const(
        world_of(ed), (de_entity){q.entity}, ids.meshes);
    if (m != NULL) {
        de_spline_poly *p = poly_of(ed, a, s, q.entity);
        float *xf = (float *)de__ed_alloc(a, sizeof(float) * 10 * DE_SPLINE_MAX_OUT);
        if (p == NULL || xf == NULL) {
            de__ed_error(w, req->id, DE_EP_ERR_BUSY, "spline.instances: out of scratch memory");
            return;
        }
        const char *path = m->mesh.id != 0 ? de__ed_asset_path(ed, m->mesh.id) : NULL;
        r.mesh = de_ep_cstr(path != NULL ? path : "");
        r.deform = m->deform;
        r.transforms = (de_ep_list_f32){xf, 10 * de__spline_place(p, m, xf)};
    }
    de_ep_write_response_spline_instances(w, req->id, &r);
}

void de__ed_spline_register(de__editor *ed) {
    de_spline_ids ids;
    (void)ids_of(world_of(ed), &ids); /* the IDE's reflection lists the components from the start */
    de__ed_on(ed, DE_EP_METHOD_SPLINE_INFO, spline_info);
    de__ed_on(ed, DE_EP_METHOD_SPLINE_SAMPLE, spline_sample);
    de__ed_on(ed, DE_EP_METHOD_SPLINE_INSTANCES, spline_instances);
    de__ed_add_capability(ed, "world.splines");
}
