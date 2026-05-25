add_library(zfleet_server_app
  src/config.cpp
  src/database.cpp
  src/http2_connection_registry.cpp
  src/http2_control_dispatcher.cpp
  src/http2_control_server.cpp
  src/http2_control_service.cpp
  src/install_script_renderer.cpp
  src/management_json_codec.cpp
  src/management_http_server.cpp
  src/package_repository.cpp
  src/static_file_service.cpp
)

set(ZFLEET_SERVER_VERSION "${PROJECT_VERSION}")

find_package(Boost REQUIRED COMPONENTS beast system)
find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLiteCpp CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

target_link_libraries(zfleet_server_app
  PUBLIC
    Boost::beast
    Boost::system
    nlohmann_json::nlohmann_json
    SQLiteCpp
    tomlplusplus::tomlplusplus
    unofficial::sqlite3::sqlite3
    zfleet::core
    zfleet::crypto
    zfleet::package
    zfleet::protocol
    zfleet::transport
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
