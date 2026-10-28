/* editor_gizmo.h: the transform gizmo and the scene overlays editor views draw (DENG-650). */
#ifndef DE_HOST_EDITOR_GIZMO_H
#define DE_HOST_EDITOR_GIZMO_H

#include "host/editor/editor_internal.h"

DE_EXTERN_C_BEGIN

/** An editor view's camera, as the IDE sets it: it looks along its local +Y with +Z up. */
typedef struct de__ed_cam {
    f64 pos[3];
    f32 rot[4];   /* quaternion x, y, z, w */
    f64 fov_y;    /* radians; 0 for orthographic */
    f64 ortho_h;  /* orthographic: the view's height in metres */
    f64 near;
    u32 w, h;     /* the source (render) size in pixels */
} de__ed_cam;

/** An sRGB RGBA8 image the overlays draw into. */
typedef struct de__ed_canvas {
    u8 *map;
    u32 pitch, w, h;
} de__ed_canvas;

/* What a view shows (view.setShowFlags). */
#define DE__ED_SHOW_GRID    (1u << 0)
#define DE__ED_SHOW_GIZMOS  (1u << 1)
#define DE__ED_SHOW_OUTLINE (1u << 2)
#define DE__ED_SHOW_CAMERAS (1u << 3)
#define DE__ED_SHOW_LIGHTS  (1u << 4)
#define DE__ED_SHOW_COLLIDERS (1u << 5) /* every 2D collider (the selection's always show) */
#define DE__ED_SHOW_DEFAULT                                                                         \
    (DE__ED_SHOW_GRID | DE__ED_SHOW_GIZMOS | DE__ED_SHOW_OUTLINE | DE__ED_SHOW_CAMERAS |           \
     DE__ED_SHOW_LIGHTS)

/** The show flags named in `names` (view.setShowFlags). */
u32 de__ed_show_flags(const de_ep_list_str *names);

void de__ed_gizmo_set_tool(const de_ep_gizmo_tool_params *q);
void de__ed_gizmo_set_selection(const u64 *entities, u32 count);
/** A pointer or key event of editor view `view`; `x`, `y` in source pixels. */
void de__ed_gizmo_input(de__editor *ed, u32 view, const de__ed_cam *c, const de_ep_input_event *e,
                        f32 x, f32 y);
/** The grid, selection outlines, camera frames and the gizmo over a view's frame. Entities in
 * `skip` (the editor's own cameras) get no camera frame. */
void de__ed_gizmo_draw(de__editor *ed, u32 view, const de__ed_cam *c, const de__ed_canvas *cv,
                       u32 show, const u64 *skip, u32 skip_count);
/** The world point under (x, y) (source pixels). A 2D view: its plane through the origin. 3D: the
 * plane of `entity` (a sprite's, or the one facing the camera through its position), or with no
 * entity the ground. `normal` faces the camera. False when the ray misses the plane. */
bool de__ed_view_point(de__editor *ed, const de__ed_cam *c, f32 x, f32 y, u64 entity, f64 point[3],
                       f32 normal[3]);
/** The camera or light whose icon is under (x, y) (source pixels), or 0. Cameras and lights
 * draw nothing a GPU pick can hit, so their icons are what a click selects (as in Unity and
 * Godot). `show` says which icons the view draws; `skip` as for de__ed_gizmo_draw. */
u64 de__ed_icon_pick(de__editor *ed, const de__ed_cam *c, f32 x, f32 y, u32 show, const u64 *skip,
                     u32 skip_count);
/** The sprites under (x, y) (source pixels), nearest first, by their rectangles: what a click
 * means in 2D, translucent sprites included (the GPU pick sees only opaque ones). At most `cap`;
 * returns how many. */
u32 de__ed_sprite_pick(de__editor *ed, const de__ed_cam *c, f32 x, f32 y, u64 *out, u32 cap);
/** The entities whose origin is inside the box (x0, y0)-(x1, y1) (source pixels), as Godot's box
 * selection takes them: empty entities, cameras, lights and translucent sprites too. Entities in
 * `skip` are left out. At most `cap`; returns how many. */
u32 de__ed_origin_pick(de__editor *ed, const de__ed_cam *c, f32 x0, f32 y0, f32 x1, f32 y1,
                       const u64 *skip, u32 skip_count, u64 *out, u32 cap);
/** A mesh instance's LOD 0 triangles, as the renderer keeps them for the editor. */
typedef struct de__ed_mesh_geo {
    const f32 *positions; /**< object space, x y z per vertex */
    const u32 *triangles; /**< 3 vertex indices each */
    u32 vertex_count, triangle_count;
    f32 model[12];   /**< de_affine: object to world, less `origin` */
    de_dvec3 origin; /**< the render origin */
} de__ed_mesh_geo;
/** Calls `fn` with each mesh instance's world bounds, as the renderer has them (editor_views.c),
 * and its triangles (NULL for a mesh loaded before the service started): what surface placement
 * lands on and vertex snapping reaches. */
void de__ed_each_bounds(de__editor *ed,
                        void (*fn)(u64 e, de_dvec3 lo, de_dvec3 hi, const de__ed_mesh_geo *geo, void *user),
                        void *user);
/** The world bounds of entity `e`'s mesh, as the renderer has them (editor_views.c); false
 * without a mesh, or before it has loaded. */
bool de__ed_entity_bounds(de__editor *ed, u64 e, de_dvec3 *min, de_dvec3 *max);
/** True for a 2D view: orthographic, looking along a world axis. */
bool de__ed_cam_flat(const de__ed_cam *c);
/** A connection ended: no drag, no hover. */
void de__ed_gizmo_reset(de__editor *ed);

DE_EXTERN_C_END

#endif /* DE_HOST_EDITOR_GIZMO_H */
