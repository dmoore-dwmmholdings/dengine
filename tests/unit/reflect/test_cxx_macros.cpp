// test_cxx_macros.cpp: the de_reflect.h registration macros compiled as C++17 (DENG-294).
// DE_TYPE_DESC is a compound literal in C; in C++ it is a de_type_desc prvalue.
#include <dengine/de_reflect.h>

namespace {
struct TCxxHealth {
    f32 current;
    f32 max;
};

const de_field_info k_fields[] = {
    DE_FIELD(TCxxHealth, current, DE_FK_F32, DE_FIELD_DEFAULT | DE_FIELD_REPLICATE),
    DE_FIELD_EX(TCxxHealth, max, DE_FK_F32, DE_FIELD_DEFAULT, {0, 1000, "hp", "Maximum", 0, 0}),
};
} // namespace

extern "C" de_type_id de__test_reflect_cxx_register(de_type_registry *r);

extern "C" de_type_id de__test_reflect_cxx_register(de_type_registry *r) {
    const de_type_desc d = DE_TYPE_DESC(TCxxHealth, 1, k_fields);
    return de_type_register(r, &d);
}
