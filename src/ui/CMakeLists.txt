# src/ui: module `ui` (target de_ui), layer gameplay.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME ui
    LAYER gameplay
    SOURCES ui_stub.c
    DEPS render text input
    OPTIONAL_OPTION DE_WITH_UI
)
