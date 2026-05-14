add_executable(zfleet_server
  src/config.cpp
  src/database.cpp
  src/main.cpp
)

find_package(CLI11 CONFIG REQUIRED)
find_package(SQLiteCpp CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)

target_link_libraries(zfleet_server
  PRIVATE
    CLI11::CLI11
    SQLiteCpp
    tomlplusplus::tomlplusplus
    zfleet::core
    zfleet::protocol
    zfleet::platform
)

target_include_directories(zfleet_server
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(zfleet_server PRIVATE cxx_std_20)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_server_tests
    tests/server_test.cpp
    src/config.cpp
    src/database.cpp
  )

  target_link_libraries(zfleet_server_tests
    PRIVATE
      Catch2::Catch2WithMain
      SQLiteCpp
      tomlplusplus::tomlplusplus
      zfleet::core
  )

  target_include_directories(zfleet_server_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_features(zfleet_server_tests PRIVATE cxx_std_20)

  catch_discover_tests(zfleet_server_tests)
endif()
