#include "monitor.h"
#include <string.h>

de_result monitor_example_resize(monitor_example *m, u32 width, u32 height) {
    if (!m || !m->renderer || !width || !height)
        return DE_ERR_INVALID_ARGUMENT;
    de_component camera_type = de_component_find(m->world, "DeCamera");
    DeCamera *camera = de_entity_get(m->world, m->camera, camera_type);
    if (!camera)
        return DE_ERR_INVALID_ARGUMENT;
    de_gpu_device *device = de_renderer_device(m->renderer);
    de_gpu_texture target = de_gpu_texture_create(
        device, &(de_gpu_texture_desc){.width = width,
                                       .height = height,
                                       .format = DE_FMT_RGBA8_UNORM,
                                       .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                DE_USAGE_COPY_SRC,
                                       .name = "monitor.feed"});
    if (!target.id)
        return DE_ERR_OUT_OF_MEMORY;
    de_view producer = de_view_create(
        m->renderer, &(de_view_desc){.target = target, .order = -10, .name = "monitor.camera"});
    if (!producer.id) {
        de_gpu_texture_destroy(device, target);
        return DE_ERR_INVALID_ARGUMENT;
    }
    de_view_set_layer_mask(m->renderer, producer, 1u); /* The screen is layer 2. */
    de_result result = de_material_set_gpu_texture(m->renderer, m->material,
                                                   DE_MATERIAL_TEXTURE_BASE_COLOR, target);
    if (result != DE_OK) {
        de_view_destroy(m->renderer, producer);
        de_gpu_texture_destroy(device, target);
        return result;
    }
    camera->view = producer;
    if (m->producer.id)
        de_view_destroy(m->renderer, m->producer);
    if (m->target.id)
        de_gpu_texture_destroy(device, m->target);
    m->producer = producer;
    m->target = target;
    return DE_OK;
}

void monitor_example_close(monitor_example *m) {
    if (!m || !m->renderer)
        return;
    if (m->camera.bits)
        de_entity_despawn(m->world, m->camera);
    if (m->screen.bits)
        de_entity_despawn(m->world, m->screen);
    if (m->producer.id)
        de_view_destroy(m->renderer, m->producer);
    if (m->material.id)
        de_material_destroy_instance(m->renderer, m->material);
    if (m->target.id)
        de_gpu_texture_destroy(de_renderer_device(m->renderer), m->target);
    memset(m, 0, sizeof *m);
}

de_result monitor_example_open(monitor_example *m, de_renderer *renderer, de_world *world,
                               de_mesh card_mesh, de_material base, u32 width, u32 height) {
    if (!m || !renderer || !world || !card_mesh.id)
        return DE_ERR_INVALID_ARGUMENT;
    *m = (monitor_example){.renderer = renderer, .world = world};
    de_component camera_type = de_component_find(world, "DeCamera");
    de_component screen_type = de_component_find(world, "DeMeshRenderer");
    if (!camera_type.id || !screen_type.id)
        goto invalid;
    m->material = de_material_create_instance(renderer, base);
    if (!m->material.id)
        goto invalid;
    de_standard_material params;
    if (de_material_get_params(renderer, m->material, &params) != DE_OK)
        goto invalid;
    params.base_color = (de_vec4){1, 1, 1, 1};
    params.unlit = true;
    params.double_sided = true;
    if (de_material_set_params(renderer, m->material, &params) != DE_OK)
        goto invalid;
    de_builtin_component_ids bi = de_builtin_components(world);
    m->camera = de_entity_spawn(world);
    DeTransform *pose = de_entity_add(world, m->camera, bi.transform);
    DeCamera *camera = de_entity_add(world, m->camera, camera_type);
    /* Adding a component can move the entity to another archetype. */
    pose = de_entity_get(world, m->camera, bi.transform);
    if (!pose || !camera)
        goto invalid;
    *pose = de_transform_identity();
    pose->position.z = 2;
    *camera = (DeCamera){.projection = DE_PROJ_ORTHOGRAPHIC, .ortho_height = 2, .near_z = .1f};
    m->screen = de_entity_spawn(world);
    pose = de_entity_add(world, m->screen, bi.transform);
    DeMeshRenderer *screen = de_entity_add(world, m->screen, screen_type);
    pose = de_entity_get(world, m->screen, bi.transform);
    if (!pose || !screen)
        goto invalid;
    *pose = de_transform_identity();
    /* The sample card.gltf imports with its normal along -Y; face the +Z viewer. */
    pose->rotation = (de_quat){-.70710678f, 0, 0, .70710678f};
    screen->mesh = card_mesh;
    screen->materials[0] = m->material;
    screen->layer_mask = 2u;
    de_result result = monitor_example_resize(m, width, height);
    if (result == DE_OK)
        return result;
    monitor_example_close(m);
    return result;
invalid:
    monitor_example_close(m);
    return DE_ERR_INVALID_ARGUMENT;
}
