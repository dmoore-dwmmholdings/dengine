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

# DENG-314: resampler works independently of the runtime audio switch.
if(TARGET de_audio_dsp)
    de_add_test(NAME audio_resample SOURCES test_resample.c LIBS de_audio_dsp)
endif()
if(DE_BUILD_TOOLS)
    de_add_test(NAME sound_import SOURCES test_sound_import.c LIBS de_cooker de_audio_dsp)
    target_include_directories(test_sound_import PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
    target_compile_definitions(test_sound_import PRIVATE "DE_SOUND_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\""
        "DE_SOUND_REGRESSION_DIR=\"${DE_ROOT_DIR}/tests/fuzz/audio/regressions\"")
endif()
if(TARGET test_sound_import)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sound-import-tmp")
    target_compile_definitions(test_sound_import PRIVATE
        "DE_SOUND_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/sound-import-tmp\"")
endif()

# DENG-312: null-capture voice graph and bus/channel behavior.
if(TARGET de_audio)
    de_add_test(NAME audio_voices SOURCES test_voices.c LIBS de_audio TIMEOUT 120)
    de_add_test(NAME audio_buses SOURCES test_buses.c LIBS de_audio)
    target_include_directories(test_audio_voices PRIVATE "${DE_ROOT_DIR}/src")
    target_include_directories(test_audio_buses PRIVATE "${DE_ROOT_DIR}/src")
endif()

# DENG-313: actual cooked asset/source and asynchronous compressed playback checks.
if(TARGET de_audio AND DE_BUILD_TOOLS)
    de_add_test(NAME audio_sources SOURCES test_sources.c LIBS de_audio de_cooker)
    de_add_test(NAME audio_streaming SOURCES test_streaming.c LIBS de_audio de_cooker TIMEOUT 120)
    de_add_test(NAME audio_procedural SOURCES test_procedural.c LIBS de_audio)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/source-tmp")
    foreach(_source_test test_audio_sources test_audio_streaming test_audio_procedural)
        target_include_directories(${_source_test} PRIVATE "${DE_ROOT_DIR}/src" "${DE_ROOT_DIR}/tools")
        target_compile_definitions(${_source_test} PRIVATE
            "DE_AUDIO_SOURCE_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\""
            "DE_AUDIO_SOURCE_TMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}/source-tmp\"")
    endforeach()
endif()

# DENG-311: native HAL callbacks, output UID enumeration and control-thread recovery.
if(TARGET de_audio AND APPLE)
    de_add_test(NAME audio_coreaudio SOURCES test_backend_coreaudio.c LIBS de_audio TIMEOUT 20)
    target_include_directories(test_audio_coreaudio PRIVATE "${DE_ROOT_DIR}/src")
    target_compile_definitions(test_audio_coreaudio PRIVATE DE_AUDIO_HAS_COREAUDIO=1)
endif()
if(TARGET test_audio_coreaudio)
    # Discovery happens after linking, so apply hardware serialization when CTest loads it.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/coreaudio_properties.cmake"
        "if(TEST audio_coreaudio.default_device_one_second_440hz_full_mixer)\n"
        "set_tests_properties(audio_coreaudio.default_device_one_second_440hz_full_mixer audio_coreaudio.uid_switch_and_listener_reopen_monotonic_within_500ms PROPERTIES RUN_SERIAL TRUE)\n"
        "endif()\n")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
        "${CMAKE_CURRENT_BINARY_DIR}/coreaudio_properties.cmake")
endif()

# DENG-309: portable format tests everywhere; real endpoint cases skip outside Windows.
if(TARGET de_audio)
    de_add_test(NAME audio_wasapi SOURCES test_backend_wasapi.c LIBS de_audio TIMEOUT 20)
    target_include_directories(test_audio_wasapi PRIVATE "${DE_ROOT_DIR}/src")
    if(WIN32)
        target_compile_definitions(test_audio_wasapi PRIVATE DE_AUDIO_HAS_WASAPI=1)
    endif()
endif()

# DENG-310: dynamic loader behavior is portable; Linux service/device tests skip elsewhere.
if(TARGET de_audio AND UNIX)
    de_add_test(NAME audio_linux SOURCES test_backend_linux.c LIBS de_audio TIMEOUT 20)
    target_include_directories(test_audio_linux PRIVATE "${DE_ROOT_DIR}/src")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(test_audio_linux PRIVATE _GNU_SOURCE=1)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND DE_ALSA_INCLUDE_DIR)
        target_include_directories(test_audio_linux SYSTEM PRIVATE ${DE_ALSA_INCLUDE_DIR})
        target_compile_definitions(test_audio_linux PRIVATE DE_AUDIO_HAS_ALSA=1)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND DE_PIPEWIRE_INCLUDE_DIR AND DE_SPA_INCLUDE_DIR)
        target_include_directories(test_audio_linux SYSTEM PRIVATE ${DE_PIPEWIRE_INCLUDE_DIR} ${DE_SPA_INCLUDE_DIR})
        target_compile_definitions(test_audio_linux PRIVATE DE_AUDIO_HAS_PIPEWIRE=1)
    endif()
endif()
