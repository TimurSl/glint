cmake_minimum_required(VERSION 3.15)
project(glintctl LANGUAGES CXX)

add_executable(glintctl
        src/glintctl.cpp
)

target_compile_features(glintctl PRIVATE cxx_std_17)

if (MSVC)
    target_compile_definitions(glintctl PRIVATE _WIN32_WINNT=0x0601)
endif()
