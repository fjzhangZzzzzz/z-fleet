find_package(CLI11 CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS beast system)
find_package(tomlplusplus CONFIG REQUIRED)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ZFLEET_AGENT_VERSION)
string(STRIP "${ZFLEET_AGENT_VERSION}" ZFLEET_AGENT_VERSION)

add_library(zfleet_agent_app
  src/command_decoder.cpp
  src/config.cpp
  src/http2_control_client.cpp
  src/package_updater.cpp
  src/runtime.cpp
  src/state.cpp
)

target_link_libraries(zfleet_agent_app
  PUBLIC
    Boost::system
    Boost::beast
    Threads::Threads
    tomlplusplus::tomlplusplus
    zfleet::core
    zfleet::crypto
    zfleet::package
    zfleet::protocol
    zfleet::platform
    zfleet::transport
)

target_include_directories(zfleet_agent_app
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(zfleet_agent_app PUBLIC cxx_std_20)

add_executable(zfleet_agent
  src/main.cpp
)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/zfleet_agent_version.txt"
  "${ZFLEET_AGENT_VERSION}\n"
)

target_link_libraries(zfleet_agent
  PRIVATE
    CLI11::CLI11
    zfleet_agent_app
)

target_compile_features(zfleet_agent PRIVATE cxx_std_20)

set(ZFLEET_AGENT_RELEASE_MAX_BYTES 20971520 CACHE STRING
  "Maximum allowed stripped release size for zfleet_agent in bytes"
)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  if(CMAKE_STRIP)
    add_custom_command(TARGET zfleet_agent POST_BUILD
      COMMAND ${CMAKE_STRIP} --strip-unneeded $<TARGET_FILE:zfleet_agent>
      COMMAND ${CMAKE_COMMAND}
        -DZFLEET_FILE=$<TARGET_FILE:zfleet_agent>
        -DZFLEET_MAX_BYTES=${ZFLEET_AGENT_RELEASE_MAX_BYTES}
        -P ${CMAKE_SOURCE_DIR}/cmake/check_file_size.cmake
      COMMENT "Stripping and checking zfleet_agent release size"
      VERBATIM
    )
  else()
    message(WARNING "CMAKE_STRIP is not available; zfleet_agent release size check will be skipped")
  endif()
endif()

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_agent_tests
    tests/agent_test.cpp
  )

  target_link_libraries(zfleet_agent_tests
    PRIVATE
      Catch2::Catch2WithMain
      zfleet_agent_app
      zfleet_test_support
  )

  target_include_directories(zfleet_agent_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_features(zfleet_agent_tests PRIVATE cxx_std_20)

  catch_discover_tests(zfleet_agent_tests)
endif()
