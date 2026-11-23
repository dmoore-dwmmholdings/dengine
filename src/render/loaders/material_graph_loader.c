/*
 * material_graph_loader.c: the runtime `material_graph` type (DENG-770). The asset is a cooked
 * material graph (DMGR, render/formats/de_cooked_material_graph.h): the graph's model name, its
 * exposed parameter layout and its cooked resolve kernel.
 *
 * - load (any thread): checks the DMGR and keeps a copy.
 * - ready (main thread, before publish, and on hot reload before the swap): takes the kernel for
 *   the device's backend from the embedded DSHD and registers the material model under the
 *   graph's name (de__material_models_add_ex). When the name is a graph model already (a hot
 *   reload, or an unload and load again) it replaces that model's pipeline instead
 *   (de__material_models_replace): the model id stays, so every material keeps its model, and the
 *   old pipeline is destroyed after the frames that may use it (the retire list). A name that a
 *   non-graph model holds fails the load. Two graph assets with the same model name: the one made
 *   ready last wins.
 * - unload: frees the copy after the frame fence. The model stays registered: model ids are never
 *   removed (material_models.h).
 *
 * Parameter edits (de_material_set_params, the DMAT defaults) change only the material's GPU
 * block, never the pipeline.
 *
 * Without material models in the uploader (no renderer: tools and tests), ready publishes without
 * registering.
 */
#include "render/loaders/loaders_internal.h"

#include "gpu/shader/shader_internal.h"
#include "render/formats/de_cooked_material_graph.h"
#include "render/loaders/render_assets.h"
#include "render/materials/material_models.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>

#include <string.h>

/** The `material_graph` asset object. */
typedef struct de__render_material_graph {
    de__render_retire retire_;
    u8 *data; /**< the DMGR copy */
    u64 size;
    de_cooked_material_graph_view view; /**< into `data` */
} de__render_material_graph;

/** The entry point the importer compiles (mg_import.c). */
#define DE__MATERIAL_GRAPH_ENTRY "cs_resolve_model"

de_result de__material_graph_load(de_asset_load_ctx *ctx) {
    de_render_uploader *u = de__loader_uploader(ctx);
    if (u == NULL)
        return DE_ERR_INTERNAL;
    u64 size = 0;
    const void *p = de_asset_load_ctx_data(ctx, &size);
    de_cooked_material_graph_view v;
    const char *why = NULL;
    if (!de_cooked_material_graph_parse(p, size, &v, &why))
        return de_asset_load_ctx_fail(ctx, DE_ERR_PARSE, "not a cooked material graph: %s", why);
    de__render_material_graph *g = (de__render_material_graph *)DE_ALLOC(
        u->alloc, sizeof *g, DE_ALIGNOF(de__render_material_graph), DE_MEM_RENDER);
    u8 *copy = g != NULL ? (u8 *)DE_ALLOC(u->alloc, (size_t)size, 16, DE_MEM_RENDER) : NULL;
    if (copy == NULL) {
        if (g != NULL)
            DE_FREE(u->alloc, g, sizeof *g);
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "no memory for the graph");
    }
    memset(g, 0, sizeof *g);
    memcpy(copy, p, (size_t)size);
    g->data = copy;
    g->size = size;
    (void)de_cooked_material_graph_parse(copy, size, &g->view, NULL); /* checked above */
    de_asset_load_ctx_set_object(ctx, g, sizeof *g);
    return DE_OK;
}

static void material_graph_free(de_render_uploader *u, void *object) {
    de__render_material_graph *g = (de__render_material_graph *)object;
    DE_FREE(u->alloc, g->data, (size_t)g->size);
    DE_FREE(u->alloc, g, sizeof *g);
}

void de__material_graph_unload(void *object, void *user) {
    de_render_uploader *u = (de_render_uploader *)user;
    if (object == NULL)
        return;
    de__render_material_graph *g = (de__render_material_graph *)object;
    de__uploader_retire(u, &g->retire_, g, material_graph_free);
}

/** A replaced resolve pipeline on the retire list (destroyed after the frame fence). */
typedef struct retired_pipeline {
    de__render_retire node;
    de_gpu_pipeline pipeline;
} retired_pipeline;

static void retired_pipeline_free(de_render_uploader *u, void *object) {
    retired_pipeline *r = (retired_pipeline *)object;
    de_gpu_pipeline_destroy(u->dev, r->pipeline);
    DE_FREE(u->alloc, r, sizeof *r);
}

de_result de__material_graph_ready(void *object, void *user) {
    de_render_uploader *u = (de_render_uploader *)user;
    de__render_material_graph *g = (de__render_material_graph *)object;
    de__material_models *models = u->material_models;
    const char *name = g->view.model;
    if (models == NULL) {
        DE_INFO(u->log, "material graph \"%s\": no renderer, the model is not registered", name);
        return DE_OK;
    }
    de__shader_view sv;
    de_result r = de__shader_view_init(g->view.shader, g->view.shader_size, &sv);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "material graph \"%s\": its cooked shader", name);
    const de_gpu_caps *caps = de_gpu_get_caps(u->dev);
    de_gpu_shader_blob code;
    de_shader_reflection refl;
    r = de__shader_view_code(&sv, caps->backend, 0u, DE__MATERIAL_GRAPH_ENTRY, &code, &refl);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r,
                            "material graph \"%s\": no %s for this device (cook it with the "
                            "device's target)",
                            name, DE__MATERIAL_GRAPH_ENTRY);
    const u32 id = de__material_models_find(models, name);
    if (id == DE__MATERIAL_MODEL_MAX) {
        u32 added = 0;
        r = de__material_models_add_ex(models, name, &code, &refl, true, g->view.features, &added);
        if (r != DE_OK)
            return DE_FAIL_WRAP(r, "material graph \"%s\": registering its model", name);
    } else {
        if (!de__material_models_graph_features(models, id, NULL))
            return DE_FAIL(DE_ERR_ALREADY_EXISTS,
                           "material graph \"%s\": a material model that is not a graph has the "
                           "name",
                           name);
        retired_pipeline *old = (retired_pipeline *)DE_ALLOC(
            u->alloc, sizeof *old, DE_ALIGNOF(retired_pipeline), DE_MEM_RENDER);
        if (old == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "material graph \"%s\": out of memory", name);
        memset(old, 0, sizeof *old);
        r = de__material_models_replace(models, id, &code, &refl, g->view.features, &old->pipeline);
        if (r != DE_OK) {
            DE_FREE(u->alloc, old, sizeof *old);
            return DE_FAIL_WRAP(r, "material graph \"%s\": replacing its model", name);
        }
        if (DE_HANDLE_VALID(old->pipeline))
            de__uploader_retire(u, &old->node, old, retired_pipeline_free);
        else
            DE_FREE(u->alloc, old, sizeof *old);
    }
    de__uploader_materials_changed(u); /* the materials naming the model repack */
    return DE_OK;
}
