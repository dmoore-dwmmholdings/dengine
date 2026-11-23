/*
 * DENG-769: the material graph over the editor IPC (DIDE-R161; protocol 1.10, capability
 * assets.materialGraph). materialGraph.catalog returns the node catalog JSON,
 * materialGraph.generate turns a graph into Slang with its params and node/pin diagnostics, and
 * materialGraph.compile also compiles the module for 1-6 Slang targets, with Slang's errors
 * mapped back to nodes (tools/cook/importers/material_graph, DENG-579).
 *
 * Debug and Dev builds with the cooker and Slang only (DE_ED_MATERIAL_GRAPH; the host links
 * de_cooker there, tools/check_layers/layers.toml: host -> cook/importers). Elsewhere this file
 * is empty and the capability is not advertised.
 */
#include "host/editor/editor_internal.h"
#if DE_ED_MATERIAL_GRAPH
#include "cook/importers/material_graph/material_graph.h"
#include <dengine/de_memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_ED_ENGINE_SHADER_ROOT
#define DE_ED_ENGINE_SHADER_ROOT "shaders"
#endif

#define MG_GRAPH_MAX   ((u32)4 << 20) /* bytes of graph text in one request */
#define MG_TARGETS_MAX 6u
#define MG_IMPORTS_MAX 1024u

extern const de__slang_module de__shader_common_modules[];
extern const u32 de__shader_common_module_count;

/* ---------- engine shader imports ---------- */

typedef struct mg_imports {
    const char *root;
    void *data[MG_IMPORTS_MAX];
    u32 count;
} mg_imports;

/* Serves `import` for the generated module from the engine shader root. Relative paths below the
 * root only: no absolute paths, "..", backslashes or drive letters. */
static de_result mg_read(void *user, const char *path, const void **data, u64 *size) {
    mg_imports *im = user;
    if (!path || !path[0] || path[0] == '/' || strchr(path, '\\') || strchr(path, ':') ||
        strstr(path, ".."))
        return DE_ERR_NOT_FOUND;
    if (im->count == MG_IMPORTS_MAX)
        return DE_ERR_OUT_OF_MEMORY;
    char full[2048];
    const int n = snprintf(full, sizeof full, "%s/%s", im->root, path);
    if (n < 0 || (size_t)n >= sizeof full)
        return DE_ERR_NOT_FOUND;
    FILE *file = fopen(full, "rb");
    if (!file)
        return DE_ERR_NOT_FOUND;
    long length = -1;
    if (fseek(file, 0, SEEK_END) == 0)
        length = ftell(file);
    if (length < 0 || length > (long)(64 << 20) || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return DE_ERR_IO;
    }
    char *bytes = malloc((size_t)length + 1);
    if (!bytes) {
        fclose(file);
        return DE_ERR_OUT_OF_MEMORY;
    }
    const size_t got = fread(bytes, 1, (size_t)length, file);
    fclose(file);
    if (got != (size_t)length) {
        free(bytes);
        return DE_ERR_IO;
    }
    bytes[length] = 0;
    im->data[im->count++] = bytes;
    *data = bytes;
    *size = (u64)length;
    return DE_OK;
}

static void mg_imports_free(mg_imports *im) {
    for (u32 i = 0; i < im->count; ++i)
        free(im->data[i]);
    im->count = 0;
}

static const char *mg_shader_root(void) {
    const char *env = getenv("DE_ENGINE_SHADER_ROOT"); /* as the renderer (engine_shaders.c) */
    return env && env[0] ? env : DE_ED_ENGINE_SHADER_ROOT;
}

/* ---------- result conversion ---------- */

static bool mg_diags(de_ep_arena *a, const de__mg_result *g, u32 first, u32 end,
                     de_ep_list_material_graph_diagnostic *out) {
    out->items = NULL;
    out->count = end > first ? end - first : 0;
    if (!out->count)
        return true;
    out->items = de__ed_alloc(a, sizeof *out->items * out->count);
    if (!out->items)
        return false;
    for (u32 i = 0; i < out->count; ++i) {
        const de__mg_diag *s = &g->diags[first + i];
        de_ep_material_graph_diagnostic *d = &out->items[i];
        memset(d, 0, sizeof *d);
        d->severity = s->severity == DE__MG_WARNING ? DE_EP_MATERIAL_GRAPH_SEVERITY_WARNING
                                                    : DE_EP_MATERIAL_GRAPH_SEVERITY_ERROR;
        d->node = de_ep_cstr(s->node);
        d->pin = de_ep_cstr(s->pin);
        d->message = de_ep_cstr(s->message);
        d->has_line = s->graph_line > 0;
        d->line = s->graph_line;
    }
    return true;
}

/* `diag_end` and `ok` are as of generation: a compile appends Slang's diagnostics to `g`. */
static bool mg_generated(de_ep_arena *a, const de__mg_result *g, u32 diag_end, bool ok,
                         de_ep_material_graph_generated *out) {
    memset(out, 0, sizeof *out);
    out->ok = ok;
    out->has_source = g->source != NULL;
    if (out->has_source)
        out->source = (de_ep_str){g->source, (uint32_t)g->source_size};
    out->features = g->features;
    if (g->param_count) {
        out->params.items = de__ed_alloc(a, sizeof *out->params.items * g->param_count);
        if (!out->params.items)
            return false;
        out->params.count = g->param_count;
        for (u32 i = 0; i < g->param_count; ++i) {
            const de__mg_param *s = &g->params[i];
            de_ep_material_graph_param *p = &out->params.items[i];
            memset(p, 0, sizeof *p);
            p->kind = s->kind == DE__MG_PARAM_COLOR     ? DE_EP_MATERIAL_GRAPH_PARAM_KIND_COLOR
                      : s->kind == DE__MG_PARAM_TEXTURE ? DE_EP_MATERIAL_GRAPH_PARAM_KIND_TEXTURE
                                                        : DE_EP_MATERIAL_GRAPH_PARAM_KIND_FLOAT;
            p->name = de_ep_cstr(s->name);
            p->node = de_ep_cstr(s->node);
            p->row = s->row;
            p->component = s->component;
            p->slot = s->slot;
            p->value.items = (float *)s->value;
            p->value.count = s->kind == DE__MG_PARAM_COLOR     ? 3u
                             : s->kind == DE__MG_PARAM_TEXTURE ? 0u
                                                               : 1u;
            p->min = s->min;
            p->max = s->max;
        }
    }
    return mg_diags(a, g, 0, diag_end, &out->diagnostics);
}

/* Validates the graph text and runs the generator. Writes the error reply and returns false when
 * the request fails (the result needs no free then). */
static bool mg_generate(const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w,
                        de_ep_str graph, bool has_name, de_ep_str name, de__mg_result *g) {
    memset(g, 0, sizeof *g);
    if (graph.len == 0 || graph.len > MG_GRAPH_MAX) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "materialGraph: graph must be 1..%u bytes", (unsigned)MG_GRAPH_MAX);
        return false;
    }
    const char *cname = has_name ? de__ed_cstr(a, name) : NULL;
    if (has_name && !cname) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "materialGraph: request too large");
        return false;
    }
    const de_result r = de__mg_generate(&(de__mg_desc){.graph = graph.ptr,
                                                       .graph_size = graph.len,
                                                       .name = cname,
                                                       .allocator = de_heap_allocator()},
                                        g);
    if (r != DE_OK && r != DE_ERR_PARSE) {
        de__mg_result_free(g);
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "materialGraph: generator failed (%s)",
                     de_result_str(r));
        return false;
    }
    return true;
}

/* ---------- handlers ---------- */

static void mg_catalog(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                       mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(material_graph_catalog, de_ep_empty, p);
    (void)p;
    de_strbuf b;
    de_strbuf_init(&b, de_heap_allocator());
    if (de__mg_catalog_json(&b) != DE_OK) {
        de_strbuf_free(&b);
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "materialGraph.catalog: out of memory");
        return;
    }
    const de_str json = de_strbuf_view(&b);
    const de_ep_material_graph_catalog c = {.json = {json.ptr ? json.ptr : "", (uint32_t)json.len}};
    de_ep_write_response_material_graph_catalog(w, req->id, &c);
    de_strbuf_free(&b);
}

static void mg_generate_handler(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                                mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(material_graph_generate, de_ep_material_graph_params, p);
    de__mg_result g;
    if (!mg_generate(req, a, w, p.graph, p.has_name, p.name, &g))
        return;
    de_ep_material_graph_generated out;
    if (!mg_generated(a, &g, g.diag_count, g.error_count == 0 && g.source != NULL, &out))
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "materialGraph.generate: reply too large");
    else
        de_ep_write_response_material_graph_generate(w, req->id, &out);
    de__mg_result_free(&g);
}

static bool mg_target_parse(de_ep_str s, de__slang_target *out) {
    for (u32 t = 0; t < (u32)DE__SLANG_TARGET_COUNT_; ++t) {
        if (de_ep_str_eq(s, de__slang_target_name((de__slang_target)t))) {
            *out = (de__slang_target)t;
            return true;
        }
    }
    return false;
}

static void mg_compile_handler(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                               mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(material_graph_compile, de_ep_material_graph_compile_params, p);
    de__slang_target targets[MG_TARGETS_MAX];
    if (p.targets.count == 0 || p.targets.count > MG_TARGETS_MAX) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "materialGraph.compile: give 1..%u targets", MG_TARGETS_MAX);
        return;
    }
    for (u32 i = 0; i < p.targets.count; ++i) {
        if (!mg_target_parse(p.targets.items[i], &targets[i])) {
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                         "materialGraph.compile: unknown target '%.*s'",
                         (int)(p.targets.items[i].len > 32 ? 32 : p.targets.items[i].len),
                         p.targets.items[i].ptr);
            return;
        }
        for (u32 j = 0; j < i; ++j) {
            if (targets[j] == targets[i]) {
                de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                             "materialGraph.compile: duplicate target '%s'",
                             de__slang_target_name(targets[i]));
                return;
            }
        }
    }
    de__mg_result g;
    if (!mg_generate(req, a, w, p.graph, p.has_name, p.name, &g))
        return;
    const u32 generated_diags = g.diag_count;
    de_ep_material_graph_compiled out;
    memset(&out, 0, sizeof out);
    const bool ok = g.error_count == 0 && g.source != NULL;
    u32 first[MG_TARGETS_MAX] = {0}, end[MG_TARGETS_MAX] = {0};
    de_ep_material_graph_target_result *results = NULL;
    if (ok) {
        results = de__ed_alloc(a, sizeof *results * p.targets.count);
        if (!results)
            goto too_large;
        memset(results, 0, sizeof *results * p.targets.count);
        char why[256] = {0};
        const bool slang = de__slang_available(why, sizeof why);
        mg_imports im = {.root = mg_shader_root()};
        for (u32 i = 0; i < p.targets.count; ++i) {
            de_ep_material_graph_target_result *tr = &results[i];
            tr->target = de_ep_cstr(de__slang_target_name(targets[i]));
            first[i] = end[i] = g.diag_count;
            if (!slang || !de__slang_target_supported(targets[i])) {
                tr->status = DE_EP_MATERIAL_GRAPH_TARGET_STATUS_UNSUPPORTED;
                const char *msg = !slang ? why : "the Slang build has no backend for this target";
                tr->message = de_ep_cstr(de__ed_cstr(a, de_ep_cstr(msg)));
                tr->has_message = tr->message.ptr != NULL;
                continue;
            }
            de__slang_compile_result cr;
            memset(&cr, 0, sizeof cr);
            const de_result r = de__mg_compile(
                &g,
                &(de__mg_compile_desc){.target = targets[i],
                                       .path = "material_graph.slang",
                                       .read = mg_read,
                                       .read_user = &im,
                                       .modules = de__shader_common_modules,
                                       .module_count = de__shader_common_module_count,
                                       .preload_count = 1},
                &cr);
            mg_imports_free(&im);
            end[i] = g.diag_count;
            if (r == DE_OK) {
                tr->status = DE_EP_MATERIAL_GRAPH_TARGET_STATUS_OK;
                u64 size = 0;
                for (u32 e = 0; e < cr.entry_count; ++e)
                    size += cr.entries[e].code_size;
                tr->has_code_size = true;
                tr->code_size = size;
            } else {
                tr->status = r == DE_ERR_UNSUPPORTED
                                 ? DE_EP_MATERIAL_GRAPH_TARGET_STATUS_UNSUPPORTED
                                 : DE_EP_MATERIAL_GRAPH_TARGET_STATUS_ERROR;
            }
            if (cr.diagnostics[0]) {
                const char *msg = de__ed_cstr(a, de_ep_cstr(cr.diagnostics));
                tr->has_message = msg != NULL;
                if (msg)
                    tr->message = de_ep_cstr(msg);
            }
            de__slang_compile_result_free(&cr);
        }
    }
    if (!mg_generated(a, &g, generated_diags, ok, &out.generated))
        goto too_large;
    if (ok) {
        for (u32 i = 0; i < p.targets.count; ++i)
            if (!mg_diags(a, &g, first[i], end[i], &results[i].diagnostics))
                goto too_large;
        out.targets.items = results;
        out.targets.count = p.targets.count;
    }
    de_ep_write_response_material_graph_compile(w, req->id, &out);
    de__mg_result_free(&g);
    return;
too_large:
    de__mg_result_free(&g);
    de__ed_error(w, req->id, DE_EP_ERR_BUSY, "materialGraph.compile: reply too large");
}

void de__ed_material_graph_register(de__editor *ed) {
    de__ed_on(ed, DE_EP_METHOD_MATERIAL_GRAPH_CATALOG, mg_catalog);
    de__ed_on(ed, DE_EP_METHOD_MATERIAL_GRAPH_GENERATE, mg_generate_handler);
    de__ed_on(ed, DE_EP_METHOD_MATERIAL_GRAPH_COMPILE, mg_compile_handler);
    de__ed_add_capability(ed, "assets.materialGraph");
}

#endif /* DE_ED_MATERIAL_GRAPH */
