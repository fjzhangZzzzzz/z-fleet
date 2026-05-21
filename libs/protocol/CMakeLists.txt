find_package(nlohmann_json CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(ZFLEET_PROTOCOL_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(ZFLEET_PROTOCOL_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ZFLEET_PROTOCOL_PROTO_FILES
  "${ZFLEET_PROTOCOL_PROTO_DIR}/zfleet/protocol/v1/agent_control.proto"
)
set(ZFLEET_PROTOCOL_GENERATED_SOURCES
  "${ZFLEET_PROTOCOL_GENERATED_DIR}/zfleet/protocol/v1/agent_control.pb.cc"
  "${ZFLEET_PROTOCOL_GENERATED_DIR}/zfleet/protocol/v1/agent_control.grpc.pb.cc"
)
set(ZFLEET_PROTOCOL_GENERATED_HEADERS
  "${ZFLEET_PROTOCOL_GENERATED_DIR}/zfleet/protocol/v1/agent_control.pb.h"
  "${ZFLEET_PROTOCOL_GENERATED_DIR}/zfleet/protocol/v1/agent_control.grpc.pb.h"
)

get_target_property(ZFLEET_GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin LOCATION)

add_custom_command(
  OUTPUT
    ${ZFLEET_PROTOCOL_GENERATED_SOURCES}
    ${ZFLEET_PROTOCOL_GENERATED_HEADERS}
  COMMAND ${CMAKE_COMMAND} -E make_directory "${ZFLEET_PROTOCOL_GENERATED_DIR}"
  COMMAND protobuf::protoc
    --proto_path="${ZFLEET_PROTOCOL_PROTO_DIR}"
    --cpp_out="${ZFLEET_PROTOCOL_GENERATED_DIR}"
    --grpc_out="${ZFLEET_PROTOCOL_GENERATED_DIR}"
    --plugin=protoc-gen-grpc="${ZFLEET_GRPC_CPP_PLUGIN}"
    ${ZFLEET_PROTOCOL_PROTO_FILES}
  DEPENDS
    ${ZFLEET_PROTOCOL_PROTO_FILES}
    protobuf::protoc
    gRPC::grpc_cpp_plugin
  VERBATIM
)

add_library(zfleet_protocol
  src/json_codec.cpp
  src/message.cpp
  ${ZFLEET_PROTOCOL_GENERATED_SOURCES}
)

add_library(zfleet::protocol ALIAS zfleet_protocol)

target_include_directories(zfleet_protocol
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${ZFLEET_PROTOCOL_GENERATED_DIR}>
)

target_link_libraries(zfleet_protocol
  PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
  PRIVATE
    nlohmann_json::nlohmann_json
)

target_compile_features(zfleet_protocol PUBLIC cxx_std_20)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_protocol_tests
    tests/grpc_protocol_test.cpp
    tests/message_test.cpp
  )

  target_link_libraries(zfleet_protocol_tests
    PRIVATE
      Catch2::Catch2WithMain
      zfleet::protocol
  )

  catch_discover_tests(zfleet_protocol_tests)
endif()
