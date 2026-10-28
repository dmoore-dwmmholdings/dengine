#ifndef DE_DEBUGGER_FIXTURE_H
#define DE_DEBUGGER_FIXTURE_H
#include <dengine/de_reflect.h>
#include <dengine/de_math.h>
typedef struct DebugHealth {
    f32 current;
#if DEBUG_HEALTH_EXTRA
    f32 shield;
#endif
    de_vec3 position;
    u32 inventory[3];
} DebugHealth;
de_result debug_register_types(de_type_registry *r);
#endif
