add_library(zfleet_package
  src/archive.cpp
)

add_library(zfleet::package ALIAS zfleet_package)

find_package(ZLIB REQUIRED)

target_include_directories(zfleet_package
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(zfleet_package
  PRIVATE
    ZLIB::ZLIB
)

target_compile_features(zfleet_package PUBLIC cxx_std_20)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_package_tests
    tests/archive_test.cpp
  )

  target_link_libraries(zfleet_package_tests
    PRIVATE
      Catch2::Catch2WithMain
      ZLIB::ZLIB
      zfleet::package
  )

  target_compile_features(zfleet_package_tests PRIVATE cxx_std_20)

  catch_discover_tests(zfleet_package_tests)
endif()
