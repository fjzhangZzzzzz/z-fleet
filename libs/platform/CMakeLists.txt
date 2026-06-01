add_library(zfleet_platform
  src/file_permissions.cpp
  src/process.cpp
  src/system.cpp
)

add_library(zfleet::platform ALIAS zfleet_platform)

find_package(Boost REQUIRED COMPONENTS filesystem process system)
find_package(boost_filesystem CONFIG REQUIRED)

target_include_directories(zfleet_platform
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(zfleet_platform PUBLIC cxx_std_20)

target_link_libraries(zfleet_platform
  PUBLIC
    Boost::filesystem
    Boost::process
)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_platform_tests
    tests/file_permissions_test.cpp
    tests/process_test.cpp
    tests/system_test.cpp
  )

  add_executable(zfleet_platform_process_fixture
    tests/process_fixture.cpp
  )

  target_link_libraries(zfleet_platform_tests
    PRIVATE
      Catch2::Catch2WithMain
      zfleet::platform
      zfleet_test_support
  )

  target_compile_definitions(zfleet_platform_tests
    PRIVATE
      ZFLEET_TEST_PROCESS_FIXTURE="$<TARGET_FILE:zfleet_platform_process_fixture>"
  )

  target_link_libraries(zfleet_platform_process_fixture
    PRIVATE
      zfleet::platform
  )

  catch_discover_tests(zfleet_platform_tests)
endif()
