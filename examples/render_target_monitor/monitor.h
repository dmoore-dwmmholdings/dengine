#ifndef MONITOR_EXAMPLE_H
#define MONITOR_EXAMPLE_H
#include <dengine/de_render.h>
#include <dengine/de_ecs.h>

/* Public-API sample: card mesh and base material must already be loaded. */
typedef struct monitor_example {
    de_renderer *renderer;
    de_world *world;
    de_gpu_texture target;
    de_material material;
    de_view producer;
    de_entity camera, screen;
} monitor_example;
de_result monitor_example_open(monitor_example *m, de_renderer *renderer, de_world *world,
                               de_mesh card_mesh, de_material base, u32 width, u32 height);
de_result monitor_example_resize(monitor_example *m, u32 width, u32 height);
void monitor_example_close(monitor_example *m);
#endif
