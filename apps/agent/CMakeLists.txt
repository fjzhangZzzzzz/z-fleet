add_executable(zfleet_agent
  src/config.cpp
  src/state.cpp
  src/main.cpp
)

find_package(CLI11 CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)

target_link_libraries(zfleet_agent
  PRIVATE
    CLI11::CLI11
    tomlplusplus::tomlplusplus
    zfleet::core
    zfleet::protocol
    zfleet::platform
)

target_include_directories(zfleet_agent
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(zfleet_agent PRIVATE cxx_std_20)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_agent_tests
    tests/agent_test.cpp
    src/config.cpp
    src/state.cpp
  )

  target_link_libraries(zfleet_agent_tests
    PRIVATE
      Catch2::Catch2WithMain
      tomlplusplus::tomlplusplus
      zfleet::core
  )

  target_include_directories(zfleet_agent_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_features(zfleet_agent_tests PRIVATE cxx_std_20)

  catch_discover_tests(zfleet_agent_tests)
endif()
