cmake_minimum_required(VERSION 3.21)
project(glint LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(glintd)
add_subdirectory(glintctl)
