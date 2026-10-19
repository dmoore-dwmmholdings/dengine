# Unicode 15.1 implementation; upstream unity build keeps the C interface intact.
add_library(de_tp_sheenbidi STATIC Source/SheenBidi.c)
target_include_directories(de_tp_sheenbidi SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Headers")
target_compile_definitions(de_tp_sheenbidi PRIVATE SB_CONFIG_UNITY)
set_target_properties(de_tp_sheenbidi PROPERTIES POSITION_INDEPENDENT_CODE ON)
