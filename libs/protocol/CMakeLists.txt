add_library(zfleet_protocol
  src/message.cpp
)

add_library(zfleet::protocol ALIAS zfleet_protocol)

target_include_directories(zfleet_protocol
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(zfleet_protocol PUBLIC cxx_std_20)

if(BUILD_TESTING)
  add_executable(zfleet_protocol_tests
    tests/message_test.cpp
  )

  target_link_libraries(zfleet_protocol_tests
    PRIVATE
      zfleet::protocol
  )

  add_test(NAME zfleet_protocol_tests COMMAND zfleet_protocol_tests)
endif()
