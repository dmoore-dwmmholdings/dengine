#include "debugger_fixture.h"
#include <dengine/de_ecs.h>
#include "core/de_array.h"
#include "core/de_hashmap.h"
de_vec3 debug_position = {1, 2, 3};
de_quat debug_rotation = {0, 0, 0, 1};
de_mat4 debug_matrix = {.m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 4, 5, 6, 1}};
DebugHealth debug_health = {.current = 75,
                            .position = {4, 5, 6},
                            .inventory = {7, 8, 9}
#if DEBUG_HEALTH_EXTRA
                            ,
                            .shield = 25
#endif
};
de_str debug_text = {"café\nworld", 11};
de_entity debug_entity = {.bits = ((u64)2 << 32) | 7};
de_array debug_array;
de_hashmap debug_map;
de_str debug_invalid = {NULL, 5};
static char debug_bytes[5000] = "long string";
de_str debug_long = {debug_bytes, sizeof debug_bytes};
volatile u32 debug_sink;
void debug_stop(void);
void debug_stop(void) {
    debug_sink++;
}
int main(void) {
    de_type_registry *r = NULL;
    if (de_type_registry_create(NULL, &r) != DE_OK || debug_register_types(r) != DE_OK)
        return 1;
    de_array_init(&debug_array, NULL, sizeof(u32), DE_ALIGNOF(u32), DE_MEM_GENERAL);
    de_hashmap_desc map = {.key_size = sizeof(u32), .value_size = sizeof(u32)};
    if (de_hashmap_init(&debug_map, &map) != DE_OK)
        return 1;
    for (u32 i = 0; i < 3; ++i) {
        if (de_array_push(&debug_array, &i) != DE_OK ||
            de_hashmap_put(&debug_map, i, &i, &i) != DE_OK)
            return 1;
    }
    debug_stop();
    de_array_free(&debug_array);
    de_hashmap_free(&debug_map);
    de_type_registry_destroy(r);
    return debug_health.current == 75 ? 0 : 1;
}
