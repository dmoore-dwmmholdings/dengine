/*
 * tools/cook/importers/material_graph/material_graph.h: material graph to Slang code generation
 * (DENG-579, SPEC §12.6, DIDE-R161). Dev and tools only; nothing here is public ABI.
 *
 * A material graph is the DIDE graph file (TOML, `kind = "material"`, `version = 1`): `[[node]]`
 * entries with an id ("n12"), a type from the catalog below, `values` (constants and the values of
 * unconnected input pins), an optional nested `graph` for a `subgraph` node, and `[[link]]`
 * entries ("n1.out" to "n2.a"). The generator checks the graph, then writes one Slang module that
 * implements IMaterialModel: resolve_gbuffer comes from the graph, the BSDF is the standard
 * model's, and the module defines the cs_resolve_model kernel that de_material_model_register
 * expects. Every problem is a diagnostic with the node id (subgraph members as "n4/n2") and pin
 * that caused it. Errors from the Slang compiler are mapped back to node ids through the line map.
 *
 * Exposed parameters (param.float, param.color) are packed into the ten float4 rows of
 * DeGpuMaterial after `alpha_mode` (base_color = row 0 ... subsurface = row 9) in node-id order,
 * first fit, never across rows. Named textures take the 19 texture slots in the same order; a
 * name used twice shares its slot. The generated model reads those rows and slots raw.
 *
 * Output is deterministic: nodes are emitted in dependency order, ties broken by node id, so the
 * order of entries in the file does not change the source.
 */
#ifndef DE_COOK_MATERIAL_GRAPH_H
#define DE_COOK_MATERIAL_GRAPH_H

#include "core/de_strbuf.h"
#include "cook/importers/shader/slang_compile.h"

DE_EXTERN_C_BEGIN

/** Limits of one graph (subgraph members included). Larger graphs get a diagnostic. */
#define DE__MG_MAX_NODES     4096u
#define DE__MG_MAX_LINKS     16384u
#define DE__MG_MAX_DEPTH     8u /**< subgraph nesting */
#define DE__MG_TEXTURE_SLOTS 19u
#define DE__MG_PARAM_ROWS    10u

typedef enum de__mg_severity {
    DE__MG_ERROR = 0,
    DE__MG_WARNING = 1,
} de__mg_severity;

/** One problem. `node` is "" for the graph as a whole and for lines no node produced. */
typedef struct de__mg_diag {
    de__mg_severity severity;
    char node[64];     /**< node path: "n3", or "n4/n2" for member n2 of subgraph n4 */
    char pin[32];      /**< the pin, or "" */
    char message[256]; /**< plain text, NUL terminated */
    u32 graph_line;    /**< 1-based line in the graph file; 0 unknown */
} de__mg_diag;

typedef enum de__mg_param_kind {
    DE__MG_PARAM_FLOAT = 0,   /**< one component of a row */
    DE__MG_PARAM_COLOR = 1,   /**< three components (xyz) of a row */
    DE__MG_PARAM_TEXTURE = 2, /**< a texture slot */
} de__mg_param_kind;

/** Where an exposed parameter lives in DeGpuMaterial, and its default. */
typedef struct de__mg_param {
    de__mg_param_kind kind;
    char name[64];
    char node[64]; /**< the first node that declares it */
    u32 row;       /**< float/color: float4 row, 0 (base_color) .. 9 (subsurface) */
    u32 component; /**< float/color: first component, 0..3 */
    u32 slot;      /**< texture: slot 0..18 */
    float value[3];
    float min, max; /**< float: the UI range (0..1 unless the node says otherwise) */
} de__mg_param;

typedef struct de__mg_desc {
    const char *graph; /**< the graph file; need not be NUL terminated */
    u64 graph_size;
    const char *name; /**< for the header comment only ("rock_moss"); may be NULL */
    const de_allocator *allocator;
} de__mg_desc;

typedef struct de__mg_result {
    char *source; /**< the Slang module, NUL terminated; NULL when the graph has errors */
    u64 source_size;
    de__mg_param *params;
    u32 param_count;
    de__mg_diag *diags;
    u32 diag_count;
    u32 error_count;
    u32 features; /**< DE_MATERIAL_FEATURE_* bits the layer outputs set */

    /* Private: line map and node paths. */
    u32 *line_node_; /**< per source line: node index + 1, 0 for none */
    u32 line_count_;
    char **node_path_;
    u32 node_count_;
    u32 diag_cap_;
    const de_allocator *alloc_;
} de__mg_result;

/**
 * Checks `d->graph` and writes its Slang module into `out`. Free `out` with de__mg_result_free
 * whatever this returns (the diagnostics are allocated on failure too).
 * @return DE_OK (no errors; warnings possible); DE_ERR_PARSE (the graph has errors: see
 *         out->diags); DE_ERR_INVALID_ARGUMENT; DE_ERR_OUT_OF_MEMORY.
 */
de_result de__mg_generate(const de__mg_desc *d, de__mg_result *out);

typedef struct de__mg_compile_desc {
    de__slang_target target;
    const char *path; /**< the module's path for Slang ("material_graph.slang" when NULL) */
    de__slang_read_fn read;
    void *read_user;
    const de__slang_module *modules;
    u32 module_count;
    u32 preload_count;
} de__mg_compile_desc;

/**
 * Compiles the generated module's cs_resolve_model with de__slang_compile. When Slang reports
 * errors or warnings in the module, each becomes a diagnostic in `g` with the node whose line it
 * names ("" for generated boilerplate). `out` is as for de__slang_compile.
 * @return de__slang_compile's result; DE_ERR_INVALID_ARGUMENT when `g` has no source.
 */
de_result de__mg_compile(de__mg_result *g, const de__mg_compile_desc *d,
                         de__slang_compile_result *out);

/** The node path that produced 1-based `line` of the source, or NULL for boilerplate. */
const char *de__mg_node_at_line(const de__mg_result *g, u32 line);

/** Appends the node catalog in the DIDE Kind JSON shape ({kind, conversions, types}). */
de_result de__mg_catalog_json(de_strbuf *out);

/** Frees everything `r` holds and zeroes it. A zeroed result is ignored. */
void de__mg_result_free(de__mg_result *r);

DE_EXTERN_C_END

#endif
