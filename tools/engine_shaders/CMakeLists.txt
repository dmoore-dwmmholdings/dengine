# tools/engine_shaders: `de-engine-shaders`, the build-time cook of the renderer's engine shaders
# (shaders/engine_shaders.cmake; DENG-421). src/render/CMakeLists.txt runs it once per listed
# shader and embeds the cooked blobs in de_render. It links the cooker library only: de-cook links
# the whole engine (de_render included), which would make a dependency cycle.
#
# Needs the shader importer (Slang) and must run on the build host, so src/render uses it only when
# DE_ENGINE_SHADERS_BUILD_COOK is on (tools built, Slang present, not cross-compiling).
if(NOT TARGET de_cooker OR NOT TARGET de_tp_slang)
    return()
endif()

add_executable(de_engine_shaders_tool main.c)
set_target_properties(de_engine_shaders_tool PROPERTIES
    OUTPUT_NAME de-engine-shaders
    FOLDER "dengine/tools")
target_include_directories(de_engine_shaders_tool PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
target_link_libraries(de_engine_shaders_tool PRIVATE de_cooker de_asset de_platform de_core
                                                     de_build_flags)
