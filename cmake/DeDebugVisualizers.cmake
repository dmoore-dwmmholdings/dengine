# de_add_debug_visualizers(NAME <identifier> [REGISTER <C-function> SOURCES <files>...]
#                         [LIBS <targets>...] [FOR_TARGET <target>])
# REGISTER has signature de_result(de_type_registry *). Share its registration-only source
# with the game; do not pass a game init callback or source that defines main(). No game,
# platform, GPU or asset system is started. Without REGISTER, only core types are generated.
# Outputs match DIDE's binaryDir/dide/visualizers discovery. Native builds generate by default;
# cross builds require CMAKE_CROSSCOMPILING_EMULATOR or execution on the target host.
include_guard(GLOBAL)
function(de_add_debug_visualizers)
    cmake_parse_arguments(PARSE_ARGV 0 V "" "NAME;REGISTER;FOR_TARGET" "SOURCES;LIBS")
    if(V_UNPARSED_ARGUMENTS OR NOT V_NAME MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "de_add_debug_visualizers: NAME must be an identifier")
    endif()
    if(V_REGISTER AND NOT V_REGISTER MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "de_add_debug_visualizers: REGISTER must name a C function")
    endif()
    if(V_FOR_TARGET AND NOT TARGET ${V_FOR_TARGET})
        message(FATAL_ERROR "de_add_debug_visualizers: unknown FOR_TARGET ${V_FOR_TARGET}")
    endif()
    if(DE_IS_SHIPPING)
        return() # No debugger executables or generated Python in a Shipping build.
    endif()
    set(DE_VISUALIZER_DECL "")
    set(DE_VISUALIZER_CALL "DE_OK")
    if(V_REGISTER)
        set(DE_VISUALIZER_DECL "de_result ${V_REGISTER}(de_type_registry *r);")
        set(DE_VISUALIZER_CALL "${V_REGISTER}(registry)")
    endif()
    set(driver "${CMAKE_CURRENT_BINARY_DIR}/${V_NAME}_visualizer_main.c")
    configure_file("${DE_ROOT_DIR}/tools/visualizers/main.c.in" "${driver}" @ONLY)
    add_executable(${V_NAME}_visualizer_gen EXCLUDE_FROM_ALL "${driver}" ${V_SOURCES}
        "${DE_ROOT_DIR}/tools/visualizers/core_types.c")
    target_include_directories(${V_NAME}_visualizer_gen PRIVATE "${DE_ROOT_DIR}/tools/visualizers"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${V_NAME}_visualizer_gen PRIVATE de_reflect de_core de_build_flags ${V_LIBS})
    if(V_FOR_TARGET)
        # Registration must see the game's conditional fields and packing options as well.
        target_compile_definitions(${V_NAME}_visualizer_gen PRIVATE
            "$<TARGET_PROPERTY:${V_FOR_TARGET},COMPILE_DEFINITIONS>")
        target_include_directories(${V_NAME}_visualizer_gen PRIVATE
            "$<TARGET_PROPERTY:${V_FOR_TARGET},INCLUDE_DIRECTORIES>")
        target_compile_options(${V_NAME}_visualizer_gen PRIVATE
            "$<TARGET_PROPERTY:${V_FOR_TARGET},COMPILE_OPTIONS>")
    endif()
    set_target_properties(${V_NAME}_visualizer_gen PROPERTIES FOLDER "dengine/tools")
    if(WIN32)
        target_compile_definitions(${V_NAME}_visualizer_gen PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    set(folder "${CMAKE_BINARY_DIR}/dide/visualizers")
    set(temp "${CMAKE_CURRENT_BINARY_DIR}/${V_NAME}-visualizer-temp")
    set(xml "${folder}/de_generated_${V_NAME}.natvis")
    set(py "${folder}/de_generated_${V_NAME}.py")
    set(can_run TRUE)
    if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
        set(can_run FALSE)
    endif()
    if(can_run)
        set(stamp "${temp}/complete.stamp")
        add_custom_command(OUTPUT "${stamp}" BYPRODUCTS "${xml}" "${py}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${folder}" "${temp}"
            COMMAND "${CMAKE_COMMAND}" -E rm -f "${temp}/types.natvis" "${temp}/types.py"
            COMMAND ${V_NAME}_visualizer_gen "${temp}/types.natvis" "${temp}/types.py"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${temp}/types.natvis" "${xml}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${temp}/types.py" "${py}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
            DEPENDS ${V_NAME}_visualizer_gen
            COMMENT "Generating ${V_NAME} debugger visualizers from reflection"
            VERBATIM)
        add_custom_target(${V_NAME}_visualizers ALL DEPENDS "${stamp}")
        if(V_FOR_TARGET)
            add_dependencies(${V_FOR_TARGET} ${V_NAME}_visualizers)
        endif()
    else()
        message(STATUS "${V_NAME}: visualizer generator is cross-built; run it on the target host")
    endif()
endfunction()
