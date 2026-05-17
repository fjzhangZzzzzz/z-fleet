add_library(zfleet_protocol
  src/json_codec.cpp
  src/message.cpp
)

add_library(zfleet::protocol ALIAS zfleet_protocol)

find_package(nlohmann_json CONFIG REQUIRED)

target_include_directories(zfleet_protocol
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(zfleet_protocol
  PRIVATE
    nlohmann_json::nlohmann_json
)

target_compile_features(zfleet_protocol PUBLIC cxx_std_20)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_protocol_tests
    tests/message_test.cpp
  )

  target_link_libraries(zfleet_protocol_tests
    PRIVATE
      Catch2::Catch2WithMain
      zfleet::protocol
  )

  catch_discover_tests(zfleet_protocol_tests)
endif()
