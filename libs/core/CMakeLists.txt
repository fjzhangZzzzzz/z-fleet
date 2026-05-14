add_library(zfleet_core
  src/log.cpp
  src/time.cpp
  src/uuid.cpp
  src/version.cpp
)

add_library(zfleet::core ALIAS zfleet_core)

find_package(Boost REQUIRED COMPONENTS uuid)
find_package(spdlog CONFIG REQUIRED)

target_include_directories(zfleet_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(zfleet_core
  PUBLIC
    Boost::uuid
  PRIVATE
    spdlog::spdlog
)

target_compile_features(zfleet_core PUBLIC cxx_std_20)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_core_tests
    tests/log_test.cpp
    tests/time_test.cpp
    tests/version_test.cpp
  )

  target_link_libraries(zfleet_core_tests
    PRIVATE
      Catch2::Catch2WithMain
      zfleet::core
  )

  catch_discover_tests(zfleet_core_tests)
endif()
