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
        source/source_opus.c source/source_vorbis.c source/stream.c source/stream_internal.h
        source/stream_ring.h)
    target_link_libraries(de_audio PRIVATE de_tp_stb de_tp_opus)
    set_property(TARGET de_audio APPEND PROPERTY DE_MODULE_LIBS de_tp_stb de_tp_opus)
endif()

# DENG-311: native pure-C HAL output; platform frameworks stay private to the backend.
if(TARGET de_audio AND APPLE)
    target_sources(de_audio PRIVATE backend/coreaudio.c backend/coreaudio_devices.c
        backend/coreaudio_internal.h)
    target_compile_definitions(de_audio PRIVATE DE_AUDIO_HAS_COREAUDIO=1)
    target_link_libraries(de_audio PRIVATE "-framework AudioToolbox" "-framework CoreAudio"
        "-framework CoreFoundation")
    set_property(TARGET de_audio APPEND PROPERTY DE_MODULE_LIBS "-framework AudioToolbox"
        "-framework CoreAudio" "-framework CoreFoundation")
endif()

# DENG-309: format/conversion helpers are portable so negotiation can run on every host.
if(TARGET de_audio)
    target_sources(de_audio PRIVATE backend/wasapi_format.c backend/wasapi_format.h)
endif()
if(TARGET de_audio AND WIN32)
    target_sources(de_audio PRIVATE backend/wasapi.c backend/wasapi_notify.c
        backend/wasapi_internal.h)
    target_compile_definitions(de_audio PRIVATE DE_AUDIO_HAS_WASAPI=1)
    target_link_libraries(de_audio PRIVATE ole32 avrt uuid)
    set_property(TARGET de_audio APPEND PROPERTY DE_MODULE_LIBS ole32 avrt uuid)
endif()

# DENG-310: Linux libraries are loaded at runtime; only their SDK headers are build dependencies.
if(TARGET de_audio AND UNIX AND NOT WIN32)
    target_sources(de_audio PRIVATE backend/linux_dl.c backend/linux_dl.h)
    target_link_libraries(de_audio PRIVATE ${CMAKE_DL_LIBS})
    set_property(TARGET de_audio APPEND PROPERTY DE_MODULE_LIBS ${CMAKE_DL_LIBS})
endif()
if(TARGET de_audio AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # ALSA/PipeWire public C SDK headers require POSIX/GNU libc declarations.
    target_compile_definitions(de_audio PRIVATE _GNU_SOURCE=1)
    find_path(DE_ALSA_INCLUDE_DIR alsa/asoundlib.h)
    find_path(DE_PIPEWIRE_INCLUDE_DIR pipewire/pipewire.h PATH_SUFFIXES pipewire-0.3)
    find_path(DE_SPA_INCLUDE_DIR spa/param/audio/format-utils.h PATH_SUFFIXES spa-0.2)
    if(DE_ALSA_INCLUDE_DIR)
        target_include_directories(de_audio SYSTEM PRIVATE ${DE_ALSA_INCLUDE_DIR})
        target_sources(de_audio PRIVATE backend/alsa.c)
        target_compile_definitions(de_audio PRIVATE DE_AUDIO_HAS_ALSA=1)
    else()
        message(STATUS "ALSA headers unavailable: backend omitted (install libasound2-dev)")
    endif()
    if(DE_PIPEWIRE_INCLUDE_DIR AND DE_SPA_INCLUDE_DIR)
        target_include_directories(de_audio SYSTEM PRIVATE ${DE_PIPEWIRE_INCLUDE_DIR} ${DE_SPA_INCLUDE_DIR})
        target_sources(de_audio PRIVATE backend/pipewire.c)
        target_compile_definitions(de_audio PRIVATE DE_AUDIO_HAS_PIPEWIRE=1)
    else()
        message(STATUS "PipeWire headers unavailable: backend omitted (install libpipewire-0.3-dev libspa-0.2-dev)")
    endif()
endif()
