# src/audio: module `audio` (target de_audio), layer gameplay. SPEC §14.
#
# Shared file (append-only within S25): later tasks append their own block below, one per task.
#
# DENG-308: mixer core. The backend interface (backend.h), the device manager (device.c), the
# null backend, the mixer and its thread, the command queue, the real-time guard, the internal
# source interface (source.h), minimal voices (voice.c; DENG-312 takes it over) and the sound
# source stub (sound_asset.c; DENG-313 takes it over).
#
# OS backends (DENG-309 WASAPI, DENG-310 PipeWire/ALSA, DENG-311 Core Audio) each append a block
# that adds their sources and turns their backend on, for example:
#
#     if(TARGET de_audio AND APPLE)
#         target_sources(de_audio PRIVATE backend/coreaudio.c)
#         target_compile_definitions(de_audio PRIVATE DE_AUDIO_HAS_COREAUDIO=1)
#         target_link_libraries(de_audio PRIVATE "-framework AudioToolbox" ...)
#     endif()
#
# backend.h defaults every DE_AUDIO_HAS_<X> to 0 and declares the vtable of each backend that is
# on; device.c selects them (DE_AUDIO_DEFAULT: WASAPI, Core Audio, PipeWire then ALSA, else null).
de_add_module(
    NAME audio
    LAYER gameplay
    SOURCES
        audio_internal.h
        backend.h
        source.h
        audio.c
        backend_null.c
        command_queue.c
        device.c
        mixer.c
        mixer_thread.c
        rt_guard.c
        sound_asset.c
        voice.c
    DEPS platform asset ecs reflect
    OPTIONAL_OPTION DE_WITH_AUDIO
)

# DENG-314: DSP is usable by the cooker even with runtime audio disabled.
add_library(de_audio_dsp STATIC dsp/resample.c dsp/resample.h)
target_include_directories(de_audio_dsp PUBLIC "${DE_ROOT_DIR}/src")
target_link_libraries(de_audio_dsp PUBLIC de_core PRIVATE de_build_flags)
target_compile_options(de_audio_dsp PRIVATE ${DE_DETERMINISTIC_FLAGS})
if(TARGET de_audio)
    target_link_libraries(de_audio PRIVATE de_audio_dsp)
endif()

# DENG-312: ramps, deterministic bus tree and channel matrices.
if(TARGET de_audio)
    target_sources(de_audio PRIVATE bus.c ramp.c channel_mix.c)
endif()
# Aggregate module-object consumers also need the non-module DSP library.
if(TARGET de_audio)
    set_property(TARGET de_audio APPEND PROPERTY DE_MODULE_LIBS de_audio_dsp)
endif()

# DENG-313: DSND sources, asynchronous compressed reads and background decoding.
if(TARGET de_audio)
    target_sources(de_audio PRIVATE source/source_pcm.c source/source_procedural.c
        source/source_opus.c source/source_vorbis.c source/stream.c source/stream_internal.h)
    target_link_libraries(de_audio PRIVATE de_tp_stb de_tp_opus)
    set_property(TARGET de_audio APPEND PROPERTY DE_MODULE_LIBS de_tp_stb de_tp_opus)
endif()
