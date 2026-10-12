# src/input: module `input` (target de_input), layer gameplay. SPEC §15.
# Shared file (append-only within S21): later S21 tasks add their sources to the list.
de_add_module(
    NAME input
    LAYER gameplay
    SOURCES
        # DENG-83: input state, key names, and hook stubs (actions.c -> DENG-84,
        # gyro.c / haptics.c -> DENG-87).
        input.c
        input_state.c
        keys.c
        actions.c
        gyro.c
        haptics.c
    DEPS platform ecs
    DETERMINISTIC
)

# DENG-84: action maps (actions.c replaces the DENG-83 stub). action_toml.c reads action-map files
# with the vendored tomlc17, through core's entry point (core/de_toml_internal.h; never
# toml_set_option). Recorded in DE_MODULE_LIBS too, so the aggregate `dengine` links it.
target_sources(de_input PRIVATE
    action_internal.h
    action_map.c
    action_eval.c
    action_modifiers.c
    action_toml.c
    context_stack.c)
target_link_libraries(de_input PRIVATE de_tp_tomlc17)
set_property(TARGET de_input APPEND PROPERTY DE_MODULE_LIBS de_tp_tomlc17)
