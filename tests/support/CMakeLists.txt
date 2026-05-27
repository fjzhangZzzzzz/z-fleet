add_library(zfleet_test_support
  test_util.cpp
  sqlite_test_util.cpp
  server_event_test_util.cpp
  http2_test_util.cpp
)

find_package(SQLiteCpp CONFIG REQUIRED)

target_include_directories(zfleet_test_support
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/apps/server/src
)

target_link_libraries(zfleet_test_support
  PUBLIC
    SQLiteCpp
    zfleet::protocol
    zfleet::transport
)

target_compile_features(zfleet_test_support PUBLIC cxx_std_20)
