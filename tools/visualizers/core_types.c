/* Core math descriptions are shared by both generated debugger formats. Offsets/sizes come
 * from the real headers, not a platform-specific layout table. No runtime registration. */
#include "core_types.h"
#include <dengine/de_math.h>

de_result de__debugger_core_types(de_type_registry *r) {
#define F(T, n, k) DE_FIELD(T, n, k, 0)
#define REG(T, ...)                                                                                \
    do {                                                                                           \
        const de_field_info fields[] = {__VA_ARGS__};                                              \
        const de_type_desc desc = DE_TYPE_DESC(T, 1, fields);                                      \
        DE_TRY(de_type_register_ex(r, &desc, NULL, NULL));                                         \
    } while (0)
    REG(de_vec2, F(de_vec2, x, DE_FK_F32), F(de_vec2, y, DE_FK_F32));
    REG(de_ivec2, F(de_ivec2, x, DE_FK_I32), F(de_ivec2, y, DE_FK_I32));
    REG(de_vec3, F(de_vec3, x, DE_FK_F32), F(de_vec3, y, DE_FK_F32), F(de_vec3, z, DE_FK_F32));
    REG(de_dvec3, F(de_dvec3, x, DE_FK_F64), F(de_dvec3, y, DE_FK_F64), F(de_dvec3, z, DE_FK_F64));
    REG(de_vec4, F(de_vec4, x, DE_FK_F32), F(de_vec4, y, DE_FK_F32), F(de_vec4, z, DE_FK_F32),
        F(de_vec4, w, DE_FK_F32));
    REG(de_quat, F(de_quat, x, DE_FK_F32), F(de_quat, y, DE_FK_F32), F(de_quat, z, DE_FK_F32),
        F(de_quat, w, DE_FK_F32));
    REG(de_name, F(de_name, hash, DE_FK_U64));
    REG(de_mat3, DE_FIELD_EX(de_mat3, m, DE_FK_FIXED_ARRAY, 0, {0, 0, NULL, NULL, "f32", 9}));
    REG(de_mat4, DE_FIELD_EX(de_mat4, m, DE_FK_FIXED_ARRAY, 0, {0, 0, NULL, NULL, "f32", 16}));
    REG(de_affine, DE_FIELD_EX(de_affine, m, DE_FK_FIXED_ARRAY, 0, {0, 0, NULL, NULL, "f32", 12}));
#undef REG
#undef F
    return DE_OK;
}
