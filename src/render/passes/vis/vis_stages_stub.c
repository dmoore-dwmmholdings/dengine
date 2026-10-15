/*
 * vis_stages_stub.c: no-op stages of the visibility-buffer frame (DENG-229). Each later S13 task
 * deletes its own stub functions here (its init, shutdown and stage) and implements them in its
 * own file; vis_stages.h has the contract. With every stage stubbed, a view graph holds only the
 * instance-culling passes (cull_instances.c), and the render graph culls those that nothing reads.
 *
 *   cull_meshlets  DENG-231        raster        DENG-234 (mesh shaders), DENG-235 (compute)
 *   hzb_build      DENG-233        raster_alpha  DENG-236
 *   classify       DENG-237        resolve       DENG-238
 */
#include "render/passes/vis/vis_stages.h"

/* ---------- raster (DENG-234, DENG-235) ---------- */

de_result de__vis_raster_init(de__vis_shared *s) {
    (void)s;
    return DE_OK;
}
void de__vis_raster_shutdown(de__vis_shared *s) {
    (void)s;
}
void de__vis_raster(de_rg *g, de__vis_context *vc, de__vis_phase phase) {
    (void)g;
    (void)vc;
    (void)phase;
}

/* ---------- hzb_build (DENG-233) ---------- */

de_result de__vis_hzb_build_init(de__vis_shared *s) {
    (void)s;
    return DE_OK;
}
void de__vis_hzb_build_shutdown(de__vis_shared *s) {
    (void)s;
}
void de__vis_hzb_build(de_rg *g, de__vis_context *vc) {
    (void)g;
    (void)vc;
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
