cmake_minimum_required(VERSION 3.25)

project(z_fleet VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_subdirectory(libs/core)
add_subdirectory(libs/protocol)
add_subdirectory(libs/platform)

add_subdirectory(apps/agent)
add_subdirectory(apps/server)
