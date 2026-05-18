add_library(zfleet_launcher_app
  src/launcher.cpp
)

target_include_directories(zfleet_launcher_app
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(zfleet_launcher_app PUBLIC cxx_std_20)

function(add_zfleet_launcher launcher_target output_name)
  add_executable(${launcher_target}
    src/main.cpp
  )

  target_link_libraries(${launcher_target}
    PRIVATE
      zfleet_launcher_app
  )

  set_target_properties(${launcher_target} PROPERTIES
    OUTPUT_NAME ${output_name}
  )

  target_compile_features(${launcher_target} PRIVATE cxx_std_20)
endfunction()

add_zfleet_launcher(zfleet_agent_launcher zfleet_agent)
add_zfleet_launcher(zfleet_server_launcher zfleet_server)
add_zfleet_launcher(zfleet_installer_launcher zfleet_installer)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_launcher_tests
    tests/launcher_test.cpp
  )

  target_link_libraries(zfleet_launcher_tests
    PRIVATE
      Catch2::Catch2WithMain
      zfleet_launcher_app
  )

  target_include_directories(zfleet_launcher_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_definitions(zfleet_launcher_tests
    PRIVATE
      ZFLEET_TEST_AGENT_LAUNCHER="$<TARGET_FILE:zfleet_agent_launcher>"
  )

  target_compile_features(zfleet_launcher_tests PRIVATE cxx_std_20)

  catch_discover_tests(zfleet_launcher_tests)
endif()
