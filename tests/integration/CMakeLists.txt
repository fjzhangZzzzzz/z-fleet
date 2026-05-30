find_package(Catch2 CONFIG REQUIRED)
include(Catch)

add_executable(zfleet_integration_tests
  control_command_stream_test.cpp
  agent_runtime_test.cpp
  control_flow_test.cpp
  install_flow_test.cpp
  installer_stub_flow_test.cpp
  smoke_test.cpp
)

target_link_libraries(zfleet_integration_tests
  PRIVATE
    Catch2::Catch2WithMain
    Threads::Threads
    zfleet::core
    zfleet_agent_app
    zfleet_installer_app
    zfleet_server_app
    zfleet::crypto
    zfleet::package
    zfleet::platform
    zfleet_test_support
)

target_compile_features(zfleet_integration_tests PRIVATE cxx_std_20)

catch_discover_tests(zfleet_integration_tests)
