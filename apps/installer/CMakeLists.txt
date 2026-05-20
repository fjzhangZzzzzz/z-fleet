find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

set(ZFLEET_INSTALLER_VERSION "${PROJECT_VERSION}")

add_library(zfleet_installer_app
  src/file_permissions.cpp
  src/installer.cpp
  src/json_codec.cpp
)

target_link_libraries(zfleet_installer_app
  PRIVATE
    nlohmann_json::nlohmann_json
    zfleet::core
    zfleet::crypto
    zfleet::package
)

target_include_directories(zfleet_installer_app
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(zfleet_installer_app PUBLIC cxx_std_20)

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

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_installer_tests
    tests/installer_test.cpp
  )

  target_link_libraries(zfleet_installer_tests
    PRIVATE
      Catch2::Catch2WithMain
      zfleet::crypto
      zfleet::package
      zfleet_installer_app
      zfleet_test_support
  )

  target_include_directories(zfleet_installer_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_features(zfleet_installer_tests PRIVATE cxx_std_20)

  catch_discover_tests(zfleet_installer_tests)
endif()
