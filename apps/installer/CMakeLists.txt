find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ZFLEET_INSTALLER_VERSION)
string(STRIP "${ZFLEET_INSTALLER_VERSION}" ZFLEET_INSTALLER_VERSION)

add_library(zfleet_installer_app
  src/installer.cpp
  src/json_codec.cpp
)

add_library(zfleet_installer_launcher_app
  launcher/src/launcher.cpp
)

target_link_libraries(zfleet_installer_app
  PRIVATE
    nlohmann_json::nlohmann_json
    zfleet::core
    zfleet::crypto
    zfleet::package
    zfleet::platform
)

target_link_libraries(zfleet_installer_launcher_app
  PUBLIC
    zfleet::core
    zfleet::platform
)

target_include_directories(zfleet_installer_app
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(zfleet_installer_launcher_app
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src
)

target_compile_features(zfleet_installer_app PUBLIC cxx_std_20)
target_compile_features(zfleet_installer_launcher_app PUBLIC cxx_std_20)

add_executable(zfleet_installer
  src/main.cpp
)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/zfleet_installer_version.txt"
  "${ZFLEET_INSTALLER_VERSION}\n"
)

target_link_libraries(zfleet_installer
  PRIVATE
    CLI11::CLI11
    zfleet_installer_app
)

target_compile_features(zfleet_installer PRIVATE cxx_std_20)
zf_configure_runtime_layout(zfleet_installer)

add_executable(zfleet_launcher
  launcher/src/main.cpp
)

target_link_libraries(zfleet_launcher
  PRIVATE
    zfleet_installer_launcher_app
)

set_target_properties(zfleet_launcher PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/launcher"
)

target_compile_features(zfleet_launcher PRIVATE cxx_std_20)
zf_configure_runtime_layout(zfleet_launcher)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_installer_tests
    tests/installer_test.cpp
  )

  target_link_libraries(zfleet_installer_tests
    PRIVATE
      Catch2::Catch2WithMain
      zfleet::core
      zfleet::crypto
      zfleet::package
      zfleet::platform
      zfleet_installer_app
      zfleet_test_support
  )

  target_include_directories(zfleet_installer_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_features(zfleet_installer_tests PRIVATE cxx_std_20)

  catch_discover_tests(zfleet_installer_tests)

  add_executable(zfleet_installer_launcher_tests
    launcher/tests/launcher_test.cpp
  )
  add_executable(zfleet_launcher_target_fixture
    launcher/tests/launcher_target_fixture.cpp
  )

  target_link_libraries(zfleet_installer_launcher_tests
    PRIVATE
      Catch2::Catch2WithMain
      zfleet::platform
      zfleet_installer_launcher_app
      zfleet_test_support
  )

  target_include_directories(zfleet_installer_launcher_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src
  )

  target_compile_definitions(zfleet_installer_launcher_tests
    PRIVATE
      ZFLEET_TEST_AGENT_LAUNCHER="$<TARGET_FILE:zfleet_launcher>"
      ZFLEET_TEST_LAUNCHER_TARGET_FIXTURE="$<TARGET_FILE:zfleet_launcher_target_fixture>"
  )

  target_compile_features(zfleet_installer_launcher_tests PRIVATE cxx_std_20)
  target_compile_features(zfleet_launcher_target_fixture PRIVATE cxx_std_20)

  catch_discover_tests(zfleet_installer_launcher_tests)
endif()
