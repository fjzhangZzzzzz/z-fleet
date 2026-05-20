add_library(zfleet_platform
  src/file_permissions.cpp
  src/system.cpp
)

add_library(zfleet::platform ALIAS zfleet_platform)

target_include_directories(zfleet_platform
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(zfleet_platform PUBLIC cxx_std_20)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_platform_tests
    tests/file_permissions_test.cpp
    tests/system_test.cpp
  )

  target_link_libraries(zfleet_platform_tests
    PRIVATE
      Catch2::Catch2WithMain
      zfleet::platform
  )

  catch_discover_tests(zfleet_platform_tests)
endif()
