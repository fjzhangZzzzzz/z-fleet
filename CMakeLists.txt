cmake_minimum_required(VERSION 3.25)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

project(z_fleet VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
  add_compile_definitions(
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
  )
endif()

if(MSVC)
  add_compile_options(
    /EHsc
    /utf-8
  )
endif()

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
find_package(Threads REQUIRED)

function(zf_configure_runtime_layout target_name)
  if(WIN32)
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:${target_name}>
        $<TARGET_FILE_DIR:${target_name}>
      COMMAND_EXPAND_LISTS
    )
  elseif(UNIX)
    set_target_properties(${target_name} PROPERTIES
      BUILD_RPATH "$ORIGIN/../lib"
      INSTALL_RPATH "$ORIGIN/../lib"
      SKIP_BUILD_RPATH FALSE
      BUILD_WITH_INSTALL_RPATH FALSE
    )
  endif()
endfunction()

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
add_subdirectory(apps/packager)
add_subdirectory(apps/server)

if(BUILD_TESTING)
  add_subdirectory(tests/integration)
endif()
