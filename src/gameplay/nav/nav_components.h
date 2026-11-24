/*
 * nav_components.h: the authoring components of DIDE's world.nav (DENG-582, protocol 1.25):
 * DeNavLink (an off-mesh link from the entity's position), DeNavModifier (a box volume that
 * marks an area or carves) and DeNavGrid2D (a 2D walkability grid). The editor service
 * (src/host/editor/editor_nav.c) bakes links and modifiers into the editor navmeshes. The game
 * runtime does not read links and modifiers yet. DeNavGrid2D is public (de_gameplay.h): the
 * system `nav2d.grid` (nav2d_grid_system.c, DENG-832) builds a de_nav2d_grid from each one.
 * Internal, not installed.
 */
#ifndef DE_GAMEPLAY_NAV_COMPONENTS_H
#define DE_GAMEPLAY_NAV_COMPONENTS_H

#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gameplay.h> /* DeNavGrid2D, DE_NAV_GRID2D_CELLS */
#include <dengine/de_math.h>

DE_EXTERN_C_BEGIN

typedef enum de_nav_modifier_mode {
    DE_NAV_MODIFIER_AREA = 0,  /* the walkable cells inside get `area` */
    DE_NAV_MODIFIER_CARVE = 1, /* the cells inside are not walkable */
} de_nav_modifier_mode;

/** An off-mesh link (a jump, a ladder) from the entity's world position to position + end. */
typedef struct DeNavLink {
    de_vec3 end;        /* m, world axes, from the entity's position */
    f32 radius;         /* m, 0.05 to 10 */
    bool bidirectional;
    u32 area;           /* 0 to 15 */
} DeNavLink;

/** A box volume of `size` (full extents) at the entity's world pose. */
typedef struct DeNavModifier {
    de_vec3 size;
    de_nav_modifier_mode mode;
    u32 area; /* 1 to 15 in area mode */
} DeNavModifier;

typedef struct de__nav_component_ids {
    de_component link, modifier, grid;
} de__nav_component_ids;

/** Registers the three components with `w` (once; later calls find them). */
de_result de__nav_components_register(de_world *w, de__nav_component_ids *out);

DE_EXTERN_C_END

#endif
