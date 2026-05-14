add_library(zfleet_core
  src/uuid.cpp
  src/version.cpp
)

add_library(zfleet::core ALIAS zfleet_core)

find_package(Boost REQUIRED COMPONENTS uuid)

target_include_directories(zfleet_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(zfleet_core
  PUBLIC
    Boost::uuid
)

target_compile_features(zfleet_core PUBLIC cxx_std_20)

if(BUILD_TESTING)
  add_executable(zfleet_core_tests
    tests/version_test.cpp
  )

  target_link_libraries(zfleet_core_tests
    PRIVATE
      zfleet::core
  )

  add_test(NAME zfleet_core_tests COMMAND zfleet_core_tests)
endif()
