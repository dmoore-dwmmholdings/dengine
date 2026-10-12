/*
 * resolve.c: manifest resolution and validation (SPEC §11.1, §11.2; DENG-139).
 *
 * de__manifest_resolve, in this order (each step appends its issues, so the issue order is fixed):
 *   1. copy the manifest's issues;
 *   2. assets, in manifest order: check and hash each ID (invalid -> error);
 *   3. the ID table: valid IDs stably sorted by hash; equal hashes of different paths -> error;
 *   4. bundles, in manifest order: expand `assets` (explicit IDs and globs) to sorted, unique
 *      member IDs; resolve `depends` names (unknown -> error; repeats ignored);
 *   5. cycles: a depth-first walk from each bundle in declaration order, edges in list order;
 *      each edge that closes a cycle is one error with the path, and is left out afterwards;
 *   6. a topological order of all bundles (Kahn; among ready bundles the one declared first),
 *      and each bundle's closure: the bundles it reaches, in that order.
 * de__manifest_validate appends the type and source checks.
 *
 * Memory: two chunk arenas on the resolve allocator. `keep` holds the de__resolved and
 * everything it owns; `scratch` holds temporaries and is released before returning. An
 * allocation failure anywhere releases both (nothing leaks). Nothing iterates a hash map; name
 * and ID lookups are binary searches in sorted arrays.
 */

#include "asset/resolve_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/de_array.h"
#include "core/de_sort.h"
#include "core/de_strbuf.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RS_NONE UINT32_MAX

/* ---- Chunk arena ------------------------------------------------------------------------- */

/* Chunks come from the backing allocator, so a tracking allocator sees all of it. free is a
 * no-op; realloc grows the most recent block in place when it fits (de_array growth). */

typedef struct rs_chunk {
    struct rs_chunk *next;
    size_t cap, used, alloc_size;
} rs_chunk;

enum { RS_CHUNK_HDR = 32, RS_CHUNK_DEFAULT = 16 * 1024 };
DE_STATIC_ASSERT(sizeof(rs_chunk) <= RS_CHUNK_HDR, "rs_chunk header too large");

typedef struct rs_arena {
    const de_allocator *backing;
    rs_chunk *head;
    u8 *last;
} rs_arena;

static u8 *rs_chunk_data(rs_chunk *c) {
    return (u8 *)c + RS_CHUNK_HDR;
}

static void *rs_arena_alloc(void *user, size_t size, size_t align, u32 tag) {
    (void)tag;
    rs_arena *a = (rs_arena *)user;
    if (align == 0)
        align = 1;
    for (int attempt = 0; attempt < 2; ++attempt) {
        rs_chunk *c = a->head;
        if (c != NULL) {
            uintptr_t base = (uintptr_t)rs_chunk_data(c);
            uintptr_t at = (base + c->used + (align - 1)) & ~(uintptr_t)(align - 1);
            size_t off = (size_t)(at - base);
            if (off <= c->cap && size <= c->cap - off) {
                c->used = off + size;
                a->last = (u8 *)at;
                return (void *)at;
            }
        }
        if (attempt == 1)
            break;
        if (size > SIZE_MAX / 2 - align - RS_CHUNK_HDR) {
            DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset resolve arena: %zu bytes is too large", size);
            return NULL;
        }
        size_t cap = size + align > RS_CHUNK_DEFAULT ? size + align : RS_CHUNK_DEFAULT;
        size_t total = cap + RS_CHUNK_HDR;
        rs_chunk *n = (rs_chunk *)DE_ALLOC(a->backing, total, 16, DE_MEM_ASSET);
        if (n == NULL)
            return NULL;
        n->next = a->head;
        n->cap = cap;
        n->used = 0;
        n->alloc_size = total;
        a->head = n;
    }
    return NULL;
}

static void *rs_arena_realloc(void *user, void *ptr, size_t old_size, size_t new_size, size_t align,
                              u32 tag) {
    rs_arena *a = (rs_arena *)user;
    if (ptr == NULL)
        return rs_arena_alloc(user, new_size, align, tag);
    if ((u8 *)ptr == a->last && a->head != NULL) {
        rs_chunk *c = a->head;
        size_t off = (size_t)((u8 *)ptr - rs_chunk_data(c));
        if (new_size <= c->cap - off) {
            c->used = off + new_size;
            return ptr;
        }
    }
    void *n = rs_arena_alloc(user, new_size, align, tag);
    if (n == NULL)
        return NULL;
    memcpy(n, ptr, old_size < new_size ? old_size : new_size);
    return n;
}

static void rs_arena_free(void *user, void *ptr, size_t size) {
    (void)user;
    (void)ptr;
    (void)size;
}

/* Gives every chunk back (the arena struct itself is the caller's). */
static void rs_arena_clear(rs_arena *a) {
    rs_chunk *c = a->head;
    while (c != NULL) {
        rs_chunk *next = c->next;
        DE_FREE(a->backing, c, c->alloc_size);
        c = next;
    }
    a->head = NULL;
    a->last = NULL;
}

static de_allocator rs_arena_allocator(rs_arena *a) {
    return (de_allocator){rs_arena_alloc, rs_arena_realloc, rs_arena_free, a};
}

/* ---- State ------------------------------------------------------------------------------- */

/* The private part of a de__resolved (impl_). Lives in its own `keep` arena. */
typedef struct rs_impl {
    rs_arena *keep;     /* allocated from the backing allocator */
    de_allocator ka;    /* allocator on `keep` */
    de_array issues;    /* de__manifest_issue, on `ka` */
    const u32 *by_name; /* bundle indices sorted by name (strcmp) */
} rs_impl;

typedef struct rs_state {
    de__resolved *r;
    rs_impl *impl;
    const de__manifest *m;
    rs_arena scratch;
    de_allocator sa; /* allocator on `scratch` */
    u64 (*hash_override)(const char *path, size_t len);
} rs_state;

/* ---- Issues ------------------------------------------------------------------------------ */

static bool rs_issue(de__resolved *r, de_asset_issue_severity sev, const char *file,
                     const char *key_path, u32 line, const char *fmt, ...) DE_PRINTF_FMT(6, 7);

static bool rs_issue(de__resolved *r, de_asset_issue_severity sev, const char *file,
                     const char *key_path, u32 line, const char *fmt, ...) {
    rs_impl *impl = (rs_impl *)r->impl_;
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0)
        n = 0;
    char *msg = (char *)DE_ALLOC(&impl->ka, (size_t)n + 1, 1, DE_MEM_ASSET);
    if (msg != NULL)
        vsnprintf(msg, (size_t)n + 1, fmt, args);
    va_end(args);
    size_t kp_len = strlen(key_path);
    char *kp = (char *)DE_ALLOC(&impl->ka, kp_len + 1, 1, DE_MEM_ASSET);
    if (msg == NULL || kp == NULL)
        return false;
    memcpy(kp, key_path, kp_len + 1);
    de__manifest_issue is = {
        .severity = sev,
        .code = DE_ERR_PARSE,
        .file = file != NULL ? file : "",
        .key_path = kp,
        .line = line,
        .message = msg,
    };
    if (de_array_push(&impl->issues, &is) != DE_OK)
        return false;
    if (sev == DE_ASSET_ISSUE_ERROR)
        r->error_count++;
    else if (sev == DE_ASSET_ISSUE_WARNING)
        r->warning_count++;
    else
        r->info_count++;
    r->issues = (const de__manifest_issue *)impl->issues.data;
    r->issue_count = impl->issues.count;
    return true;
}

static bool rs_is_bare_key(const char *k) {
    if (*k == '\0')
        return false;
    for (; *k != '\0'; ++k) {
        char c = *k;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return false;
    }
    return true;
}

/* Appends `.key` (or just `key` at the start), quoted as TOML needs it, like the manifest
 * parser's key paths (`assets."ui/logo".source`). */
static bool rs_kp_key(de_strbuf *b, const char *key) {
    if (b->len != 0 && de_strbuf_append_char(b, '.') != DE_OK)
        return false;
    if (rs_is_bare_key(key))
        return de_strbuf_append_cstr(b, key) == DE_OK;
    if (de_strbuf_append_char(b, '"') != DE_OK)
        return false;
    for (const unsigned char *s = (const unsigned char *)key; *s != '\0'; ++s) {
        de_result res;
        if (*s == '"' || *s == '\\')
            res = de_strbuf_appendf(b, "\\%c", (char)*s);
        else if (*s < 0x20 || *s == 0x7f)
            res = de_strbuf_appendf(b, "\\u%04X", (unsigned)*s);
        else
            res = de_strbuf_append_char(b, (char)*s);
        if (res != DE_OK)
            return false;
    }
    return de_strbuf_append_char(b, '"') == DE_OK;
}

/* "<section>.<name>[.<field>[.<sub>]]" in `b` (cleared first). NULL on allocation failure. */
static const char *rs_kp(de_strbuf *b, const char *section, const char *name, const char *field,
                         const char *sub) {
    de_strbuf_clear(b);
    if (!rs_kp_key(b, section) || !rs_kp_key(b, name))
        return NULL;
    if (field != NULL && !rs_kp_key(b, field))
        return NULL;
    if (sub != NULL && !rs_kp_key(b, sub))
        return NULL;
    return de_strbuf_cstr(b);
}

/* ---- IDs --------------------------------------------------------------------------------- */

static u64 rs_hash(const rs_state *st, const char *path) {
    size_t len = strlen(path);
    return st->hash_override != NULL ? st->hash_override(path, len) : de_hash64(path, len, 0);
}

static int rs_entry_cmp(const void *a, const void *b, void *user) {
    (void)user;
    u64 x = ((const de__resolved_entry *)a)->id.hash, y = ((const de__resolved_entry *)b)->id.hash;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int rs_id_cmp(const void *a, const void *b, void *user) {
    (void)user;
    u64 x = ((const de_asset_id *)a)->hash, y = ((const de_asset_id *)b)->hash;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Index of the first row with `hash` (or of the first row after it). */
static u32 rs_lower_bound(const de__resolved *r, u64 hash) {
    u32 lo = 0, hi = r->id_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (r->ids[mid].id.hash < hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* The asset declared with exactly the path `path` (whose ID is `id`), or RS_NONE. */
static u32 rs_lookup_path(const de__resolved *r, de_asset_id id, const char *path) {
    for (u32 i = rs_lower_bound(r, id.hash); i < r->id_count && r->ids[i].id.hash == id.hash; ++i)
        if (strcmp(r->manifest->assets[r->ids[i].asset].id, path) == 0)
            return r->ids[i].asset;
    return RS_NONE;
}

static bool rs_assets(rs_state *st, de_strbuf *kp) {
    const de__manifest *m = st->m;
    de__resolved *r = st->r;
    de_asset_id *ids = NULL;
    de__resolved_entry *table = NULL;
    if (m->asset_count != 0) {
        ids = (de_asset_id *)DE_ALLOC(&st->impl->ka, sizeof *ids * m->asset_count,
                                      DE_ALIGNOF(de_asset_id), DE_MEM_ASSET);
        table = (de__resolved_entry *)DE_ALLOC(&st->impl->ka, sizeof *table * m->asset_count,
                                               DE_ALIGNOF(de__resolved_entry), DE_MEM_ASSET);
        if (ids == NULL || table == NULL)
            return false;
    }
    u32 n = 0;
    for (u32 i = 0; i < m->asset_count; ++i) {
        const de__manifest_asset *a = &m->assets[i];
        const char *why = de__asset_id_check(a->id, strlen(a->id));
        if (why != NULL) {
            ids[i] = (de_asset_id){0};
            const char *k = rs_kp(kp, "assets", a->id, NULL, NULL);
            if (k == NULL || !rs_issue(r, DE_ASSET_ISSUE_ERROR, a->file, k, a->line,
                                       "invalid asset ID \"%s\": %s", a->id, why))
                return false;
            continue;
        }
        ids[i] = de__asset_id_make(rs_hash(st, a->id));
        table[n++] = (de__resolved_entry){ids[i], i};
    }
    if (n > 1 && de_sort_stable(table, n, sizeof *table, rs_entry_cmp, NULL, &st->sa) != DE_OK)
        return false;
    r->asset_ids = ids;
    r->ids = table;
    r->id_count = n;

    /* Collisions: rows with one hash are adjacent, in manifest order (stable sort). */
    for (u32 i = 1; i < n; ++i) {
        if (table[i].id.hash != table[i - 1].id.hash)
            continue;
        u32 first = i - 1;
        while (first > 0 && table[first - 1].id.hash == table[i].id.hash)
            --first;
        const de__manifest_asset *a = &m->assets[table[i].asset];
        const de__manifest_asset *f = &m->assets[table[first].asset];
        const char *k = rs_kp(kp, "assets", a->id, NULL, NULL);
        if (k == NULL ||
            !rs_issue(r, DE_ASSET_ISSUE_ERROR, a->file, k, a->line,
                      "asset IDs \"%s\" and \"%s\" (%s, line %u) have the same hash "
                      "0x%016llx; rename one of them",
                      a->id, f->id, f->file, f->line, (unsigned long long)table[i].id.hash))
            return false;
    }
    return true;
}

/* ---- Bundles ----------------------------------------------------------------------------- */

static int rs_name_cmp(const void *a, const void *b, void *user) {
    const de__manifest *m = (const de__manifest *)user;
    return strcmp(m->bundles[*(const u32 *)a].name, m->bundles[*(const u32 *)b].name);
}

static u32 rs_find_bundle(const de__manifest *m, const u32 *by_name, const char *name) {
    u32 lo = 0, hi = m->bundle_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        int c = strcmp(m->bundles[by_name[mid]].name, name);
        if (c == 0)
            return by_name[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return RS_NONE;
}

/* Expands bundle `bi`'s `assets` into sorted, unique member IDs. `scratch` is a reused
 * de_array of de_asset_id. */
static bool rs_members(rs_state *st, u32 bi, de_array *scratch, de_strbuf *kp) {
    const de__manifest *m = st->m;
    de__resolved *r = st->r;
    const de__manifest_bundle *b = &m->bundles[bi];
    de__resolved_bundle *rb = (de__resolved_bundle *)(void *)&r->bundles[bi];
    de_array_clear(scratch);
    for (u32 e = 0; e < b->asset_count; ++e) {
        const char *entry = b->assets[e];
        if (de__glob_is_pattern(entry)) {
            u32 matched = 0;
            for (u32 i = 0; i < m->asset_count; ++i) {
                if (r->asset_ids[i].hash == 0 || !de__glob_match(entry, m->assets[i].id))
                    continue;
                if (de_array_push(scratch, &r->asset_ids[i]) != DE_OK)
                    return false;
                ++matched;
            }
            if (matched == 0) {
                const char *k = rs_kp(kp, "bundles", b->name, "assets", NULL);
                if (k == NULL ||
                    !rs_issue(r, DE_ASSET_ISSUE_WARNING, b->file, k, b->line,
                              "bundle \"%s\": glob \"%s\" matches no asset", b->name, entry))
                    return false;
            }
            continue;
        }
        const char *why = de__asset_id_check(entry, strlen(entry));
        const char *k = rs_kp(kp, "bundles", b->name, "assets", NULL);
        if (k == NULL)
            return false;
        if (why != NULL) {
            if (!rs_issue(r, DE_ASSET_ISSUE_ERROR, b->file, k, b->line,
                          "bundle \"%s\": \"%s\" is not a valid asset ID: %s", b->name, entry, why))
                return false;
            continue;
        }
        de_asset_id id = de__asset_id_make(rs_hash(st, entry));
        if (rs_lookup_path(r, id, entry) == RS_NONE) {
            const char *sub = strchr(entry, '#');
            bool ok;
            if (sub != NULL)
                ok = rs_issue(r, DE_ASSET_ISSUE_ERROR, b->file, k, b->line,
                              "bundle \"%s\": \"%s\" is a sub-asset; list its asset \"%.*s\" "
                              "instead",
                              b->name, entry, (int)(sub - entry), entry);
            else
                ok = rs_issue(r, DE_ASSET_ISSUE_ERROR, b->file, k, b->line,
                              "bundle \"%s\": asset \"%s\" is not declared in [assets]", b->name,
                              entry);
            if (!ok)
                return false;
            continue;
        }
        if (de_array_push(scratch, &id) != DE_OK)
            return false;
    }

    de_asset_id *ids = (de_asset_id *)scratch->data;
    u32 n = scratch->count;
    if (n > 1 && de_sort_stable(ids, n, sizeof *ids, rs_id_cmp, NULL, &st->sa) != DE_OK)
        return false;
    u32 unique = 0;
    for (u32 i = 0; i < n; ++i)
        if (unique == 0 || ids[unique - 1].hash != ids[i].hash)
            ids[unique++] = ids[i];
    rb->members = NULL;
    rb->member_count = unique;
    if (unique != 0) {
        de_asset_id *out = (de_asset_id *)DE_ALLOC(&st->impl->ka, sizeof *out * unique,
                                                   DE_ALIGNOF(de_asset_id), DE_MEM_ASSET);
        if (out == NULL)
            return false;
        memcpy(out, ids, sizeof *out * unique);
        rb->members = out;
    }
    return true;
}

/* The dependency graph: bundle u depends on dep[off[u] .. off[u + 1]) (known names only, first
 * occurrence of each, in list order). back[e] marks an edge that closes a cycle. */
typedef struct rs_graph {
    u32 *off; /* bundle_count + 1 */
    u32 *dep;
    bool *back;
} rs_graph;

static bool rs_depends(rs_state *st, const u32 *by_name, rs_graph *g, de_strbuf *kp) {
    const de__manifest *m = st->m;
    u32 nb = m->bundle_count;
    size_t edges = 0;
    for (u32 i = 0; i < nb; ++i)
        edges += m->bundles[i].depends_count;
    g->off =
        (u32 *)DE_ALLOC(&st->sa, sizeof(u32) * ((size_t)nb + 1), DE_ALIGNOF(u32), DE_MEM_ASSET);
    g->dep = (u32 *)DE_ALLOC(&st->sa, sizeof(u32) * (edges + 1), DE_ALIGNOF(u32), DE_MEM_ASSET);
    g->back = (bool *)DE_ALLOC(&st->sa, edges + 1, 1, DE_MEM_ASSET);
    if (g->off == NULL || g->dep == NULL || g->back == NULL)
        return false;
    memset(g->back, 0, edges + 1);
    u32 n = 0;
    for (u32 i = 0; i < nb; ++i) {
        const de__manifest_bundle *b = &m->bundles[i];
        g->off[i] = n;
        for (u32 d = 0; d < b->depends_count; ++d) {
            u32 target = rs_find_bundle(m, by_name, b->depends[d]);
            if (target == RS_NONE) {
                const char *k = rs_kp(kp, "bundles", b->name, "depends", NULL);
                if (k == NULL || !rs_issue(st->r, DE_ASSET_ISSUE_ERROR, b->file, k, b->line,
                                           "bundle \"%s\" depends on unknown bundle \"%s\"",
                                           b->name, b->depends[d]))
                    return false;
                continue;
            }
            bool seen = false;
            for (u32 e = g->off[i]; e < n && !seen; ++e)
                seen = g->dep[e] == target;
            if (!seen)
                g->dep[n++] = target;
        }
    }
    g->off[nb] = n;
    return true;
}

/* Depth-first walk (iterative, so a long chain cannot overflow the stack). */
static bool rs_cycles(rs_state *st, rs_graph *g, de_strbuf *kp, de_strbuf *path) {
    const de__manifest *m = st->m;
    u32 nb = m->bundle_count;
    if (nb == 0)
        return true;
    u8 *color = (u8 *)DE_ALLOC(&st->sa, nb, 1, DE_MEM_ASSET); /* 0 new, 1 on stack, 2 done */
    u32 *stack = (u32 *)DE_ALLOC(&st->sa, sizeof(u32) * nb, DE_ALIGNOF(u32), DE_MEM_ASSET);
    u32 *next = (u32 *)DE_ALLOC(&st->sa, sizeof(u32) * nb, DE_ALIGNOF(u32), DE_MEM_ASSET);
    u32 *pos = (u32 *)DE_ALLOC(&st->sa, sizeof(u32) * nb, DE_ALIGNOF(u32), DE_MEM_ASSET);
    if (color == NULL || stack == NULL || next == NULL || pos == NULL)
        return false;
    memset(color, 0, nb);
    for (u32 root = 0; root < nb; ++root) {
        if (color[root] != 0)
            continue;
        u32 depth = 0;
        stack[depth] = root;
        next[root] = g->off[root];
        pos[root] = depth++;
        color[root] = 1;
        while (depth != 0) {
            u32 u = stack[depth - 1];
            if (next[u] == g->off[u + 1]) {
                color[u] = 2;
                --depth;
                continue;
            }
            u32 e = next[u]++;
            u32 v = g->dep[e];
            if (color[v] == 0) {
                color[v] = 1;
                next[v] = g->off[v];
                pos[v] = depth;
                stack[depth++] = v;
            } else if (color[v] == 1) {
                g->back[e] = true;
                de_strbuf_clear(path);
                for (u32 i = pos[v]; i < depth; ++i)
                    if (de_strbuf_appendf(path, "%s -> ", m->bundles[stack[i]].name) != DE_OK)
                        return false;
                if (de_strbuf_append_cstr(path, m->bundles[v].name) != DE_OK)
                    return false;
                const de__manifest_bundle *b = &m->bundles[u];
                const char *k = rs_kp(kp, "bundles", b->name, "depends", NULL);
                if (k == NULL || !rs_issue(st->r, DE_ASSET_ISSUE_ERROR, b->file, k, b->line,
                                           "bundle dependency cycle: %s", de_strbuf_cstr(path)))
                    return false;
            }
        }
    }
    return true;
}

/* Min-heap of bundle indices (declaration order). */
static void rs_heap_push(u32 *heap, u32 *n, u32 v) {
    u32 i = (*n)++;
    heap[i] = v;
    while (i > 0) {
        u32 parent = (i - 1) / 2;
        if (heap[parent] <= heap[i])
            break;
        u32 t = heap[parent];
        heap[parent] = heap[i];
        heap[i] = t;
        i = parent;
    }
}

static u32 rs_heap_pop(u32 *heap, u32 *n) {
    u32 top = heap[0];
    heap[0] = heap[--(*n)];
    u32 i = 0;
    for (;;) {
        u32 l = 2 * i + 1, rr = l + 1, s = i;
        if (l < *n && heap[l] < heap[s])
            s = l;
        if (rr < *n && heap[rr] < heap[s])
            s = rr;
        if (s == i)
            break;
        u32 t = heap[s];
        heap[s] = heap[i];
        heap[i] = t;
        i = s;
    }
    return top;
}

static int rs_rank_cmp(const void *a, const void *b, void *user) {
    const u32 *rank = (const u32 *)user;
    u32 x = rank[*(const u32 *)a], y = rank[*(const u32 *)b];
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Ranks every bundle (Kahn on the graph without back edges; ready bundles in declaration order),
 * then gives each bundle the bundles it reaches, sorted by rank. */
static bool rs_closure(rs_state *st, const rs_graph *g) {
    const de__manifest *m = st->m;
    u32 nb = m->bundle_count;
    if (nb == 0)
        return true;
    u32 ne = g->off[nb];
    u32 *indeg = (u32 *)DE_ALLOC(&st->sa, sizeof(u32) * nb, DE_ALIGNOF(u32), DE_MEM_ASSET);
    u32 *roff =
        (u32 *)DE_ALLOC(&st->sa, sizeof(u32) * ((size_t)nb + 1), DE_ALIGNOF(u32), DE_MEM_ASSET);
    u32 *rdep =
        (u32 *)DE_ALLOC(&st->sa, sizeof(u32) * ((size_t)ne + 1), DE_ALIGNOF(u32), DE_MEM_ASSET);
    u32 *heap = (u32 *)DE_ALLOC(&st->sa, sizeof(u32) * nb, DE_ALIGNOF(u32), DE_MEM_ASSET);
    u32 *rank = (u32 *)DE_ALLOC(&st->sa, sizeof(u32) * nb, DE_ALIGNOF(u32), DE_MEM_ASSET);
    u32 *stamp = (u32 *)DE_ALLOC(&st->sa, sizeof(u32) * nb, DE_ALIGNOF(u32), DE_MEM_ASSET);
    u32 *list = (u32 *)DE_ALLOC(&st->sa, sizeof(u32) * nb, DE_ALIGNOF(u32), DE_MEM_ASSET);
    if (indeg == NULL || roff == NULL || rdep == NULL || heap == NULL || rank == NULL ||
        stamp == NULL || list == NULL)
        return false;

    /* Reverse edges (dependency -> dependents), by counting. */
    memset(roff, 0, sizeof(u32) * ((size_t)nb + 1));
    for (u32 u = 0; u < nb; ++u) {
        indeg[u] = 0;
        for (u32 e = g->off[u]; e < g->off[u + 1]; ++e) {
            if (g->back[e])
                continue;
            indeg[u]++;
            roff[g->dep[e] + 1]++;
        }
    }
    for (u32 v = 0; v < nb; ++v)
        roff[v + 1] += roff[v];
    for (u32 v = 0; v < nb; ++v)
        stamp[v] = roff[v]; /* fill cursor */
    for (u32 u = 0; u < nb; ++u)
        for (u32 e = g->off[u]; e < g->off[u + 1]; ++e)
            if (!g->back[e])
                rdep[stamp[g->dep[e]]++] = u;

    u32 hn = 0, k = 0;
    for (u32 u = 0; u < nb; ++u)
        if (indeg[u] == 0)
            rs_heap_push(heap, &hn, u);
    while (hn != 0) {
        u32 v = rs_heap_pop(heap, &hn);
        rank[v] = k++;
        for (u32 e = roff[v]; e < roff[v + 1]; ++e)
            if (--indeg[rdep[e]] == 0)
                rs_heap_push(heap, &hn, rdep[e]);
    }
    DE_ASSERT(k == nb); /* the graph without back edges is acyclic */

    for (u32 v = 0; v < nb; ++v)
        stamp[v] = RS_NONE;
    for (u32 b = 0; b < nb; ++b) {
        /* Reachable set: `list` doubles as the DFS stack (each bundle enters it once). */
        u32 n = 0, head = 0;
        stamp[b] = b;
        list[n++] = b;
        while (head < n) {
            u32 u = list[head++];
            for (u32 e = g->off[u]; e < g->off[u + 1]; ++e) {
                u32 v = g->dep[e];
                if (g->back[e] || stamp[v] == b)
                    continue;
                stamp[v] = b;
                list[n++] = v;
            }
        }
        de__resolved_bundle *rb = (de__resolved_bundle *)(void *)&st->r->bundles[b];
        u32 count = n - 1; /* without b itself (list[0]) */
        rb->depends = NULL;
        rb->depends_count = count;
        if (count == 0)
            continue;
        u32 *out =
            (u32 *)DE_ALLOC(&st->impl->ka, sizeof(u32) * count, DE_ALIGNOF(u32), DE_MEM_ASSET);
        if (out == NULL)
            return false;
        memcpy(out, list + 1, sizeof(u32) * count);
        if (count > 1 &&
            de_sort_stable(out, count, sizeof(u32), rs_rank_cmp, rank, &st->sa) != DE_OK)
            return false;
        rb->depends = out;
    }
    return true;
}

static bool rs_bundles(rs_state *st, de_strbuf *kp, de_strbuf *path) {
    const de__manifest *m = st->m;
    de__resolved *r = st->r;
    u32 nb = m->bundle_count;
    if (nb == 0)
        return true;
    de__resolved_bundle *bundles = (de__resolved_bundle *)DE_ALLOC(
        &st->impl->ka, sizeof *bundles * nb, DE_ALIGNOF(de__resolved_bundle), DE_MEM_ASSET);
    u32 *by_name = (u32 *)DE_ALLOC(&st->impl->ka, sizeof(u32) * nb, DE_ALIGNOF(u32), DE_MEM_ASSET);
    if (bundles == NULL || by_name == NULL)
        return false;
    memset(bundles, 0, sizeof *bundles * nb);
    for (u32 i = 0; i < nb; ++i) {
        bundles[i].bundle = &m->bundles[i];
        by_name[i] = i;
    }
    if (nb > 1 && de_sort_stable(by_name, nb, sizeof(u32), rs_name_cmp, (void *)(uintptr_t)m,
                                 &st->sa) != DE_OK)
        return false;
    r->bundles = bundles;
    r->bundle_count = nb;
    st->impl->by_name = by_name;

    de_array scratch;
    DE_ARRAY_INIT_TAG(&scratch, de_asset_id, &st->sa, DE_MEM_ASSET);
    for (u32 i = 0; i < nb; ++i)
        if (!rs_members(st, i, &scratch, kp))
            return false;
    rs_graph g;
    return rs_depends(st, by_name, &g, kp) && rs_cycles(st, &g, kp, path) && rs_closure(st, &g);
}

/* ---- Entry points ------------------------------------------------------------------------ */

/* Creates an empty de__resolved with its keep arena. NULL on allocation failure. */
static de__resolved *rs_create(const de__manifest *m, const de_allocator *a) {
    rs_arena *keep = DE_NEW(a, rs_arena, DE_MEM_ASSET);
    if (keep == NULL)
        return NULL;
    memset(keep, 0, sizeof *keep);
    keep->backing = a;
    de_allocator ka = rs_arena_allocator(keep);
    rs_impl *impl = DE_NEW(&ka, rs_impl, DE_MEM_ASSET);
    de__resolved *r = DE_NEW(&ka, de__resolved, DE_MEM_ASSET);
    if (impl == NULL || r == NULL) {
        rs_arena_clear(keep);
        DE_FREE(a, keep, sizeof *keep);
        return NULL;
    }
    memset(impl, 0, sizeof *impl);
    memset(r, 0, sizeof *r);
    impl->keep = keep;
    impl->ka = ka;
    DE_ARRAY_INIT_TAG(&impl->issues, de__manifest_issue, &impl->ka, DE_MEM_ASSET);
    r->impl_ = impl;
    r->manifest = m;
    return r;
}

void de__resolved_destroy(de__resolved *r) {
    if (r == NULL)
        return;
    rs_arena *keep = ((rs_impl *)r->impl_)->keep;
    const de_allocator *a = keep->backing;
    rs_arena_clear(keep); /* r and impl live in it */
    DE_FREE(a, keep, sizeof *keep);
}

de_result de__manifest_resolve(const de__manifest *m, const de__resolve_desc *d,
                               de__resolved **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__manifest_resolve: out is NULL");
    *out = NULL;
    if (m == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__manifest_resolve: manifest is NULL");
    const de_allocator *a = d != NULL && d->allocator != NULL ? d->allocator : de_heap_allocator();

    de__resolved *r = rs_create(m, a);
    if (r == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "asset manifest %s: out of memory resolving",
                            m->root);
    rs_state st;
    memset(&st, 0, sizeof st);
    st.r = r;
    st.impl = (rs_impl *)r->impl_;
    st.m = m;
    st.scratch.backing = a;
    st.sa = rs_arena_allocator(&st.scratch);
    st.hash_override = d != NULL ? d->hash_override : NULL;

    de_strbuf kp, path;
    de_strbuf_init(&kp, &st.sa);
    de_strbuf_init(&path, &st.sa);

    /* The manifest's issues come first; resolution appends after them. */
    bool ok = m->issue_count == 0 ||
              de_array_push_n(&st.impl->issues, m->issues, m->issue_count) == DE_OK;
    if (ok) {
        r->issues = (const de__manifest_issue *)st.impl->issues.data;
        r->issue_count = st.impl->issues.count;
        r->error_count = m->error_count;
        r->warning_count = m->warning_count;
        r->info_count = m->info_count;
    }
    ok = ok && rs_assets(&st, &kp) && rs_bundles(&st, &kp, &path);

    rs_arena_clear(&st.scratch); /* kp and path live in it */
    if (!ok) {
        de__resolved_destroy(r);
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "asset manifest %s: out of memory resolving",
                            m->root);
    }
    *out = r;
    return DE_OK;
}

/* ---- Validation -------------------------------------------------------------------------- */

static bool rs_default_exists(void *user, const char *vpath) {
    (void)user;
    return de_path_exists(vpath);
}

/* Checks one source path; appends an error if it is invalid or missing. */
static bool rs_check_source(de__resolved *r, const de__validate_desc *d, const char *dir,
                            const de__manifest_asset *a, const char *source, const char *key,
                            de_strbuf *buf) {
    char norm[DE_FS_PATH_MAX];
    de_strbuf_clear(buf);
    if (de_strbuf_appendf(buf, "%s/%s", dir, source) != DE_OK)
        return false;
    if (de_path_normalize(de_strbuf_cstr(buf), norm, sizeof norm) != DE_OK)
        return rs_issue(r, DE_ASSET_ISSUE_ERROR, a->file, key, a->line,
                        "asset \"%s\": source \"%s\" is not a valid path under %s", a->id, source,
                        dir);
    bool (*exists)(void *, const char *) = d->exists != NULL ? d->exists : rs_default_exists;
    if (!exists(d->exists_user, norm))
        return rs_issue(r, DE_ASSET_ISSUE_ERROR, a->file, key, a->line,
                        "asset \"%s\": source file %s does not exist", a->id, norm);
    return true;
}

de_result de__manifest_validate(de__resolved *r, const de__validate_desc *desc) {
    if (r == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__manifest_validate: resolved is NULL");
    de__validate_desc d = desc != NULL ? *desc : (de__validate_desc){0};
    const de__manifest *m = r->manifest;
    rs_impl *impl = (rs_impl *)r->impl_;

    rs_arena scratch;
    memset(&scratch, 0, sizeof scratch);
    scratch.backing = impl->keep->backing;
    de_allocator sa = rs_arena_allocator(&scratch);
    de_strbuf kp, buf, dir;
    de_strbuf_init(&kp, &sa);
    de_strbuf_init(&buf, &sa);
    de_strbuf_init(&dir, &sa);
    bool ok = true;

    /* The source directory: given, or `asset_root` next to the root manifest. */
    bool check_sources = !d.skip_sources;
    if (check_sources) {
        if (d.source_dir != NULL) {
            ok = de_strbuf_append_cstr(&dir, d.source_dir) == DE_OK;
        } else {
            const char *slash = strrchr(m->root, '/');
            size_t n = slash != NULL ? (size_t)(slash - m->root) : 0;
            ok = de_strbuf_append(&dir, (de_str){m->root, n}) == DE_OK &&
                 (m->asset_root[0] == '\0' ||
                  de_strbuf_appendf(&dir, "/%s", m->asset_root) == DE_OK);
        }
    }

    for (u32 i = 0; ok && i < m->asset_count; ++i) {
        const de__manifest_asset *a = &m->assets[i];
        if (d.known_types != NULL) {
            bool known = false;
            for (u32 t = 0; t < d.known_type_count && !known; ++t)
                known = d.known_types[t] != NULL && strcmp(d.known_types[t], a->type) == 0;
            if (!known) {
                const char *k = rs_kp(&kp, "assets", a->id, "type", NULL);
                ok = k != NULL &&
                     rs_issue(r, DE_ASSET_ISSUE_ERROR, a->file, k, a->line,
                              "asset \"%s\": unknown asset type \"%s\"", a->id, a->type);
            }
        }
        if (!ok || !check_sources)
            continue;
        const char *k = rs_kp(&kp, "assets", a->id, "source", NULL);
        ok = k != NULL && rs_check_source(r, &d, de_strbuf_cstr(&dir), a, a->source, k, &buf);
        for (u32 l = 0; ok && l < a->locale_count; ++l) {
            k = rs_kp(&kp, "assets", a->id, "locale", a->locales[l].locale);
            ok = k != NULL &&
                 rs_check_source(r, &d, de_strbuf_cstr(&dir), a, a->locales[l].source, k, &buf);
        }
    }

    rs_arena_clear(&scratch);
    if (!ok)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "asset manifest %s: out of memory validating",
                            m->root);
    return DE_OK;
}

/* ---- Lookups ----------------------------------------------------------------------------- */

const de__resolved_entry *de__resolved_find(const de__resolved *r, de_asset_id id) {
    if (r == NULL || id.hash == 0)
        return NULL;
    u32 i = rs_lower_bound(r, id.hash);
    return i < r->id_count && r->ids[i].id.hash == id.hash ? &r->ids[i] : NULL;
}

const de__manifest_asset *de__resolved_asset(const de__resolved *r, de_asset_id id) {
    const de__resolved_entry *e = de__resolved_find(r, id);
    return e != NULL ? &r->manifest->assets[e->asset] : NULL;
}

const char *de__resolved_path(const de__resolved *r, de_asset_id id) {
    const de__manifest_asset *a = de__resolved_asset(r, id);
    return a != NULL ? a->id : NULL;
}

const de__resolved_bundle *de__resolved_find_bundle(const de__resolved *r, const char *name) {
    if (r == NULL || name == NULL || r->bundle_count == 0)
        return NULL;
    u32 i = rs_find_bundle(r->manifest, ((const rs_impl *)r->impl_)->by_name, name);
    return i != RS_NONE ? &r->bundles[i] : NULL;
}
