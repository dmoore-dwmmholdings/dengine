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
