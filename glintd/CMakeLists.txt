cmake_minimum_required(VERSION 3.21)
project(glintd LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMMON_SOURCES
        src/common/capture_base.h
        src/common/capture_base.cpp
        src/common/config.h
        src/common/config.cpp
        src/common/detector.h
        src/common/detector.cpp
        src/common/ipc_server.h
        src/common/ipc_server_stdin.h
        src/common/ipc_server_stdin.cpp
        src/common/logger.h
        src/common/logger.cpp
        src/common/marker_manager.h
        src/common/marker_manager.cpp
        src/common/replay_buffer.h
        src/common/replay_buffer.cpp
        src/common/ipc_server_pipe.h
        src/rpc/handlers.cpp
        src/rpc/handlers.h
        src/common/db.cpp
        src/common/db.h
        src/common/encoder.h
        src/common/muxer.h
        src/common/ff/encoder_ffmpeg.cpp
        src/common/ff/encoder_ffmpeg.h
        src/common/ff/muxer_avformat.cpp
        src/common/ff/muxer_avformat.h
        src/common/recording_pipeline.cpp
        src/common/recording_pipeline.h
        src/common/ffmpeg_common.h
        src/common/recorder.cpp
        src/common/recorder.h
        src/common/frame_types.h
)

set(FFMPEG_ROOT "C:/Dev/ffmpeg")
set(CMAKE_PREFIX_PATH "${FFMPEG_ROOT}")
link_directories("${FFMPEG_ROOT}/lib")
include_directories("${FFMPEG_ROOT}/include")

find_library(AVCODEC_LIBRARY avcodec REQUIRED)
find_library(AVFORMAT_LIBRARY avformat REQUIRED)
find_library(AVUTIL_LIBRARY avutil REQUIRED)
find_library(SWSCALE_LIBRARY swscale REQUIRED)
find_library(SWRESAMPLE_LIBRARY swresample REQUIRED)

if (WIN32)
    # DXGI + D3D11 + WASAPI
    set(OS_LIBS
            dxgi
            d3d11
            d3dcompiler
            ole32
            avrt
            mmdevapi
            uuid
    )
else ()
    find_library(PIPEWIRE_LIBRARY pipewire-0.3 OPTIONAL)
    find_library(X11_LIBRARY X11 OPTIONAL)
    find_library(PULSE_LIBRARY pulse OPTIONAL)
    find_library(PULSE_SIMPLE_LIBRARY pulse-simple OPTIONAL)
    set(OS_LIBS ${PIPEWIRE_LIBRARY} ${X11_LIBRARY} ${PULSE_LIBRARY} ${PULSE_SIMPLE_LIBRARY})
endif ()

if (WIN32)
    set(PLATFORM_SOURCES
            src/windows/capture_dxgi_stub.cpp
            src/windows/ipc_server_pipe_win.cpp
            src/windows/capture_dxgi.cpp
            src/windows/ks_guids.cpp)
    add_definitions(-DGLINT_WINDOWS)
else ()
    set(PLATFORM_SOURCES
            src/linux/capture_linux_stub.cpp
            src/linux/ipc_server_pipe_unix.cpp
            src/common/constants.h
            src/windows/audio_wasapi.cpp)
    add_definitions(-DGLINT_LINUX)
endif ()

add_executable(glintd
        src/main.cpp
        ${COMMON_SOURCES}
        ${PLATFORM_SOURCES}
)
add_library(sqlite3 STATIC include/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include/sqlite)
target_link_libraries(glintd PRIVATE sqlite3)

target_include_directories(glintd PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(glintd PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        GLINT_ENABLE_NVENC
)

target_link_libraries(glintd PRIVATE
        ${AVCODEC_LIBRARY}
        ${AVFORMAT_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWSCALE_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${OS_LIBS}
)

if (WIN32)
    target_link_libraries(glintd PRIVATE
            Dwmapi
    )
else ()
    target_link_libraries(glintd PRIVATE pthread)
endif ()
