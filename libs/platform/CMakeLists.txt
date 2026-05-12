add_library(zfleet_platform
  src/system.cpp
)

add_library(zfleet::platform ALIAS zfleet_platform)

target_include_directories(zfleet_platform
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(zfleet_platform PUBLIC cxx_std_20)

if(BUILD_TESTING)
  add_executable(zfleet_platform_tests
    tests/system_test.cpp
  )

  target_link_libraries(zfleet_platform_tests
    PRIVATE
      zfleet::platform
  )

  add_test(NAME zfleet_platform_tests COMMAND zfleet_platform_tests)
endif()
