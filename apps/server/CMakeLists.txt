add_library(zfleet_server_app
  src/config.cpp
  src/database.cpp
  src/grpc_control_server.cpp
  src/grpc_control_service.cpp
  src/http_handler.cpp
  src/http_server.cpp
)

set(ZFLEET_SERVER_VERSION "${PROJECT_VERSION}")

find_package(CLI11 CONFIG REQUIRED)
find_package(SQLiteCpp CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(tomlplusplus CONFIG REQUIRED)

target_link_libraries(zfleet_server_app
  PUBLIC
    Boost::system
    SQLiteCpp
    tomlplusplus::tomlplusplus
    zfleet::core
    zfleet::protocol
)

target_include_directories(zfleet_server_app
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(zfleet_server_app PUBLIC cxx_std_20)

add_executable(zfleet_server
  src/main.cpp
)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/zfleet_server_version.txt"
  "${ZFLEET_SERVER_VERSION}\n"
)

target_link_libraries(zfleet_server
  PRIVATE
    CLI11::CLI11
    zfleet::platform
    zfleet_server_app
)

target_compile_features(zfleet_server PRIVATE cxx_std_20)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_server_tests
    tests/server_test.cpp
  )

  target_link_libraries(zfleet_server_tests
    PRIVATE
      Catch2::Catch2WithMain
      zfleet_server_app
      zfleet_test_support
  )

  target_include_directories(zfleet_server_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_features(zfleet_server_tests PRIVATE cxx_std_20)

  catch_discover_tests(zfleet_server_tests)
endif()
