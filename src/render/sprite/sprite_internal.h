#ifndef DE_RENDER_SPRITE_INTERNAL_H
#define DE_RENDER_SPRITE_INTERNAL_H
#include "render/world/render_world.h"
#include "render/scene/gpu_scene.h"
#include "render/loaders/render_assets.h"
/* Synthetic records never escape the GPU scene's private tables. No asset handle is created. */
#define DE__SPRITE_MESH_ID       0xffffffffu
void de__sprite_extract(de__render_world *rw, de_world *w, de_query query);
de_affine de__sprite_local_transform(const DeSprite *s, de_plane_2d plane);
de_result de__sprite_quad_create(de_gpu_device *dev, de_render_mesh *mesh);
void de__sprite_pack_material(const DeSprite *s, de__gs_material *out,
                              u32 (*texture)(void *, de_texture), void *user);
#endif
