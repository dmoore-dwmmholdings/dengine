# tests/fuzz/input: fuzz targets of module `input` (DENG-89). The .deir recording loader is a
# parser (CONVENTIONS §5). The seed corpus holds valid recordings of the target's three-action
# schema (header identity and checksum zeroed: the target's fixed-up pass repairs them) and
# hand-made edge cases. See cmake/DeTesting.cmake for real fuzzing with libFuzzer.
if(NOT TARGET de_input)
    return()
endif()
de_add_fuzz_target(NAME replay_load SOURCES fuzz_replay_load.c LIBS de_input
                   CORPUS corpus/replay_load)
if(TARGET fuzz_replay_load)
    # src/ is an include root for the internal input/record_format.h.
    target_include_directories(fuzz_replay_load PRIVATE "${DE_ROOT_DIR}/src")
endif()
