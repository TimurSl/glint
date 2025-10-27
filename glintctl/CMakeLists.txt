cmake_minimum_required(VERSION 3.15)
project(glintctl LANGUAGES CXX)

add_executable(glintctl
        src/main.cpp
        src/common/endpoint.cpp
        src/common/json_builder.cpp
)

if(WIN32)
    target_sources(glintctl PRIVATE src/windows/transport_win.cpp)
else()
    target_sources(glintctl PRIVATE src/linux/transport_unix.cpp)
endif()

target_include_directories(glintctl PRIVATE src)
target_compile_features(glintctl PRIVATE cxx_std_17)
