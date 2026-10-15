/*
 * vis_stages_stub.c: no-op stages of the visibility-buffer frame (DENG-229). Each later S13 task
 * deletes its own stub functions here (its init, shutdown and stage) and implements them in its
 * own file; vis_stages.h has the contract. With every stage stubbed, a view graph holds only the
 * instance-culling passes (cull_instances.c), and the render graph culls those that nothing reads.
 *
 *   cull_meshlets  DENG-231        raster        DENG-234 (mesh shaders), DENG-235 (compute):
 *                                                the path switch below stays here
 *   hzb_build      DENG-233        raster_alpha  DENG-236
 *   classify       DENG-237        resolve       DENG-238
 */
#include "render/passes/vis/vis_stages.h"

/* ---------- raster: the path switch (DENG-234 mesh shaders, DENG-235 compute expansion) -------
 * The raster stage is not a stub: it dispatches to the path de__vis_raster_choose picks
 * (r.raster_path, de_gpu_caps.mesh_shaders). The mesh path's init owns the shared state (the
 * cvars, the vis-ID clear, de__vis_raster_targets); a path adds its init / shutdown line and one
 * `case` below. */

de_result de__vis_raster_init(de__vis_shared *s) {
    DE_TRY(de__vis_raster_mesh_init(s)); /* DENG-234 */
    return DE_OK;
}
void de__vis_raster_shutdown(de__vis_shared *s) {
    de__vis_raster_mesh_shutdown(s); /* DENG-234 */
}
void de__vis_raster(de_rg *g, de__vis_context *vc, de__vis_phase phase) {
    if (phase == DE__VIS_PHASE_1)
        vc->raster_path = (u32)de__vis_raster_choose(vc->shared);
    switch ((de__vis_raster_path)vc->raster_path) {
    case DE__VIS_RASTER_MESH:
        de__vis_raster_mesh(g, vc, phase);
        break;
    default: /* DE__VIS_RASTER_NONE; DE__VIS_RASTER_COMPUTE until DENG-235 adds its case */
        break;
    }
}

/* ---------- raster_alpha (DENG-236) ---------- */

de_result de__vis_raster_alpha_init(de__vis_shared *s) {
    (void)s;
    return DE_OK;
}
void de__vis_raster_alpha_shutdown(de__vis_shared *s) {
    (void)s;
}
void de__vis_raster_alpha(de_rg *g, de__vis_context *vc) {
    (void)g;
    (void)vc;
}

/* ---------- classify (DENG-237) ---------- */

de_result de__vis_classify_init(de__vis_shared *s) {
    (void)s;
    return DE_OK;
}
void de__vis_classify_shutdown(de__vis_shared *s) {
    (void)s;
}
void de__vis_classify(de_rg *g, de__vis_context *vc) {
    (void)g;
    (void)vc;
}

/* ---------- resolve (DENG-238) ---------- */

de_result de__vis_resolve_init(de__vis_shared *s) {
    (void)s;
    return DE_OK;
}
void de__vis_resolve_shutdown(de__vis_shared *s) {
    (void)s;
}
void de__vis_resolve(de_rg *g, de__vis_context *vc) {
    (void)g;
    (void)vc;
}
