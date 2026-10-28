#ifndef DE_TEST_PUBLIC_LIGHT_ISOLATION_FIXTURE_H
#define DE_TEST_PUBLIC_LIGHT_ISOLATION_FIXTURE_H
/* A game-side scene using only the public SDK. The driver supplies ordinary loaded assets
 * and reads pixels/shadow maps; it does not mutate renderer state between view builds. */
#include <dengine/de_ecs.h>
#include <dengine/de_render.h>

typedef struct de_test_light_scene {
    de_view views[2];
    de_entity cameras[2], receivers[2], lights[2];
} de_test_light_scene;

static inline de_result de_test_light_scene_create(de_renderer *renderer, de_world *world,
                                                   de_mesh mesh, de_material material,
                                                   const de_gpu_texture targets[2],
                                                   de_light_kind kind, de_test_light_scene *out) {
    const de_builtin_component_ids bi = de_builtin_components(world);
    const de_component camera = de_component_find(world, "DeCamera");
    const de_component geometry = de_component_find(world, "DeMeshRenderer");
    const de_component light = de_component_find(world, "DeLight");
    for (u32 i = 0; i < 2; ++i) {
        out->views[i] = de_view_create(renderer, &(de_view_desc){.target = targets[i],
                                                                 .name = i ? "feed" : "office",
                                                                 .order = i ? 0 : 1});
        if (!out->views[i].id)
            return DE_ERR_INTERNAL;
        de_view_set_layer_mask(renderer, out->views[i], 1u << i);
        DE_TRY(de_view_set_light_mask(renderer, out->views[i], 1u << i));
        const de_component camera_fields[] = {bi.transform, camera};
        DE_TRY(de_entity_spawn_with(world, camera_fields, 2, &out->cameras[i]));
        DeTransform *t = de_entity_get(world, out->cameras[i], bi.transform);
        *t = de_transform_identity();
        t->position.z = 5;
        DeCamera *c = de_entity_get(world, out->cameras[i], camera);
        c->view = out->views[i];
        c->projection = DE_PROJ_ORTHOGRAPHIC;
        c->ortho_height = 4;
        c->exposure_ev100 = 3;
        const de_component mesh_fields[] = {bi.transform, geometry};
        DE_TRY(de_entity_spawn_with(world, mesh_fields, 2, &out->receivers[i]));
        *(DeTransform *)de_entity_get(world, out->receivers[i], bi.transform) =
            de_transform_identity();
        DeMeshRenderer *m = de_entity_get(world, out->receivers[i], geometry);
        m->mesh = mesh;
        m->materials[0] = material;
        m->layer_mask = 1u << i;
        const de_component light_fields[] = {bi.transform, light};
        DE_TRY(de_entity_spawn_with(world, light_fields, 2, &out->lights[i]));
        t = de_entity_get(world, out->lights[i], bi.transform);
        *t = de_transform_identity();
        t->position.z = 4;
        DeLight *l = de_entity_get(world, out->lights[i], light);
        l->kind = kind;
        l->color = i ? (de_vec3){0, 1, 0} : (de_vec3){1, 0, 0};
        l->intensity = kind == DE_LIGHT_DIRECTIONAL ? 10.f : 2000.f;
        l->range = kind == DE_LIGHT_DIRECTIONAL ? 0.f : 20.f;
        l->view_mask = 1u << i;
        l->cast_shadows = true;
    }
    return DE_OK;
}
static inline de_result de_test_light_scene_monitor(de_renderer *renderer, de_world *world,
                                                    de_mesh mesh, de_material base,
                                                    de_gpu_texture feed, de_entity *entity,
                                                    de_material *material) {
    *material = de_material_create_instance(renderer, base);
    if (!material->id)
        return DE_ERR_INTERNAL;
    de_standard_material params;
    DE_TRY(de_material_get_params(renderer, *material, &params));
    params.unlit = true;
    // Unlit values are exposed too. Compensate the fixture's EV100=3 on the display surface.
    params.base_color = (de_vec4){9.6f, 9.6f, 9.6f, 1};
    DE_TRY(de_material_set_params(renderer, *material, &params));
    DE_TRY(de_material_set_gpu_texture(renderer, *material, DE_MATERIAL_TEXTURE_BASE_COLOR, feed));
    const de_builtin_component_ids bi = de_builtin_components(world);
    const de_component geometry = de_component_find(world, "DeMeshRenderer");
    const de_component fields[] = {bi.transform, geometry};
    DE_TRY(de_entity_spawn_with(world, fields, 2, entity));
    DeTransform *transform = de_entity_get(world, *entity, bi.transform);
    *transform = de_transform_identity();
    transform->position = (de_dvec3){-1.4, 1.4, 1};
    transform->scale = (de_vec3){.4f, .4f, .4f};
    DeMeshRenderer *m = de_entity_get(world, *entity, geometry);
    m->mesh = mesh;
    m->materials[0] = *material;
    m->layer_mask = 1;
    m->cast_shadows = m->visible_in_rt = false;
    return DE_OK;
}
#endif
