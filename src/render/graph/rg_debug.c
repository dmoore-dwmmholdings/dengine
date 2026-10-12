/*
 * rg_debug.c: render graph names for messages, the Graphviz dump and the r.rg_dump cvar
 * (DENG-171). Dev tooling: nothing here runs in a normal frame unless r.rg_dump is set.
 *
 * Render a dump with `dot -Tsvg rg.dot -o rg.svg`.
 */
#include "render/graph/rg_internal.h"

#include <dengine/de_platform.h>

#include <stdio.h>
#include <string.h>

const char *de__rg_queue_name(de_gpu_queue q) {
    switch (q) {
    case DE_QUEUE_GRAPHICS:
        return "graphics";
    case DE_QUEUE_COMPUTE:
        return "compute";
    case DE_QUEUE_COPY:
        return "copy";
    default:
        return "?";
    }
}

const char *de__rg_access_str(u32 access, char *buf, size_t cap) {
    static const char *const names[] = {
        "SHADER_READ", "SHADER_WRITE", "RENDER_TARGET", "DEPTH_READ",  "DEPTH_WRITE", "INDIRECT",
        "COPY_SRC",    "COPY_DST",     "ACCEL_READ",    "ACCEL_WRITE", "PRESENT",
    };
    if (!buf || cap == 0)
        return "";
    buf[0] = '\0';
    if (access == 0) {
        snprintf(buf, cap, "NONE");
        return buf;
    }
    size_t len = 0;
    for (u32 bit = 0; bit < 32u; ++bit) {
        if (!(access & (1u << bit)))
            continue;
        char unknown[16];
        const char *n = bit < sizeof names / sizeof names[0] ? names[bit] : unknown;
        if (n == unknown)
            snprintf(unknown, sizeof unknown, "0x%x", 1u << bit);
        int w = snprintf(buf + len, cap - len, "%s%s", len ? "|" : "", n);
        if (w < 0 || (size_t)w >= cap - len)
            break; /* cut; buf stays NUL-terminated */
        len += (size_t)w;
    }
    return buf;
}

/* ---------- Graphviz ---------- */

/* Appends `s` inside a double-quoted dot string. */
static de_result dot_str(de_strbuf *out, const char *s) {
    DE_TRY(de_strbuf_append_char(out, '"'));
    for (; s && *s; ++s) {
        if (*s == '"' || *s == '\\')
            DE_TRY(de_strbuf_append_char(out, '\\'));
        if (*s == '\n') {
            DE_TRY(de_strbuf_append_cstr(out, "\\n"));
            continue;
        }
        DE_TRY(de_strbuf_append_char(out, *s));
    }
    return de_strbuf_append_char(out, '"');
}

static bool range_is_whole(de__rg_range r) {
    return r.mip_first == 0 && r.mip_end == UINT32_MAX && r.layer_first == 0 &&
           r.layer_end == UINT32_MAX;
}

/* " mips 1-1 layers 0-5" for a partial range, "" otherwise. */
static const char *range_str(const de__rg_resource *res, de__rg_range r, char *buf, size_t cap) {
    buf[0] = '\0';
    if (res->kind != DE__RG_TEXTURE || range_is_whole(r))
        return buf;
    char mips[32] = "", layers[32] = "";
    if (r.mip_first != 0 || r.mip_end != UINT32_MAX) {
        if (r.mip_end == UINT32_MAX)
            snprintf(mips, sizeof mips, " mips %u-last", r.mip_first);
        else
            snprintf(mips, sizeof mips, " mips %u-%u", r.mip_first, r.mip_end - 1u);
    }
    if (r.layer_first != 0 || r.layer_end != UINT32_MAX) {
        if (r.layer_end == UINT32_MAX)
            snprintf(layers, sizeof layers, " layers %u-last", r.layer_first);
        else
            snprintf(layers, sizeof layers, " layers %u-%u", r.layer_first, r.layer_end - 1u);
    }
    snprintf(buf, cap, "%s%s", mips, layers);
    return buf;
}

static const char *const k_live_fill[DE_QUEUE_COUNT_] = {"#cfe2ff", "#d1e7dd", "#fff3cd"};
#define DOT_CULLED_FILL "#eeeeee"
#define DOT_CULLED_TEXT "#999999"

static de_result dot_passes(const de_rg *g, de_strbuf *out) {
    char label[256];
    for (u32 pi = 0; pi < g->pass_count; ++pi) {
        const de_rg_pass *p = g->passes[pi];
        snprintf(label, sizeof label, "%u: %s\n%s%s%s", pi, p->name, de__rg_queue_name(p->queue),
                 p->side_effect ? ", side effect" : "", p->culled ? "\nculled" : "");
        DE_TRY(de_strbuf_appendf(out, "  p%u [shape=box, label=", pi));
        DE_TRY(dot_str(out, label));
        if (p->culled)
            DE_TRY(de_strbuf_appendf(out,
                                     ", style=\"rounded,filled,dashed\", fillcolor=\"%s\", "
                                     "fontcolor=\"%s\", color=\"%s\"];\n",
                                     DOT_CULLED_FILL, DOT_CULLED_TEXT, DOT_CULLED_TEXT));
        else
            DE_TRY(de_strbuf_appendf(out, ", style=\"rounded,filled\", fillcolor=\"%s\"];\n",
                                     k_live_fill[p->queue < DE_QUEUE_COUNT_ ? p->queue : 0]));
    }
    return DE_OK;
}

/* One node per resource version: v0 (the contents at import) when something reads it, and one
 * per write. A version whose writer is culled is grey. */
static de_result dot_versions(const de_rg *g, de_strbuf *out) {
    char label[256], sub[64];
    for (u32 ri = 0; ri < g->resource_count; ++ri) {
        const de__rg_resource *r = &g->resources[ri];
        const bool imported = (r->flags & DE__RG_RES_IMPORTED) != 0;
        const bool exported = (r->flags & DE__RG_RES_EXPORTED) != 0;
        const char *kind = r->kind == DE__RG_TEXTURE ? "texture" : "buffer";
        const char *origin = imported ? "imported" : "transient";
        if (r->access_first == DE__RG_NONE) {
            snprintf(label, sizeof label, "%s\n%s %s, unused", r->name, origin, kind);
            DE_TRY(de_strbuf_appendf(out, "  r%u [shape=ellipse, label=", ri));
            DE_TRY(dot_str(out, label));
            DE_TRY(de_strbuf_appendf(out, ", style=dashed, fontcolor=\"%s\", color=\"%s\"];\n",
                                     DOT_CULLED_TEXT, DOT_CULLED_TEXT));
            continue;
        }
        if (!g->accesses[r->access_first].write) {
            snprintf(label, sizeof label, "%s v0\n%s %s", r->name, origin, kind);
            DE_TRY(de_strbuf_appendf(out, "  r%u_v0 [shape=ellipse, label=", ri));
            DE_TRY(dot_str(out, label));
            DE_TRY(de_strbuf_appendf(out, "%s];\n", imported ? ", style=bold" : ""));
        }
        for (u32 a = r->access_first; a != DE__RG_NONE; a = g->accesses[a].res_next) {
            const de__rg_access *x = &g->accesses[a];
            if (!x->write)
                continue;
            const bool live = !g->passes[x->pass]->culled;
            const bool final = x->version == r->version_count;
            snprintf(label, sizeof label, "%s v%u%s\n%s %s%s", r->name, x->version,
                     range_str(r, x->range, sub, sizeof sub), origin, kind,
                     final && exported ? ", exported" : "");
            DE_TRY(de_strbuf_appendf(out, "  r%u_v%u [shape=ellipse, label=", ri, x->version));
            DE_TRY(dot_str(out, label));
            if (!live)
                DE_TRY(de_strbuf_appendf(out, ", style=dashed, fontcolor=\"%s\", color=\"%s\"",
                                         DOT_CULLED_TEXT, DOT_CULLED_TEXT));
            else if (final && (exported || imported))
                DE_TRY(de_strbuf_append_cstr(out, ", peripheries=2"));
            DE_TRY(de_strbuf_append_cstr(out, "];\n"));
        }
    }
    return DE_OK;
}

static de_result dot_accesses(const de_rg *g, de_strbuf *out) {
    char bits[160], sub[64], label[256];
    for (u32 a = 0; a < g->access_count; ++a) {
        const de__rg_access *x = &g->accesses[a];
        const de__rg_resource *r = &g->resources[x->resource];
        const bool culled = g->passes[x->pass]->culled;
        snprintf(label, sizeof label, "%s%s", de__rg_access_str(x->access, bits, sizeof bits),
                 range_str(r, x->range, sub, sizeof sub));
        if (x->write)
            DE_TRY(de_strbuf_appendf(out, "  p%u -> r%u_v%u [label=", x->pass, x->resource,
                                     x->version));
        else
            DE_TRY(de_strbuf_appendf(out, "  r%u_v%u -> p%u [label=", x->resource, x->version,
                                     x->pass));
        DE_TRY(dot_str(out, label));
        if (culled)
            DE_TRY(de_strbuf_appendf(out, ", style=dashed, color=\"%s\", fontcolor=\"%s\"",
                                     DOT_CULLED_TEXT, DOT_CULLED_TEXT));
        DE_TRY(de_strbuf_append_cstr(out, "];\n"));
    }
    /* Ordering edges that the resource nodes do not show (RAW is the version node path). */
    for (u32 e = 0; e < g->edge_count; ++e) {
        const de__rg_edge *x = &g->edges[e];
        if (x->kind == DE__RG_EDGE_RAW)
            continue;
        DE_TRY(de_strbuf_appendf(
            out, "  p%u -> p%u [style=dotted, constraint=false, label=\"%s %s\"];\n", x->from,
            x->to, x->kind == DE__RG_EDGE_WAR ? "WAR" : "WAW", g->resources[x->resource].name));
    }
    return DE_OK;
}

de_result de__rg_dump_dot(const de_rg *g, de_strbuf *out) {
    if (!g || !out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__rg_dump_dot: %s is NULL",
                       g ? "out" : "the graph");
    if (!g->finalized)
        return DE_FAIL(DE_ERR_BUSY, "de__rg_dump_dot(\"%s\"): finalize the graph first", g->name);
    DE_TRY(de_strbuf_append_cstr(out, "digraph "));
    DE_TRY(dot_str(out, g->name));
    DE_TRY(de_strbuf_appendf(out,
                             " {\n"
                             "  // %u passes (%u culled), %u resources, %u accesses, %u edges\n"
                             "  rankdir=LR;\n"
                             "  node [fontname=\"Helvetica\", fontsize=10];\n"
                             "  edge [fontname=\"Helvetica\", fontsize=9];\n",
                             g->pass_count, g->culled_count, g->resource_count, g->access_count,
                             g->edge_count));
    DE_TRY(dot_passes(g, out));
    DE_TRY(dot_versions(g, out));
    DE_TRY(dot_accesses(g, out));
    return de_strbuf_append_cstr(out, "}\n");
}

de_result de__rg_dump_dot_file(const de_rg *g, const char *vpath) {
    if (!g || !vpath)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__rg_dump_dot_file: %s is NULL",
                       g ? "vpath" : "the graph");
    de_strbuf b;
    de_strbuf_init(&b, g->alloc);
    b.tag = DE_MEM_RENDER;
    de_result r = de__rg_dump_dot(g, &b);
    if (r == DE_OK) {
        de_str v = de_strbuf_view(&b);
        r = de_file_write_all_atomic(vpath, v.ptr, v.len);
    }
    de_strbuf_free(&b);
    return r;
}

void de__rg_dump_if_requested(de_rg *g) {
    if (!g || !DE_HANDLE_VALID(g->dump_cvar))
        return;
    const char *path = de_cvar_string(g->dump_cvar);
    if (!path || !path[0])
        return;
    if (de__rg_dump_dot_file(g, path) == DE_OK)
        DE_INFO(g->log, "render graph \"%s\" written to %s (%u passes, %u culled)", g->name, path,
                g->pass_count, g->culled_count);
    else
        DE_WARN(g->log, "render graph \"%s\": r.rg_dump could not write %s: %s", g->name, path,
                de_error_last() ? de_error_last()->message : "unknown error");
    /* One dump per request. A failed set leaves the cvar as it was; the next frame retries. */
    (void)de_cvar_set(g->dump_cvar, "");
}
