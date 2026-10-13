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

# DENG-85: context stacks (context_stack.c replaces the DENG-84 minimal version; the state is in
# context_internal.h, embedded in struct de__actions).
target_sources(de_input PRIVATE context_internal.h)

# DENG-86: runtime rebinding (listen mode, conflicts, policies, defaults) and the user bindings
# file (user:/bindings.toml, read through core's tomlc17 entry point like action_toml.c).
target_sources(de_input PRIVATE
    rebind.c
    bindings_io.c)

# DENG-89: recording / replay per fixed tick (.deir files) and netcode action packing. The module
# is DETERMINISTIC as a whole (de_add_module above), so these sources already get
# DE_DETERMINISTIC_FLAGS and tools/determinism_modules.toml covers them through its [[module]]
# input entry; de_mark_deterministic_sources() is only for files of non-deterministic modules.
target_sources(de_input PRIVATE
    record_format.h
    record.c
    replay.c
    pack.c)
