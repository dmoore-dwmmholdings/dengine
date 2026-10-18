if(NOT TARGET de_gpu_shader OR NOT TARGET de_input)
    return()
endif()
add_executable(sky_hop main.c)
target_link_libraries(sky_hop PRIVATE de_gpu de_gpu_shader de_asset de_task de_input de_platform de_core de_tp_stb de_build_flags)
target_compile_definitions(sky_hop PRIVATE "HELLO_ASSET_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
set_target_properties(sky_hop PROPERTIES FOLDER "dengine/examples")
if(DE_BUILD_TESTS)
    add_test(NAME example_sky_hop_logic COMMAND sky_hop --self-check)
    set_tests_properties(example_sky_hop_logic PROPERTIES LABELS "example" TIMEOUT 10)
endif()

if(APPLE)
    set_target_properties(sky_hop PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.dengine.skyhop"
        MACOSX_BUNDLE_BUNDLE_NAME "Sky Hop"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
endif()
