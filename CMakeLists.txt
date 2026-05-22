cmake_minimum_required(VERSION 3.25)

project(z_fleet VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_program(ZFLEET_GOLD_LINKER ld.gold)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND ZFLEET_GOLD_LINKER)
  add_link_options(-fuse-ld=gold)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(
    $<$<CONFIG:Release>:-Os>
    $<$<CONFIG:Release>:-ffunction-sections>
    $<$<CONFIG:Release>:-fdata-sections>
  )
  add_link_options(
    $<$<CONFIG:Release>:-Wl,--gc-sections>
  )
endif()

include(CTest)

if(BUILD_TESTING)
  add_subdirectory(tests/support)
endif()

add_subdirectory(libs/platform)
add_subdirectory(libs/core)
add_subdirectory(libs/crypto)
add_subdirectory(libs/protocol)
add_subdirectory(libs/transport)
add_subdirectory(libs/package)

add_subdirectory(apps/agent)
add_subdirectory(apps/installer)
add_subdirectory(apps/launcher)
add_subdirectory(apps/packager)
add_subdirectory(apps/server)

if(BUILD_TESTING)
  add_subdirectory(tests/integration)
endif()
