# tests/unit/audio: unit tests of module `audio` (de_audio, SPEC §14). Shared file (append-only
# within S25): later S25 tasks add their own de_add_test() blocks below, one per task.
#
# One executable per suite, so the CTest names are audio_mixer.<test>, audio_sched.<test>,
# audio_cmdq.<test> and audio_device.<test>. The tests include the internal header
# "audio/audio_internal.h" (src/ is an include root) and the shared audio_test_util.h. Skipped
# with the module (DE_WITH_AUDIO=OFF or DE_HEADLESS).

# DENG-308: mixer core (mixer, scheduling, command queue, null device, real-time guard).
if(TARGET de_audio)
    de_add_test(NAME audio_mixer SOURCES test_mixer.c LIBS de_audio de_test_support TIMEOUT 120)
    de_add_test(NAME audio_sched SOURCES test_scheduling.c LIBS de_audio)
    de_add_test(NAME audio_cmdq SOURCES test_command_queue.c LIBS de_audio LABELS unit tsan
                TIMEOUT 120)
    de_add_test(NAME audio_device SOURCES test_device_null.c LIBS de_audio TIMEOUT 120)
    foreach(_de_audio_test test_audio_mixer test_audio_sched test_audio_cmdq test_audio_device)
        if(TARGET ${_de_audio_test})
            target_include_directories(${_de_audio_test} PRIVATE "${DE_ROOT_DIR}/src")
        endif()
    endforeach()
endif()
