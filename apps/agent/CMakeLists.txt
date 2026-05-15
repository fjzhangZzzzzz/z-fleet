find_package(CLI11 CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(tomlplusplus CONFIG REQUIRED)

add_library(zfleet_agent_app
  src/app.cpp
  src/config.cpp
  src/state.cpp
)

target_link_libraries(zfleet_agent_app
  PUBLIC
    Boost::system
    tomlplusplus::tomlplusplus
    zfleet::core
    zfleet::protocol
    zfleet::platform
)

target_include_directories(zfleet_agent_app
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(zfleet_agent_app PUBLIC cxx_std_20)

add_executable(zfleet_agent
  src/main.cpp
)

target_link_libraries(zfleet_agent
  PRIVATE
    CLI11::CLI11
    zfleet_agent_app
)

target_compile_features(zfleet_agent PRIVATE cxx_std_20)

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
  )

  target_include_directories(zfleet_agent_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_features(zfleet_agent_tests PRIVATE cxx_std_20)

  catch_discover_tests(zfleet_agent_tests)
endif()
