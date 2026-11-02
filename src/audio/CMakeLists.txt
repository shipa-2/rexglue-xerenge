# rexaudio - Audio subsystem library
# Audio processing and XMA decoding for Xbox 360 emulation

add_library(rexaudio OBJECT
    
    audio_driver.cpp
    audio_system.cpp
    downmix.cpp
    xma_context.cpp
    xma_decoder.cpp
    xma_register_file.cpp
    # NOP backend (always available)
    nop/nop_audio_system.cpp
    # SDL backend
    sdl/sdl_audio_system.cpp
    sdl/sdl_audio_driver.cpp
)
if(REX_UWP)
    target_sources(rexaudio PRIVATE xaudio2/xaudio2_audio_system.cpp xaudio2/xaudio2_audio_driver.cpp)
    target_link_libraries(rexaudio PUBLIC xaudio2)
    get_target_property(_rexaudio_sources rexaudio SOURCES)
    list(REMOVE_ITEM _rexaudio_sources sdl/sdl_audio_system.cpp sdl/sdl_audio_driver.cpp)
    set_target_properties(rexaudio PROPERTIES SOURCES "${_rexaudio_sources}")
endif()
add_library(rex::audio ALIAS rexaudio)

rex_target_pch(rexaudio
    <mutex>
    <rex/math.h>
    <rex/string.h>
    <rex/memory.h>
    <rex/filesystem.h>
    <rex/thread.h>
    <rex/thread/mutex.h>
    <rex/ppc/context.h>
    <rex/system/kernel_state.h>
    <rex/system/xthread.h>
)

target_include_directories(rexaudio PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(rexaudio
    PUBLIC rexcore SDL3::SDL3
    PRIVATE libavcodec libavutil libavformat libswresample
)
