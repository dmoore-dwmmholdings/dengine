/* editor_assets.c: assets, the cook cache and model queries (DENG-629). */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* snprintf */
#endif
#include "host/editor/editor_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_text.h>

#include "asset/asset_internal.h"
#include "asset/cook_stage_internal.h"
#include "asset/hot_reload_internal.h"
#include "asset/manifest_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"
#include "host/host_internal.h"
#if DE_HAS_RENDER
#include "render/formats/de_cooked_model.h"
#include "render/loaders/render_assets.h"
#include "host/editor/editor_model_inspection.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ED_ASSET_PAGE 2000u

#define ED_LOADS 32u

typedef struct de__ed_assets {
    de_event_reader events;
    bool has_reader;
    /* Loads asset.reimport started for entries new to the manifest; released once final. */
    de_load_handle loads[ED_LOADS];
    de_asset_id load_ids[ED_LOADS];
    u32 load_count;
} de__ed_assets;

static de__ed_assets g_assets;

static de_asset_system *S(de__editor *ed) {
    return ed->e->assets;
}

static const de__manifest *manifest(de__editor *ed) {
    de_asset_system *s = S(ed);
    return s != NULL && s->resolved != NULL ? s->resolved->manifest : NULL;
}

static const char *state_name(de_load_state st) {
    switch (st) {
    case DE_LOAD_PENDING:
    case DE_LOAD_LOADING:
        return "loading";
    case DE_LOAD_READY:
        return "loaded";
    case DE_LOAD_PARTIAL:
    case DE_LOAD_FAILED:
        return "failed";
    default:
        return "unloaded";
    }
}

static void asset_info(de__editor *ed, const de__manifest_asset *ma, de_ep_asset_info *o) {
    memset(o, 0, sizeof *o);
    o->id = de_ep_cstr(ma->id);
    o->type = de_ep_cstr(ma->type);
    o->path = de_ep_cstr(ma->source != NULL ? ma->source : "");
    o->state = de_ep_cstr(state_name(de_asset_status_of(S(ed), de_asset_id_from(ma->id))));
    const u32 h = de__slot_find(S(ed), de_asset_id_from(ma->id));
    const de__slot *sl = h != 0 ? de__slot_get(S(ed), h) : NULL;
    if (sl != NULL && sl->bytes != 0) {
        o->has_cooked_size = true;
        o->cooked_size = sl->bytes;
    }
}

static bool matches(const de__manifest_asset *ma, de_ep_str filter) {
    if (filter.len == 0)
        return true;
    if (de_ep_str_eq(filter, ma->type))
        return true;
    char f[256];
    const u32 n = filter.len < sizeof f - 1 ? filter.len : (u32)sizeof f - 1;
    memcpy(f, filter.ptr, n);
    f[n] = '\0';
    return strstr(ma->id, f) != NULL || (ma->source != NULL && strstr(ma->source, f) != NULL);
}

static void on_list(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(asset_list, de_ep_asset_list_params, q);
    const de__manifest *m = manifest(ed);
    de_ep_asset_list list;
    memset(&list, 0, sizeof list);
    u32 start = 0;
    if (q.has_cursor && q.cursor.len > 0) {
        char buf[16];
        const u32 n = q.cursor.len < 15 ? q.cursor.len : 15;
        memcpy(buf, q.cursor.ptr, n);
        buf[n] = '\0';
        start = (u32)strtoul(buf, NULL, 10);
    }
    list.assets.items =
        (de_ep_asset_info *)de__ed_alloc(a, sizeof(de_ep_asset_info) * ED_ASSET_PAGE);
    u32 i = start;
    for (; m != NULL && i < m->asset_count && list.assets.items != NULL; ++i) {
        if (!matches(&m->assets[i], q.filter))
            continue;
        if (list.assets.count == ED_ASSET_PAGE)
            break;
        asset_info(ed, &m->assets[i], &list.assets.items[list.assets.count++]);
    }
    char cursor[16];
    if (m != NULL && i < m->asset_count) {
        snprintf(cursor, sizeof cursor, "%u", i);
        list.has_cursor = true;
        list.cursor = de_ep_cstr(cursor);
    }
    de_ep_write_response_asset_list(w, req->id, &list);
}

static const de__manifest_asset *find_asset(de__editor *ed, de_ep_str id) {
    const de__manifest *m = manifest(ed);
    for (u32 i = 0; m != NULL && i < m->asset_count; ++i)
        if (de_ep_str_eq(id, m->assets[i].id))
            return &m->assets[i];
    return NULL;
}

static void on_status(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(asset_status, de_ep_asset_ids, q);
    de_ep_asset_list list;
    memset(&list, 0, sizeof list);
    list.assets.items =
        (de_ep_asset_info *)de__ed_alloc(a, sizeof(de_ep_asset_info) * (q.ids.count + 1));
    for (u32 i = 0; i < q.ids.count && list.assets.items != NULL; ++i) {
        const de__manifest_asset *ma = find_asset(ed, q.ids.items[i]);
        if (ma == NULL) {
            de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "asset %.*s", (int)q.ids.items[i].len,
                         q.ids.items[i].ptr);
            return;
        }
        asset_info(ed, ma, &list.assets.items[list.assets.count++]);
    }
    de_ep_write_response_asset_status(w, req->id, &list);
}

static void on_reimport(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                        mpack_writer_t *w) {
    DE__ED_PARAMS(asset_reimport, de_ep_asset_ids, q);
#if DE_HAS_RENDER
    de__ed_preview_invalidate();
#endif
    for (u32 i = 0; i < q.ids.count; ++i) {
        char *id = de__ed_cstr(a, q.ids.items[i]);
        /* An entry the IDE just added: read the manifest now rather than at the watcher's next
         * event. A new entry loads with the manifest; there is nothing to reimport. */
        if (id != NULL && find_asset(ed, q.ids.items[i]) == NULL &&
            de__asset_reload_manifest_now(S(ed)) && find_asset(ed, q.ids.items[i]) != NULL) {
            /* Start loading it (frames finish the load; asset.status says when), so what it
             * declares, such as a model's meshes, can be referenced. */
            if (g_assets.load_count < ED_LOADS)
                g_assets.loads[g_assets.load_count++] =
                    de_asset_load(S(ed), de_asset_id_from(id), 0);
            continue;
        }
        if (id == NULL || find_asset(ed, q.ids.items[i]) == NULL) {
            de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "asset %s", id ? id : "?");
            return;
        }
        /* Known but never loaded: load it (as a new entry). */
        if (de_asset_status_of(S(ed), de_asset_id_from(id)) == DE_LOAD_INVALID) {
            if (g_assets.load_count < ED_LOADS)
                g_assets.loads[g_assets.load_count++] =
                    de_asset_load(S(ed), de_asset_id_from(id), 0);
            continue;
        }
        if (!de__asset_reload_queue(S(ed), de_asset_id_from(id))) {
            de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED,
                         "hot reload is off ([assets] hot_reload)");
            return;
        }
    }
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_asset_reimport(w, req->id, &e);
}

/* ---------- Dependencies ---------- */

static de_ep_str id_str(de__editor *ed, de_asset_id id, de_ep_arena *a) {
    const char *p = S(ed)->resolved ? de__resolved_path(S(ed)->resolved, id) : NULL;
    if (p != NULL)
        return de_ep_cstr(p);
    char *buf = (char *)de__ed_alloc(a, 24);
    if (buf == NULL)
        return de_ep_cstr("");
    snprintf(buf, 24, "%016llx", (unsigned long long)id.hash);
    return de_ep_cstr(buf);
}

static void on_dependencies(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                            mpack_writer_t *w) {
    DE__ED_PARAMS(asset_dependencies, de_ep_asset_id_params, q);
    char *id = de__ed_cstr(a, q.id);
    const u32 h = id ? de__slot_find(S(ed), de_asset_id_from(id)) : 0;
    const de__slot *sl = h ? de__slot_get(S(ed), h) : NULL;
    de_ep_asset_ids r;
    memset(&r, 0, sizeof r);
    if (sl != NULL) {
        r.ids.items = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * (sl->dep_count + 1));
        for (u32 i = 0; i < sl->dep_count && r.ids.items != NULL; ++i)
            r.ids.items[r.ids.count++] = id_str(ed, sl->deps[i], a);
    } else if (find_asset(ed, q.id) == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "asset %s", id ? id : "?");
        return;
    }
    de_ep_write_response_asset_dependencies(w, req->id, &r);
}

typedef struct ref_walk {
    de__editor *ed;
    de_asset_id target;
    de_ep_arena *a;
    de_ep_asset_ids *out;
    u32 cap;
} ref_walk;

static void ref_visit(de_asset_system *s, u32 handle, void *user) {
    ref_walk *rw = (ref_walk *)user;
    const de__slot *sl = de__slot_get(s, handle);
    if (sl == NULL || rw->out->ids.count >= rw->cap)
        return;
    for (u32 i = 0; i < sl->dep_count; ++i)
        if (sl->deps[i].hash == rw->target.hash) {
            rw->out->ids.items[rw->out->ids.count++] = id_str(rw->ed, sl->id, rw->a);
            return;
        }
}

static void on_referencers(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                           mpack_writer_t *w) {
    DE__ED_PARAMS(asset_referencers, de_ep_asset_id_params, q);
    char *id = de__ed_cstr(a, q.id);
    de_ep_asset_ids r;
    memset(&r, 0, sizeof r);
    ref_walk rw = {ed, de_asset_id_from(id ? id : ""), a, &r, 4096};
    r.ids.items = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * rw.cap);
    if (r.ids.items != NULL && S(ed) != NULL)
        de__slot_for_each_live(S(ed), ref_visit, &rw);
    de_ep_write_response_asset_referencers(w, req->id, &r);
}

/* ---------- Thumbnails ---------- */

static bool read_vfile(const char *vpath, void **data, u64 *size) {
    if (de_file_read_all(vpath, de_heap_allocator(), data, size) != DE_OK) {
        de_error_clear();
        return false;
    }
    return true;
}

/* The source file's virtual path: the manifest's folder, its asset_root, then `source`. */
static bool source_vpath(de__editor *ed, const de__manifest_asset *ma, char *out, size_t cap) {
    const de__manifest *m = manifest(ed);
    if (m == NULL || ma->source == NULL)
        return false;
    const char *root = m->root;
    const char *slash = strrchr(root, '/');
    const int dir = slash != NULL ? (int)(slash - root) : 0;
    const char *ar = m->asset_root != NULL && m->asset_root[0] ? m->asset_root : ".";
    const int n = snprintf(out, cap, "%.*s/%s/%s", dir, root, ar, ma->source);
    return n > 0 && (size_t)n < cap;
}

static void on_thumbnail(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w) {
    DE__ED_PARAMS(asset_thumbnail, de_ep_asset_thumbnail_params, q);
    const de__manifest_asset *ma = find_asset(ed, q.id);
    if (ma == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "asset %.*s", (int)q.id.len, q.id.ptr);
        return;
    }
    /* Images are their own thumbnail: the IDE scales the PNG/JPEG source. */
    char vpath[DE_FS_PATH_MAX];
    const char *ext = ma->source ? strrchr(ma->source, '.') : NULL;
    const bool image = ext != NULL && (strcmp(ext, ".png") == 0 || strcmp(ext, ".jpg") == 0 ||
                                       strcmp(ext, ".jpeg") == 0);
    void *data = NULL;
    u64 size = 0;
    if (!image || !source_vpath(ed, ma, vpath, sizeof vpath) || !read_vfile(vpath, &data, &size)) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "no thumbnail for %s assets yet", ma->type);
        return;
    }
    de_ep_blob_ref b;
    memset(&b, 0, sizeof b);
    b.blob_id = de__ed_send_blob(ed, data, (size_t)size);
    DE_FREE(de_heap_allocator(), data, (size_t)size);
    de_ep_write_response_asset_thumbnail(w, req->id, &b);
}

/* ---------- Importer schemas ---------- */

static bool asset_load_pending(de_asset_system *assets, de_asset_id id) {
    for (u32 i = 0; i < g_assets.load_count; ++i)
        if (g_assets.load_ids[i].hash == id.hash)
            return true;
    if (g_assets.load_count == ED_LOADS)
        return false;
    de_load_handle load = de_asset_load(assets, id, 0);
    if (!load.id)
        return false;
    u32 at = g_assets.load_count++;
    g_assets.loads[at] = load;
    g_assets.load_ids[at] = id;
    return true;
}

static void on_font_atlas(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                          mpack_writer_t *w) {
    DE__ED_PARAMS(asset_font_atlas, de_ep_font_atlas_params, q);
    if (!ed->e->text) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "assets.fontAtlas");
        return;
    }
    if (!q.size || q.size > 1024) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "font atlas size must be 1..1024");
        return;
    }
    const de__manifest_asset *ma = find_asset(ed, q.id);
    if (!ma) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "font asset not found");
        return;
    }
    if (strcmp(ma->type, "font")) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "asset is not a font");
        return;
    }
    de_asset_id id = de_asset_id_from(ma->id);
    de_font handle = de_asset_font(S(ed), id);
    de_font_atlas_info info;
    if (de_text_copy_font_atlas(ed->e->text, handle, 0, q.size, NULL, 0, &info) != DE_OK) {
        if (de_asset_status_of(S(ed), id) == DE_LOAD_FAILED) {
            char error[256] = {0};
            (void)de_asset_last_error(S(ed), id, error, sizeof error);
            de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "font load failed: %s", error);
        } else {
            (void)asset_load_pending(S(ed), id);
            de__ed_error(w, req->id, DE_EP_ERR_BUSY, "font atlas is loading; retry");
        }
        return;
    }
    if (q.page >= info.page_count) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "font atlas page is out of range");
        return;
    }
    const u64 bytes = (u64)info.width * info.height * 4;
    u8 *pixels = de__ed_alloc(a, (size_t)bytes);
    if (!pixels || de_text_copy_font_atlas(ed->e->text, handle, q.page, q.size, pixels, bytes,
                                           &info) != DE_OK) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "font atlas changed; retry");
        return;
    }
    u64 blob = de__ed_send_blob(ed, pixels, (size_t)bytes);
    if (!blob) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "font atlas transfer failed");
        return;
    }
    de_ep_font_atlas_page result = {.page = q.page,
                                    .page_count = info.page_count,
                                    .baked_page_count = info.baked_page_count,
                                    .width = info.width,
                                    .height = info.height,
                                    .atlas_width = info.atlas_width,
                                    .atlas_height = info.atlas_height,
                                    .generation = info.generation,
                                    .source_revision = info.source_revision,
                                    .distance_range = info.distance_range,
                                    .mode = de_ep_cstr(info.pixel    ? "pixel"
                                                       : info.bitmap ? "bitmap"
                                                                     : "msdf"),
                                    .pixels = {.blob_id = blob}};
    de_ep_write_response_asset_font_atlas(w, req->id, &result);
}

typedef struct schema_prop {
    const char *name, *type, *def, *values, *desc;
    f64 min, max;
} schema_prop;

static const schema_prop k_texture[] = {
    {"usage", "string", "color", "color normal mask environment", "What the texture holds", 0, 0},
    {"srgb", "boolean", NULL, NULL, "The colour channels are sRGB-encoded", 0, 0},
    {"mips", "boolean", "true", NULL, "The full mip chain", 0, 0},
    {"pixel_art", "boolean", "false", NULL, "No mips; nearest sampling", 0, 0},
    {"alpha_coverage", "number", NULL, NULL, "Alpha-test cutoff kept in every mip", 0, 1},
    {"max_size", "integer", NULL, NULL, "Longest edge, scaled down to it", 1, 16384},
    {"address", "string", "wrap", "wrap clamp", "How mips read past edges", 0, 0},
    {"compression", "string", "auto", "auto none bc1 bc3 bc4 bc5 bc6h bc7", "Block compression", 0,
     0},
    {"quality", "string", "normal", "fast normal high", "Encoder effort", 0, 0},
    {"rdo_quality", "number", "0", NULL, "Rate-distortion lambda (0: off)", 0, 4},
    {"rt_proxy", "boolean", "false", NULL, "Cook a silhouette proxy for sprite ray tracing", 0, 0},
};
static const schema_prop k_model[] = {
    {"scale", "number", "1", NULL, "Uniform scale", 0.0001, 10000},
    {"import_cameras", "boolean", "false", NULL, "Import cameras and lights", 0, 0},
    {"generate_lods", "boolean", "true", NULL, "Build the LOD chain", 0, 0},
    {"lod_error", "number", "0.01", NULL, "Screen-space error per LOD", 0, 1},
    {"meshlets", "boolean", "true", NULL, "Meshlets per LOD", 0, 0},
    {"max_position_error", "number", "0.0005", NULL, "Largest position error (m)", 0, 1},
    {"axis_up", "string", "auto", "auto +x -x +y -y +z -z", "FBX up axis", 0, 0},
    {"axis_front", "string", "auto", "auto +x -x +y -y +z -z", "FBX front axis", 0, 0},
};
static const schema_prop k_sound[] = {
    {"streaming", "boolean", "false", NULL, "Stream from disk instead of decoding up front", 0, 0},
    {"quality", "number", "5", NULL, "Codec quality", 0, 10},
    {"loop", "boolean", "false", NULL, "Loop by default", 0, 0},
};

static void write_schema(mpack_writer_t *mw, const schema_prop *p, u32 n) {
    mpack_build_map(mw);
    mpack_write_cstr(mw, "type");
    mpack_write_cstr(mw, "object");
    mpack_write_cstr(mw, "properties");
    mpack_build_map(mw);
    for (u32 i = 0; i < n; ++i) {
        mpack_write_cstr(mw, p[i].name);
        mpack_build_map(mw);
        mpack_write_cstr(mw, "type");
        mpack_write_cstr(mw, p[i].type);
        if (p[i].desc) {
            mpack_write_cstr(mw, "description");
            mpack_write_cstr(mw, p[i].desc);
        }
        if (p[i].def) {
            mpack_write_cstr(mw, "default");
            if (strcmp(p[i].type, "boolean") == 0)
                mpack_write_bool(mw, strcmp(p[i].def, "true") == 0);
            else if (strcmp(p[i].type, "string") == 0)
                mpack_write_cstr(mw, p[i].def);
            else
                mpack_write_double(mw, atof(p[i].def));
        }
        if (p[i].values) {
            mpack_write_cstr(mw, "enum");
            mpack_build_array(mw);
            char buf[256];
            snprintf(buf, sizeof buf, "%s", p[i].values);
            for (char *t = strtok(buf, " "); t != NULL; t = strtok(NULL, " "))
                mpack_write_cstr(mw, t);
            mpack_complete_array(mw);
        }
        if (p[i].min < p[i].max) {
            mpack_write_cstr(mw, "minimum");
            mpack_write_double(mw, p[i].min);
            mpack_write_cstr(mw, "maximum");
            mpack_write_double(mw, p[i].max);
        }
        mpack_complete_map(mw);
    }
    mpack_complete_map(mw);
    mpack_complete_map(mw);
}

static void on_schema(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(asset_importer_schema, de_ep_asset_type_params, q);
    const schema_prop *p = NULL;
    u32 n = 0;
    if (de_ep_str_eq(q.type, "texture")) {
        p = k_texture;
        n = (u32)(sizeof k_texture / sizeof k_texture[0]);
    } else if (de_ep_str_eq(q.type, "model")) {
        p = k_model;
        n = (u32)(sizeof k_model / sizeof k_model[0]);
    } else if (de_ep_str_eq(q.type, "sound")) {
        p = k_sound;
        n = (u32)(sizeof k_sound / sizeof k_sound[0]);
    }
    if (p == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "no importer settings for %.*s",
                     (int)q.type.len, q.type.ptr);
        return;
    }
    mpack_writer_t mw;
    de__ed_raw_begin(a, &mw);
    write_schema(&mw, p, n);
    de_ep_schema s;
    s.schema = de__ed_raw_end(a, &mw);
    de_ep_write_response_asset_importer_schema(w, req->id, &s);
}

static void on_preview(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                       mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(asset_preview_settings, de_ep_asset_preview_params, q);
    (void)q;
    de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED,
                 "settings previews need an in-process cook (DENG-578)");
}

/* ---------- Cook cache ---------- */

typedef struct cache_walk {
    const char *dir;
    de_ep_arena *a;
    de_ep_cache_entry *items;
    u32 count, cap;
    u64 bytes;
    bool remove;
} cache_walk;

static bool cache_visit(const char *name, bool is_dir, void *user) {
    cache_walk *cw = (cache_walk *)user;
    if (is_dir)
        return true;
    char vpath[DE_FS_PATH_MAX];
    snprintf(vpath, sizeof vpath, "%s/%s", cw->dir, name);
    if (cw->remove) {
        (void)de_file_delete(vpath);
        de_error_clear();
        return true;
    }
    de_file *f = NULL;
    u64 size = 0;
    if (de_file_open(vpath, DE_FILE_READ, &f) == DE_OK) {
        size = de_file_size(f);
        de_file_close(f);
    }
    de_error_clear();
    cw->bytes += size;
    if (cw->items != NULL && cw->count < cw->cap) {
        de_ep_cache_entry *e = &cw->items[cw->count++];
        memset(e, 0, sizeof *e);
        char *n = (char *)de__ed_alloc(cw->a, strlen(name) + 1);
        if (n != NULL)
            memcpy(n, name, strlen(name) + 1);
        e->asset = de_ep_cstr(n ? n : "");
        e->type = de_ep_cstr("");
        e->bytes = size;
    }
    return true;
}

static void on_cache_stats(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                           mpack_writer_t *w) {
    const char *dir = S(ed) != NULL ? S(ed)->desc.cache_dir : "cache:/decache";
    cache_walk cw = {dir, a, NULL, 0, 4096, 0, false};
    cw.items = (de_ep_cache_entry *)de__ed_alloc(a, sizeof(de_ep_cache_entry) * cw.cap);
    (void)de_dir_list(dir, cache_visit, &cw);
    de_error_clear();
    de_ep_cache_stats st;
    memset(&st, 0, sizeof st);
    st.dir = de_ep_cstr(dir);
    st.bytes = cw.bytes;
    st.entries.items = cw.items;
    st.entries.count = cw.count;
    de_ep_write_response_cache_stats(w, req->id, &st);
}

static void on_cache_clear(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                           mpack_writer_t *w) {
    DE__ED_PARAMS(cache_clear, de_ep_cache_clear_params, q);
    if ((q.has_ids && q.ids.count > 0) || (q.has_types && q.types.count > 0)) {
        /* Entries are keyed by content hash: only a full clear is possible. */
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "the cook cache can only be cleared whole");
        return;
    }
    const char *dir = S(ed) != NULL ? S(ed)->desc.cache_dir : "cache:/decache";
    cache_walk cw = {dir, a, NULL, 0, 0, 0, true};
    (void)de_dir_list(dir, cache_visit, &cw);
    de_error_clear();
    de_ep_empty e;
    memset(&e, 0, sizeof e);
    de_ep_write_response_cache_clear(w, req->id, &e);
}

/* ---------- Models ---------- */

#if DE_HAS_RENDER
typedef struct model_names {
    de__editor *ed;
    de_ep_arena *arena;
    de_result error;
} model_names;
static const char *model_asset_name(model_names *context, de_asset_id id) {
    de_asset_system *assets = S(context->ed);
    const char *path = de_asset_id_path(assets, id);
    if (path)
        return path;
    size_t bytes = 0;
    context->error = de__cook_stage_subasset_path(assets, id, NULL, 0, &bytes);
    if (context->error != DE_OK)
        return NULL;
    char *copy = de__ed_alloc(context->arena, bytes);
    if (!copy) {
        context->error = DE_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    context->error = de__cook_stage_subasset_path(assets, id, copy, bytes, &bytes);
    return context->error == DE_OK ? copy : NULL;
}
static const char *model_handle_name(void *user, u32 handle) {
    model_names *context = user;
    const de__slot *slot = de__slot_get(S(context->ed), handle);
    return slot ? model_asset_name(context, slot->id) : NULL;
}
static const char *model_material_name(void *user, de_asset_id id) {
    return model_asset_name(user, id);
}
#endif
static void on_model_inspect(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                             mpack_writer_t *w) {
    DE__ED_PARAMS(asset_model_inspect, de_ep_model_inspect_params, q);
#if DE_HAS_RENDER
    if (!ed->e->renderer) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "assets.modelInspection");
        return;
    }
    const de__manifest_asset *entry = find_asset(ed, q.id);
    if (!entry) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "model asset not found");
        return;
    }
    if (strcmp(entry->type, "model")) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "inspection requires a model asset");
        return;
    }
    if (q.mesh >= 1024) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "model mesh index is out of range");
        return;
    }
    de_asset_system *assets = S(ed);
    de_asset_id id = de_asset_id_from(entry->id);
    de_model handle = de_asset_model(assets, id);
    const de_render_model *model = de_render_model_get(assets, handle);
    de_load_state state = de_asset_status_of(assets, id);
    if (!model || state == DE_LOAD_FAILED || state == DE_LOAD_PARTIAL) {
        if (state == DE_LOAD_FAILED || state == DE_LOAD_PARTIAL) {
            char error[256] = {0};
            (void)de_asset_last_error(assets, id, error, sizeof error);
            de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "model load failed: %s", error);
        } else {
            (void)asset_load_pending(assets, id);
            de__ed_error(w, req->id, DE_EP_ERR_BUSY, "model inspection is loading; retry");
        }
        return;
    }
    if (model->mesh_count && q.mesh >= model->mesh_count) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "model mesh index is out of range");
        return;
    }
    const de_render_mesh *mesh =
        model->mesh_count ? de_render_mesh_get(assets, model->meshes[q.mesh]) : NULL;
    de_ep_model_inspection result;
    model_names context = {.ed = ed, .arena = a, .error = DE_OK};
    const de__ed_model_names names = {model_handle_name, model_material_name, &context};
    de_result status = de__ed_model_inspect(model, mesh, q.mesh, &names, a, &result);
    if (status == DE_ERR_NOT_FOUND && context.error != DE_OK)
        status = context.error;
    if (status != DE_OK) {
        de__ed_error(w, req->id,
                     status == DE_ERR_INVALID_ARGUMENT ? DE_EP_ERR_INVALID_PARAMS
                     : status == DE_ERR_UNSUPPORTED    ? DE_EP_ERR_UNSUPPORTED
                                                       : DE_EP_ERR_INTERNAL,
                     "model inspection could not produce complete metadata: %s",
                     de_result_str(status));
        return;
    }
    de_ep_write_response_asset_model_inspect(w, req->id, &result);
#else
    (void)q;
    de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "assets.modelInspection");
#endif
}

static void on_model_info(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                          mpack_writer_t *w) {
    DE__ED_PARAMS(asset_model_info, de_ep_asset_id_params, q);
#if DE_HAS_RENDER
    char *id = de__ed_cstr(a, q.id);
    de_asset_system *s = S(ed);
    const de_asset_id aid = de_asset_id_from(id ? id : "");
    de_model m = s ? de_asset_model(s, aid) : (de_model){0};
    if (!DE_HANDLE_VALID(m)) {
        de_error_clear();
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "model %s", id ? id : "?");
        return;
    }
    de_load_handle lh = de_asset_load(s, aid, 0);
    (void)de_asset_wait(s, lh, 10000000000ULL);
    const de_render_model *rm = de_render_model_get(s, m);
    if (rm == NULL) {
        de_asset_release(s, lh);
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "model %s did not load", id);
        return;
    }
    de_ep_model_info mi;
    memset(&mi, 0, sizeof mi);
    mi.nodes.items =
        (de_ep_model_node *)de__ed_alloc(a, sizeof(de_ep_model_node) * (rm->node_count + 1));
    for (u32 i = 0; i < rm->node_count && mi.nodes.items != NULL; ++i) {
        de_ep_model_node *n = &mi.nodes.items[mi.nodes.count++];
        memset(n, 0, sizeof *n);
        n->name = de_ep_cstr(rm->nodes[i].name ? rm->nodes[i].name : "");
        if (rm->nodes[i].parent != DE_COOKED_NONE) {
            n->has_parent = true;
            n->parent = rm->nodes[i].parent;
        }
        if (DE_HANDLE_VALID(rm->nodes[i].mesh)) {
            n->has_mesh = true;
            n->mesh = n->name;
        }
    }
    /* LODs, attributes and bounds from the first mesh. */
    static f32 bmin[3], bmax[3];
    const de_render_mesh *mesh = rm->mesh_count > 0 ? de_render_mesh_get(s, rm->meshes[0]) : NULL;
    if (mesh != NULL) {
        mi.lods.items =
            (de_ep_model_lod *)de__ed_alloc(a, sizeof(de_ep_model_lod) * (mesh->lod_count + 1));
        for (u32 l = 0; l < mesh->lod_count && mi.lods.items != NULL; ++l) {
            de_ep_model_lod *o = &mi.lods.items[mi.lods.count++];
            o->triangles = mesh->lods[l].index_count / 3u;
            o->meshlets = mesh->lods[l].meshlet_count;
            o->screen_error = mesh->lod_error * (f32)(l + 1);
        }
        static const char *const stream_names[] = {"position", "normal", "tangent", "uv0",
                                                   "uv1",      "color",  "joints",  "weights"};
        mi.attributes.items = (de_ep_model_attribute *)de__ed_alloc(
            a, sizeof(de_ep_model_attribute) * DE_COOKED_STREAM_COUNT);
        for (u32 k = 0; k < DE_COOKED_STREAM_COUNT && k < 8 && mi.attributes.items != NULL; ++k) {
            if (mesh->stream_format[k] == 0)
                continue;
            de_ep_model_attribute *o = &mi.attributes.items[mi.attributes.count++];
            o->name = de_ep_cstr(stream_names[k]);
            char *fmt = (char *)de__ed_alloc(a, 16);
            if (fmt != NULL)
                snprintf(fmt, 16, "fmt%u", mesh->stream_format[k]);
            o->format = de_ep_cstr(fmt ? fmt : "");
            if (k == 3 || k == 4)
                ++mi.uv_sets;
        }
        memcpy(bmin, mesh->bounds_min, sizeof bmin);
        memcpy(bmax, mesh->bounds_max, sizeof bmax);
        mi.bounds_min.items = bmin;
        mi.bounds_min.count = 3;
        mi.bounds_max.items = bmax;
        mi.bounds_max.count = 3;
    }
    mi.slots.items =
        (de_ep_model_slot *)de__ed_alloc(a, sizeof(de_ep_model_slot) * (rm->material_count + 1));
    for (u32 i = 0; i < rm->material_count && mi.slots.items != NULL; ++i) {
        de_ep_model_slot *o = &mi.slots.items[mi.slots.count++];
        char *nm = (char *)de__ed_alloc(a, 16);
        if (nm != NULL)
            snprintf(nm, 16, "slot%u", i);
        o->name = de_ep_cstr(nm ? nm : "");
        const char *mp = de__ed_asset_path(ed, rm->materials[i].id);
        o->material = de_ep_cstr(mp ? mp : "");
    }
    de_asset_release(s, lh);
    de_ep_write_response_asset_model_info(w, req->id, &mi);
#else
    (void)ed;
    (void)q;
    de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "no renderer in this build");
#endif
}

/* ---------- asset.event ---------- */

static void tick(de__editor *ed) {
    de_asset_system *s = S(ed);
    if (s == NULL)
        return;
    for (u32 i = 0; i < g_assets.load_count;) {
        const de_load_state st = de_asset_status(s, g_assets.loads[i], NULL);
        if (st == DE_LOAD_READY || st == DE_LOAD_FAILED || st == DE_LOAD_CANCELLED) {
            de_asset_release(s, g_assets.loads[i]);
            g_assets.loads[i] = g_assets.loads[--g_assets.load_count];
            g_assets.load_ids[i] = g_assets.load_ids[g_assets.load_count];
            g_assets.load_ids[g_assets.load_count] = (de_asset_id){0};
        } else {
            ++i;
        }
    }
    if (!g_assets.has_reader) {
        const de_event_type t = de_asset_event_type(s);
        if (t.id == 0)
            return;
        g_assets.events = de_event_reader_create(ed->e->world, t);
        g_assets.has_reader = true;
    }
    const de_asset_event *ev;
    while ((ev = (const de_asset_event *)de_event_next(ed->e->world, &g_assets.events)) != NULL) {
        de_ep_asset_event_kind kind;
        switch (ev->kind) {
        case DE_ASSET_EV_LOADED:
            kind = DE_EP_ASSET_EVENT_KIND_LOADED;
            break;
        case DE_ASSET_EV_FAILED:
#if DE_HAS_RENDER
            de__ed_preview_invalidate();
#endif
            kind = DE_EP_ASSET_EVENT_KIND_FAILED;
            break;
        case DE_ASSET_EV_RELOADED:
#if DE_HAS_RENDER
            de__ed_preview_invalidate();
#endif
            kind = DE_EP_ASSET_EVENT_KIND_RELOADED;
            break;
        case DE_ASSET_EV_EVICTED:
            kind = DE_EP_ASSET_EVENT_KIND_EVICTED;
            break;
        default:
            continue;
        }
        de_ep_arena a = {ed->arena, ed->arena_cap, 0};
        de_ep_asset_event d;
        memset(&d, 0, sizeof d);
        d.id = id_str(ed, ev->id, &a);
        d.kind = kind;
        d.has_ms = ev->has_cook_ms;
        d.ms = ev->cook_ms;
        if (ev->source[0]) {
            d.has_source = true;
            d.source = de_ep_cstr(ev->source);
        }
        if (ev->line) {
            d.has_line = true;
            d.line = ev->line;
        }
        char msg[256];
        if (kind == DE_EP_ASSET_EVENT_KIND_FAILED &&
            de_asset_last_error(s, ev->id, msg, sizeof msg) != DE_OK) {
            d.has_error = true;
            d.error = de_ep_cstr(msg);
        }
        mpack_writer_t nw;
        const u64 seq = de__ed_note_begin(ed, &nw);
        de_ep_write_notify_asset_event(&nw, seq, &d);
        de__ed_send(ed, &nw, DE_EP_KIND_NOTIFICATION);
    }
}

void de__ed_assets_register(de__editor *ed) {
    memset(&g_assets, 0, sizeof g_assets);
    ed->assets = (struct de__ed_assets *)&g_assets;
    de__ed_on(ed, DE_EP_METHOD_ASSET_LIST, on_list);
    de__ed_on(ed, DE_EP_METHOD_ASSET_STATUS, on_status);
    de__ed_on(ed, DE_EP_METHOD_ASSET_REIMPORT, on_reimport);
    de__ed_on(ed, DE_EP_METHOD_ASSET_DEPENDENCIES, on_dependencies);
    de__ed_on(ed, DE_EP_METHOD_ASSET_REFERENCERS, on_referencers);
    de__ed_on(ed, DE_EP_METHOD_ASSET_THUMBNAIL, on_thumbnail);
    de__ed_on(ed, DE_EP_METHOD_ASSET_FONT_ATLAS, on_font_atlas);
    if (ed->e->text)
        de__ed_add_capability(ed, "assets.fontAtlas");
    de__ed_on(ed, DE_EP_METHOD_ASSET_IMPORTER_SCHEMA, on_schema);
    de__ed_on(ed, DE_EP_METHOD_ASSET_PREVIEW_SETTINGS, on_preview);
    de__ed_on(ed, DE_EP_METHOD_CACHE_STATS, on_cache_stats);
    de__ed_on(ed, DE_EP_METHOD_CACHE_CLEAR, on_cache_clear);
    de__ed_on(ed, DE_EP_METHOD_ASSET_MODEL_INFO, on_model_info);
    de__ed_on(ed, DE_EP_METHOD_ASSET_MODEL_INSPECT, on_model_inspect);
#if DE_HAS_RENDER
    if (ed->e->renderer)
        de__ed_add_capability(ed, "assets.modelInspection");
#endif
    de__ed_on_tick(ed, tick);
}
