#include "glyph_cache_internal.h"
#include "fallback_internal.h"
#include "msdf/msdf_wrap.h"
#include "asset/asset_internal.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#if defined(__APPLE__)
#include <pthread.h>
#endif
enum { GLYPH_QUEUED, GLYPH_COMPLETE, GLYPH_INTEGRATED, GLYPH_REJECTED };
static u32 lookup(struct de__glyph_cache *c, u32 font, u32 glyph, bool insert) {
    u32 hash = (glyph * 2654435761u + font * 2246822519u) & (DE_GLYPH_HASH - 1);
    for (u32 i = 0; i < DE_GLYPH_HASH; ++i) {
        u32 slot = (hash + i) & (DE_GLYPH_HASH - 1), v = c->hash[slot];
        if (!v) {
            if (!insert || c->count == DE_GLYPH_REQUESTS)
                return UINT32_MAX;
            u32 index = c->count++;
            c->hash[slot] = index + 1;
            c->jobs[index] = (de__glyph_job){.owner = c, .font = font, .glyph = glyph};
            return index;
        }
        if (c->jobs[v - 1].font == font && c->jobs[v - 1].glyph == glyph)
            return v - 1;
    }
    return UINT32_MAX;
}
static u32 font_index(struct de__glyph_cache *c, de_font handle, const de_text_font *f,
                      bool create) {
    for (u32 i = 0; i < c->font_count; ++i)
        if (c->fonts[i].handle.id == handle.id && c->fonts[i].identity == f)
            return i;
    if (!create || c->font_count == DE_GLYPH_FONTS)
        return UINT32_MAX;
    u32 i = c->font_count++;
    de__glyph_font *g = &c->fonts[i];
    memset(g, 0, sizeof *g);
    g->handle = handle;
    g->identity = f;
    g->source_size = f->source_size;
    g->source = DE_ALLOC(c->alloc, f->source_size, 1, DE_MEM_UI);
    if (!g->source) {
        --c->font_count;
        return UINT32_MAX;
    }
    memcpy(g->source, f->source, f->source_size);
    g->base_pages = f->pages;
    g->width = f->width;
    g->height = f->height;
    g->em = f->em;
    g->range = f->range;
    return i;
}
static de_result generate(de__glyph_job *j) {
    struct de__glyph_cache *c = j->owner;
    de__glyph_font *f = &c->fonts[j->font];
    stbtt_fontinfo face;
    if (!stbtt_InitFont(&face, f->source, 0))
        return DE_ERR_PARSE;
    u32 upem = (u32)f->source[face.head + 18] * 256 + f->source[face.head + 19];
    float inv = 1.0f / (f32)upem;
    double scale = (double)f->em / upem;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, adv, lsb;
    stbtt_GetGlyphHMetrics(&face, (int)j->glyph, &adv, &lsb);
    stbtt_GetGlyphBox(&face, (int)j->glyph, &x0, &y0, &x1, &y1);
    j->entry =
        (de__font_glyph){.index = j->glyph, .advance = (f32)adv * inv, .bearing = (f32)lsb * inv};
    if (x0 == x1 || y0 == y1)
        return DE_OK;
    int range = (int)f->range, lx = (int)floor(x0 * scale) - range,
        by = (int)floor(y0 * scale) - range, rx = (int)ceil(x1 * scale) + range,
        ty = (int)ceil(y1 * scale) + range;
    u32 w = (u32)(rx - lx), h = (u32)(ty - by);
    if (w > f->width || h > f->height)
        return DE_ERR_UNSUPPORTED;
    j->entry.w = w;
    j->entry.h = h;
    j->entry.left = (f32)lx / f->em;
    j->entry.bottom = (f32)by / f->em;
    j->entry.right = (f32)rx / f->em;
    j->entry.top = (f32)ty / f->em;
    stbtt_vertex *v = NULL;
    int n = stbtt_GetGlyphShape(&face, (int)j->glyph, &v);
    if (n <= 0)
        return DE_OK;
    de__msdf_vertex *edges =
        DE_ALLOC(c->alloc, (u64)n * sizeof *edges, _Alignof(de__msdf_vertex), DE_MEM_UI);
    float *scratch = DE_ALLOC(c->alloc, (u64)w * h * 4 * sizeof(float), _Alignof(float), DE_MEM_UI);
    j->pixels = DE_ALLOC(c->alloc, (u64)w * h * 4, 1, DE_MEM_UI);
    if (!edges || !scratch || !j->pixels) {
        if (edges)
            DE_FREE(c->alloc, edges, (u64)n * sizeof *edges);
        if (scratch)
            DE_FREE(c->alloc, scratch, (u64)w * h * 4 * sizeof(float));
        stbtt_FreeShape(&face, v);
        return DE_ERR_OUT_OF_MEMORY;
    }
    for (int i = 0; i < n; ++i)
        edges[i] =
            (de__msdf_vertex){v[i].type, v[i].x, v[i].y, v[i].cx, v[i].cy, v[i].cx1, v[i].cy1};
    stbtt_FreeShape(&face, v);
    de_result result =
        de__msdf_generate(edges, (u32)n, scale, -j->entry.left * (u32)upem,
                          -j->entry.bottom * (u32)upem, f->range, w, h, scratch, j->pixels);
    DE_FREE(c->alloc, edges, (u64)n * sizeof *edges);
    DE_FREE(c->alloc, scratch, (u64)w * h * 4 * sizeof(float));
    return result;
}
static void glyph_job(void *user) {
    de__glyph_job *j = user;
    struct de__glyph_cache *c = j->owner;
    j->worker = de_task_worker_index();
#if defined(__APPLE__)
    (void)pthread_getname_np(pthread_self(), j->thread_name, sizeof j->thread_name);
#endif /* A task-record OOM may run inline. Never perform MSDF on                                  \
                                           frame workers. */
    de_result result = j->worker == DE_TASK_WORKER_NONE ? generate(j) : DE_ERR_BUSY;
    de_mutex_lock(&c->mutex);
    j->result = result;
    j->status = GLYPH_COMPLETE;
    if (j->worker == DE_TASK_WORKER_NONE)
        ++c->stats.background_jobs;
    de_mutex_unlock(&c->mutex);
}
de_result de_text__glyph_cache_init(de_text_system *t) {
    struct de__glyph_cache *c = DE_NEW(t->alloc, struct de__glyph_cache, DE_MEM_UI);
    if (!c)
        return DE_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof *c);
    c->alloc = t->alloc;
    c->text = t;
    t->glyph_cache = c;
    de_mutex_init(&c->mutex);
    c->per_frame = de_cvar_register(&(de_cvar_desc){.name = "t.glyphs_per_frame",
                                                    .type = DE_CVAR_INT,
                                                    .default_number = 64,
                                                    .min = 1,
                                                    .max = 8192});
    c->pages_max = de_cvar_register(&(de_cvar_desc){.name = "t.runtime_pages_max",
                                                    .type = DE_CVAR_INT,
                                                    .default_number = 8,
                                                    .min = 0,
                                                    .max = 8});
    if (t->assets)
        c->tasks = t->assets->tasks;
    if (t->assets && !c->tasks) {
        de_fp_env_init_thread(); /* The standalone owner becomes task worker zero. */
        de_result r = de_task_system_create(&(de_task_system_desc){.allocator = t->alloc,
                                                                   .worker_count = 1,
                                                                   .background_worker_count = 1},
                                            &c->tasks);
        if (r != DE_OK) {
            de_text__glyph_cache_destroy(t);
            return r;
        }
        c->own_tasks = true;
    }
    return DE_OK;
}
void de_text__glyph_jobs_wait(de_text_system *t) {
    if (t && t->glyph_cache && t->glyph_cache->tasks)
        de_task_wait(t->glyph_cache->tasks, &t->glyph_cache->counter);
}
void de_text__glyph_cache_destroy(de_text_system *t) {
    struct de__glyph_cache *c = t->glyph_cache;
    if (!c)
        return;
    de_text__glyph_jobs_wait(t);
    if (c->own_tasks)
        de_task_system_destroy(c->tasks);
    for (u32 i = 0; i < c->count; ++i)
        if (c->jobs[i].pixels)
            DE_FREE(c->alloc, c->jobs[i].pixels, (u64)c->jobs[i].entry.w * c->jobs[i].entry.h * 4);
    for (u32 i = 0; i < c->font_count; ++i) {
        de__glyph_font *f = &c->fonts[i];
        for (u32 p = 0; p < f->pages; ++p) {
            DE_FREE(c->alloc, f->pixels[p], (u64)f->width * f->height * 4);
            DE_FREE(c->alloc, f->packers[p], sizeof(de_atlas_packer));
        }
        DE_FREE(c->alloc, f->source, f->source_size);
    }
    de_mutex_destroy(&c->mutex);
    DE_FREE(c->alloc, c, sizeof *c);
    t->glyph_cache = NULL;
}
void de_text__glyph_miss(de_text_system *t, de_font handle, u32 glyph) {
    if (!t || !t->glyph_cache || !t->glyph_cache->tasks)
        return;
    struct de__glyph_cache *c = t->glyph_cache;
    de__text_lock(t->runtime);
    de_text_font *f = de_text__font_get(t, handle);
    if (!f || f->mode != DE_FONT_MSDF || glyph >= (u32)f->face.numGlyphs) {
        de__text_unlock(t->runtime);
        return;
    }
    de_mutex_lock(&c->mutex);
    u32 fi = font_index(c, handle, f, true);
    u32 job = fi == UINT32_MAX ? UINT32_MAX : lookup(c, fi, glyph, false);
    bool submit = false;
    if (job == UINT32_MAX && fi != UINT32_MAX) {
        job = lookup(c, fi, glyph, true);
        if (job != UINT32_MAX) {
            ++c->stats.runtime_pending;
            submit = true;
        }
    }
    de_mutex_unlock(&c->mutex);
    de__text_unlock(t->runtime);
    if (submit)
        de_task_submit(c->tasks, DE_TASK_POOL_BACKGROUND, glyph_job, &c->jobs[job], &c->counter);
}
static void fill(de_text_font *f, const de__font_glyph *g, de_text_glyph_entry *out) {
    *out = (de_text_glyph_entry){.page = g->page,
                                 .x = g->x,
                                 .y = g->y,
                                 .w = g->w,
                                 .h = g->h,
                                 .uv = {(f32)g->x / (f32)f->width, (f32)g->y / (f32)f->height,
                                        (f32)(g->x + g->w) / (f32)f->width,
                                        (f32)(g->y + g->h) / (f32)f->height},
                                 .plane = {g->left, g->bottom, g->right, g->top},
                                 .advance = g->advance,
                                 .generation = f->generation};
}
bool de_text__runtime_glyph(de_text_system *t, de_font handle, u32 glyph,
                            de_text_glyph_entry *out) {
    struct de__glyph_cache *c = t->glyph_cache;
    if (!c)
        return false;
    de_text_font *f = de_text__font_get(t, handle);
    if (!f)
        return false;
    de_mutex_lock(&c->mutex);
    u32 fi = font_index(c, handle, f, false),
        index = fi == UINT32_MAX ? UINT32_MAX : lookup(c, fi, glyph, false);
    bool found = false;
    if (index != UINT32_MAX) {
        de__glyph_job *j = &c->jobs[index];
        if (j->status == GLYPH_INTEGRATED) {
            fill(f, &j->entry, out);
            found = true;
        } else if (j->status == GLYPH_REJECTED && f->count && f->glyphs[0].index == 0) {
            fill(f, &f->glyphs[0], out);
            found = true;
        }
    }
    de_mutex_unlock(&c->mutex);
    return found;
}
static int compare_jobs(const void *a, const void *b) {
    const de__glyph_job *ja = *(const de__glyph_job *const *)a,
                        *jb = *(const de__glyph_job *const *)b;
    de__glyph_font *fa = &ja->owner->fonts[ja->font], *fb = &jb->owner->fonts[jb->font];
    if (fa->handle.id != fb->handle.id)
        return fa->handle.id < fb->handle.id ? -1 : 1;
    if (ja->glyph != jb->glyph)
        return ja->glyph < jb->glyph ? -1 : 1;
    return ja->font < jb->font ? -1 : ja->font > jb->font;
}
static bool place(struct de__glyph_cache *c, de__glyph_font *f, de__font_glyph *g) {
    if (!g->w || !g->h) {
        g->page = 0;
        return true;
    }
    u32 max = (u32)de_cvar_int(c->pages_max), page, x = 0, y = 0;
    for (page = 0; page < f->pages; ++page)
        if (de_atlas_packer_insert(f->packers[page], g->w, g->h, &x, &y))
            break;
    if (page == f->pages) {
        if (page >= max)
            return false;
        u64 bytes = (u64)f->width * f->height * 4;
        u8 *p = DE_ALLOC(c->alloc, bytes, 1, DE_MEM_UI);
        de_atlas_packer *packer = DE_NEW(c->alloc, de_atlas_packer, DE_MEM_UI);
        if (!p || !packer) {
            if (p)
                DE_FREE(c->alloc, p, bytes);
            if (packer)
                DE_FREE(c->alloc, packer, sizeof *packer);
            return false;
        }
        memset(p, 0, (size_t)bytes);
        de_atlas_packer_init(packer, f->width, f->height, 0);
        if (!de_atlas_packer_insert(packer, g->w, g->h, &x, &y)) {
            DE_FREE(c->alloc, p, bytes);
            DE_FREE(c->alloc, packer, sizeof *packer);
            return false;
        }
        f->pixels[page] = p;
        f->packers[page] = packer;
        ++f->pages;
        ++c->stats.runtime_pages;
    }
    g->page = f->base_pages + page;
    g->x = x;
    g->y = y;
    return true;
}
void de_text__integrate_glyphs(de_text_system *t) {
    struct de__glyph_cache *c = t->glyph_cache;
    if (!c)
        return;
    de__text_lock(t->runtime);
    de_mutex_lock(&c->mutex);
    if (!c->stats.runtime_pending) {
        de_mutex_unlock(&c->mutex);
        de__text_unlock(t->runtime);
        return;
    }
    de__glyph_job *ready[DE_GLYPH_REQUESTS];
    u32 n = 0;
    for (u32 i = 0; i < c->count; ++i)
        if (c->jobs[i].status <= GLYPH_COMPLETE)
            ready[n++] = &c->jobs[i];
    qsort(ready, n, sizeof *ready, compare_jobs);
    u32 limit = (u32)de_cvar_int(c->per_frame), integrated = 0;
    for (u32 i = 0; i < n && integrated < limit; ++i) {
        de__glyph_job *j = ready[i];
        if (j->status == GLYPH_QUEUED)
            break;
        de__glyph_font *f = &c->fonts[j->font];
        de_text_font *live = de_text__font_get(t, f->handle);
        bool success = j->result == DE_OK && live == f->identity && place(c, f, &j->entry);
        if (success) {
            de__font_glyph *g = &j->entry;
            if (g->w && g->h && j->pixels) {
                u32 page = g->page - f->base_pages;
                for (u32 y = 0; y < g->h; ++y)
                    memcpy(f->pixels[page] + 4 * ((u64)(g->y + y) * f->width + g->x),
                           j->pixels + 4 * (u64)y * g->w, 4 * g->w);
                de_text_dirty_rect rect = {g->page, g->x, g->y, g->w, g->h};
                if (live->dirty_count < 256)
                    live->dirty[live->dirty_count++] = rect;
                else {
                    for (u32 k = 0; k < live->dirty_count; ++k)
                        if (live->dirty[k].page == rect.page) {
                            live->dirty[k] =
                                (de_text_dirty_rect){rect.page, 0, 0, live->width, live->height};
                            break;
                        }
                }
            }
            j->status = GLYPH_INTEGRATED;
            ++live->generation;
            ++c->stats.runtime_generated;
        } else {
            j->status = GLYPH_REJECTED;
            if (live == f->identity && !f->warned) {
                f->warned = true;
                ++c->stats.cap_warnings;
                de_log_category log = de_log_register_category("text", DE_LOG_INFO);
                DE_WARN(log, "runtime glyph atlas budget reached for font %u; using .notdef",
                        f->handle.id);
            }
        }
        if (j->pixels) {
            DE_FREE(c->alloc, j->pixels, (u64)j->entry.w * j->entry.h * 4);
            j->pixels = NULL;
        }
        --c->stats.runtime_pending;
        ++integrated;
    }
    de_mutex_unlock(&c->mutex);
    de__text_unlock(t->runtime);
}
de_text_stats de_text_get_stats(const de_text_system *t) {
    de_text_stats stats = {0};
    if (t && t->glyph_cache) {
        struct de__glyph_cache *c = t->glyph_cache;
        de_mutex_lock(&c->mutex);
        stats = c->stats;
        de_mutex_unlock(&c->mutex);
    }
    return stats;
}
u32 de_text__page_count(de_text_system *t, de_font handle) {
    de_text_font *f = de_text__font_get(t, handle);
    if (!f)
        return 0;
    u32 pages = f->pages;
    struct de__glyph_cache *c = t->glyph_cache;
    if (!c)
        return pages;
    de_mutex_lock(&c->mutex);
    u32 i = font_index(c, handle, f, false);
    if (i != UINT32_MAX)
        pages += c->fonts[i].pages;
    de_mutex_unlock(&c->mutex);
    return pages;
}
const u8 *de_text__page_data(de_text_system *t, de_font handle, u32 page) {
    de_text_font *f = de_text__font_get(t, handle);
    if (!f)
        return NULL;
    if (page < f->pages)
        return f->pixels[page];
    struct de__glyph_cache *c = t->glyph_cache;
    if (!c)
        return NULL;
    de_mutex_lock(&c->mutex);
    u32 i = font_index(c, handle, f, false);
    const u8 *p = NULL;
    if (i != UINT32_MAX && page - f->pages < c->fonts[i].pages)
        p = c->fonts[i].pixels[page - f->pages];
    de_mutex_unlock(&c->mutex);
    return p;
}
