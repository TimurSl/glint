cmake_minimum_required(VERSION 3.21)
project(glintd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMMON_SOURCES
        src/common/capture_base.h
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
)

if(WIN32)
    set(PLATFORM_SOURCES
            src/windows/capture_dxgi_stub.cpp
            src/windows/ipc_server_pipe_win.cpp)
    add_definitions(-DGLINT_WINDOWS)
else()
    set(PLATFORM_SOURCES
            src/linux/capture_linux_stub.cpp
            src/linux/ipc_server_pipe_unix.cpp)
    add_definitions(-DGLINT_LINUX)
endif()

add_executable(glintd
        src/main.cpp
        ${COMMON_SOURCES}
        ${PLATFORM_SOURCES}
)

target_include_directories(glintd PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(WIN32)
    target_link_libraries(glintd PRIVATE
            Dwmapi
    )
else()
    target_link_libraries(glintd PRIVATE pthread)
endif()
