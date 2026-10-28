/* This same registration-only source is linked by the game fixture and its build generator. */
#include "debugger_fixture.h"
de_result debug_register_types(de_type_registry *r) {
    const de_field_info fields[] = {
        DE_FIELD(DebugHealth, current, DE_FK_F32, DE_FIELD_DEFAULT),
#if DEBUG_HEALTH_EXTRA
        DE_FIELD(DebugHealth, shield, DE_FK_F32, DE_FIELD_DEFAULT),
#endif
        DE_FIELD(DebugHealth, position, DE_FK_VEC3, DE_FIELD_DEFAULT),
        DE_FIELD_EX(DebugHealth, inventory, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT,
                    {0, 0, NULL, NULL, "u32", 3}),
    };
    const de_type_desc d = DE_TYPE_DESC(DebugHealth, 1, fields);
    return de_type_register_ex(r, &d, NULL, NULL);
}
