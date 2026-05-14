find_package(Catch2 CONFIG REQUIRED)
include(Catch)

add_executable(zfleet_integration_tests
  smoke_test.cpp
)

target_link_libraries(zfleet_integration_tests
  PRIVATE
    Catch2::Catch2WithMain
    zfleet::core
    zfleet::platform
    zfleet::protocol
)

target_compile_features(zfleet_integration_tests PRIVATE cxx_std_20)

catch_discover_tests(zfleet_integration_tests)
