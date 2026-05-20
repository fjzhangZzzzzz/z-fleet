find_package(CLI11 CONFIG REQUIRED)

add_library(zfleet_packager_app
  src/packager.cpp
)

target_link_libraries(zfleet_packager_app
  PRIVATE
    zfleet::core
    zfleet::crypto
    zfleet::package
)

target_include_directories(zfleet_packager_app
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(zfleet_packager_app PUBLIC cxx_std_20)

add_executable(zfleet_packager
  src/main.cpp
)

target_link_libraries(zfleet_packager
  PRIVATE
    CLI11::CLI11
    zfleet_packager_app
)

target_compile_features(zfleet_packager PRIVATE cxx_std_20)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_packager_tests
    tests/packager_test.cpp
  )

  target_link_libraries(zfleet_packager_tests
    PRIVATE
      Catch2::Catch2WithMain
      zfleet::crypto
      zfleet::package
      zfleet_packager_app
      zfleet_test_support
  )

  target_include_directories(zfleet_packager_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_features(zfleet_packager_tests PRIVATE cxx_std_20)

  catch_discover_tests(zfleet_packager_tests)
endif()
