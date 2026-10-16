/*
 * components_internal.h: the render components' registration state (DENG-178; SPEC §9.1, §12.4).
 * Internal to de_render; include as "render/components_internal.h".
 *
 * de_render_register_components (de_render.h) stores the component ids of one world in the world
 * resource DeRenderComponents. The resource is the per-world "registered" flag (a second call
 * returns at once) and the place Extract (DENG-175) reads the ids from, without a lookup by name.
 *
 * Later sprints append components (S15: DeReflectionProbe, DeIrradianceVolume): add a field at
 * the end of DeRenderComponents, a register function and one row of the table in components.c.
 */
#ifndef DE_RENDER_COMPONENTS_INTERNAL_H
#define DE_RENDER_COMPONENTS_INTERNAL_H

#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_render.h>

DE_EXTERN_C_BEGIN

/** Name of the resource type in the world's type registry. */
#define DE__RENDER_COMPONENTS_TYPE_NAME "DeRenderComponents"

/** The render components of one world (a world resource; never serialized). */
typedef struct DeRenderComponents {
    de_component camera;
    de_component mesh_renderer;
    de_component light;
    de_component environment;
    de_component sprite;
    de_component sky;
    de_component fog_volume;
    de_component decal;
    de_component particle_system;
    de_component terrain;
    de_component water;
    de_component reflection_probe;
    de_component irradiance_volume;
} DeRenderComponents;

/** The ids of `w`, or NULL when de_render_register_components has not run on it. Main thread
 * or a system that declares no write of the resource. */
const DeRenderComponents *de__render_components(de_world *w);

/** de_renderer_attach's call: registers the components of `w`; a failure is logged to the
 * renderer's `render` category (attach returns nothing). */
void de__render_components_attach(de_renderer *r, de_world *w);

DE_EXTERN_C_END
#endif
