# src/world/foliage: module `foliage` (target de_foliage), layer gameplay.
# The CPU foliage model (palette types, paint and erase strokes, seeded scatter rules, instances per
# 16 m cell) behind the DIDE editor IPC world.foliage, protocol 1.22 (DENG-581). DeFoliage is a
# render component. The module is DETERMINISTIC: all math is de_dmath.
de_add_module(
    NAME foliage
    LAYER gameplay
    SOURCES foliage.c foliage.h
    DEPS core ecs asset terrain
    OPTIONAL_DEPS render
    DETERMINISTIC
)

# foliageType assets are TOML, read with the vendored tomlc17 through core's entry point
# (core/de_toml_internal.h). Recorded in DE_MODULE_LIBS too, so the aggregate `dengine` links it.
target_link_libraries(de_foliage PRIVATE de_tp_tomlc17)
set_property(TARGET de_foliage APPEND PROPERTY DE_MODULE_LIBS de_tp_tomlc17)
