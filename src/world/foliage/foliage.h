/* foliage.h: the CPU foliage model behind the DIDE editor IPC world.foliage, protocol 1.22
 * (DENG-581). Include as "world/foliage/foliage.h". Internal to the engine.
 *
 * The model is the one of the DIDE mock engine (dide mock-engine/src/foliage.rs). The palette of
 * foliageType assets (.foliage.toml) and the scatter rules are on DeFoliage (de_render.h); the
 * instances are in a de_foliage_store, kept per DE_FOLIAGE_CELL_SIZE-metre cell (DENG-823: on the
 * heap, loaded from and written back to the layer's DeFoliagePage entities by the caller).
 *
 *   - Cell (x, z) of a point is (floor(x / 16), floor(z / 16)). Cells are sorted by z, then x.
 *   - A paint stroke is a list of dabs [x, y, z, pressure]. Each dab draws
 *     min(ceil(max(density * strength * pressure, 0) * pi * radius^2), 65536) candidates, uniform
 *     in the disc, from a splitmix64 generator seeded seed ^ ((dab + 1) << 32) ^ type. A
 *     candidate with weight w = strength * falloff(r / radius) * pressure is kept with chance w,
 *     lands on the highest surface, and is placed when the type's slope and height ranges hold and
 *     no instance of its type is closer than max(minSpacing, 0.5 / sqrt(max(density * w, 1e-6))).
 *   - An erase stroke removes an instance (of the stroke's type, when it has one) with chance w,
 *     the largest dab weight at the instance, from a generator seeded by the instance position.
 *   - A scatter removes the instances of the chosen rules, then re-runs each enabled one over its
 *     area on its terrain with its seed, so a re-run gives the same placement and painted
 *     instances (rule 0) stay.
 *   - A range with max <= min is unbounded.
 *
 * A cell holds at most DE_FOLIAGE_CELL_INSTANCES and the store DE_FOLIAGE_LAYER_INSTANCES: a
 * candidate that finds no room (or no memory) is skipped, as one too close to another is.
 *
 * The module is DETERMINISTIC (SPEC section 5.7): all math is de_dmath, so an op gives the same
 * bytes on every platform. */
#ifndef DE_WORLD_FOLIAGE_H
#define DE_WORLD_FOLIAGE_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>

#include <stddef.h>

DE_EXTERN_C_BEGIN

#define DE_FOLIAGE_COMPONENT  "DeFoliage"
#define DE_FOLIAGE_PAGE_COMPONENT "DeFoliagePage"
#define DE_FOLIAGE_ASSET_TYPE "foliageType"
#define DE_FOLIAGE_SUFFIX     ".foliage.toml"
#define DE_FOLIAGE_CAPABILITY "world.foliage"
/** Numbers per instance in undo data: x, y, z, yaw, scale, type, rule. */
#define DE_FOLIAGE_STRIDE 7u
/** Most candidates one dab or one rule draws. */
#define DE_FOLIAGE_MAX_CANDIDATES 65536u
#define DE_FOLIAGE_ID_MAX         128u

/** A foliageType asset, parsed. */
typedef struct de_foliage_type {
    char id[DE_FOLIAGE_ID_MAX];
    char mesh[DE_FOLIAGE_ID_MAX];
    bool has_impostor;
    char impostor[DE_FOLIAGE_ID_MAX];
    bool has_impostor_distance;
    f32 impostor_distance;
    f32 density; /**< instances per square metre at full strength */
    f32 scale[2], rotation[2];
    bool align_to_normal;
    f32 slope[2], height[2];
    f32 min_spacing;
    bool collision;
    f32 cull_distance;
} de_foliage_type;

/** Error kinds of the model. */
typedef enum de_foliage_error {
    DE_FOLIAGE_OK = 0,
    DE_FOLIAGE_INVALID,  /**< bad params */
    DE_FOLIAGE_NOT_FOUND /**< a named entity or asset is missing */
} de_foliage_error;

/** Parses a .foliage.toml (`n` bytes) as type `id`. False with a message in `msg`. */
bool de__foliage_parse_type(const char *id, const char *text, size_t n, de_foliage_type *out,
                            char *msg, size_t cap);

/** A surface foliage lands on: a terrain at origin (ox, oy, oz), or (terrain NULL) a horizontal
 * plane at height oy. */
typedef struct de_foliage_surface {
    const DeTerrain *terrain;
    f64 ox, oy, oz;
} de_foliage_surface;

/** What the caller resolves: the palette type of a slot, and the terrain of a rule's entity. Both
 * return DE_FOLIAGE_OK or an error kind with a message in `msg`. */
typedef struct de_foliage_env {
    void *user;
    de_foliage_error (*type_of)(void *user, u32 slot, de_foliage_type *out, char *msg,
                                size_t cap);
    /** DE_FOLIAGE_NOT_FOUND when `e` is not alive; a non-terrain surface is returned with terrain
     * NULL (the scatter refuses it). */
    de_foliage_error (*surface_of)(void *user, de_entity e, de_foliage_surface *out, char *msg,
                                   size_t cap);
} de_foliage_env;

/** A paint or erase stroke (the protocol's FoliageStroke). */
typedef struct de_foliage_stroke {
    bool has_type;
    u32 type;
    const f32 *points; /**< [x, y, z, pressure] per dab */
    u32 point_count;
    f32 radius, strength;
    const f32 *falloff;
    u32 falloff_count;
    bool has_density;
    f32 density;
    u32 seed;
} de_foliage_stroke;

/** The instances of one cell, contiguous, in placement order. */
typedef struct de_foliage_cell {
    i32 x, z;
    u32 count, cap;
    u64 revision; /**< entity revision of the cell's last edit (DeFoliagePage.revision) */
    de_foliage_instance *inst;
} de_foliage_cell;

/** The instances of a layer: cells sorted by z, then x; none empty. Heap memory from `alloc`. */
typedef struct de_foliage_store {
    const de_allocator *alloc;
    de_foliage_cell *cells;
    u32 cell_count, cell_cap;
    u32 instance_count;
} de_foliage_store;

/** An empty store on `alloc` (NULL: de_heap_allocator()). */
void de__foliage_store_init(de_foliage_store *st, const de_allocator *alloc);
/** Frees the memory of `st`; it is empty after. */
void de__foliage_store_free(de_foliage_store *st);
/** Makes `dst` (initialised) a copy of `src`. False when out of memory (then `dst` is empty). */
bool de__foliage_store_copy(de_foliage_store *dst, const de_foliage_store *src);
/** Appends the `n` instances of a page to cell (x, z), after those it has. Pages of one cell are
 * added in page order. False (nothing changed) when the cell or the store is full or out of
 * memory. */
bool de__foliage_store_append(de_foliage_store *st, i32 x, i32 z, u64 revision,
                              const de_foliage_instance *inst, u32 n);
/** The cell (x, z), or NULL. */
de_foliage_cell *de__foliage_store_cell(const de_foliage_store *st, i32 x, i32 z);

/** Paints `s` on the highest of `surfaces`. */
de_foliage_error de__foliage_paint(de_foliage_store *st, const de_foliage_env *env,
                                   const de_foliage_stroke *s, const de_foliage_surface *surfaces,
                                   u32 surface_count, char *msg, size_t cap);
/** Erases with `s`. */
de_foliage_error de__foliage_erase(de_foliage_store *st, const de_foliage_stroke *s, char *msg,
                                   size_t cap);
/** Re-runs rules `ids` of `rules` (NULL: every enabled rule with an id), or only clears their
 * instances. */
de_foliage_error de__foliage_scatter(de_foliage_store *st,
                                     const de_foliage_rule rules[DE_FOLIAGE_RULES],
                                     const de_foliage_env *env, const u32 *ids, u32 id_count,
                                     bool clear, char *msg, size_t cap);

/** The instances of cell (x, z), `*n` of them (NULL and 0 when the cell has none). */
const de_foliage_instance *de__foliage_cell(const de_foliage_store *st, i32 x, i32 z, u32 *n);
/** True when cell (x, z) holds the same instances, in the same order, in `a` and `b`. */
bool de__foliage_cell_equal(const de_foliage_store *a, const de_foliage_store *b, i32 x, i32 z);
/** Replaces the instances of cell (x, z) with `n` of `inst`; an empty cell is dropped. False
 * (nothing changed) when there is no room. */
bool de__foliage_set_cell(de_foliage_store *st, i32 x, i32 z, const de_foliage_instance *inst,
                          u32 n);
/** True when the cell table and the counts are consistent. */
bool de__foliage_valid(const de_foliage_store *st);
/** Gives the upright instances of alignToNormal types the normal of the terrain under them: the
 * one of `surfaces` whose height there is within 5 cm of the instance's y. The undo data has no
 * normals (DE_FOLIAGE_STRIDE), so a restore calls this to get them back. */
void de__foliage_realign(de_foliage_store *st, const de_foliage_env *env,
                         const de_foliage_surface *surfaces, u32 surface_count);
/** Position, rotation quaternion (x, y, z, w: the yaw about +y, then for an aligned instance the
 * tilt of +y onto its normal) and scale (3): 10 floats. */
void de__foliage_transform(const de_foliage_instance *i, f32 out[10]);

DE_EXTERN_C_END

#endif /* DE_WORLD_FOLIAGE_H */
