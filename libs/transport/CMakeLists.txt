find_path(NGHTTP2_INCLUDE_DIR
  NAMES nghttp2/nghttp2.h
  REQUIRED
)

find_library(NGHTTP2_LIBRARY
  NAMES nghttp2
  REQUIRED
)

add_library(zfleet_nghttp2 INTERFACE)
add_library(zfleet::nghttp2 ALIAS zfleet_nghttp2)

target_include_directories(zfleet_nghttp2 SYSTEM INTERFACE
  "${NGHTTP2_INCLUDE_DIR}"
)

target_link_libraries(zfleet_nghttp2 INTERFACE
  "${NGHTTP2_LIBRARY}"
)

add_library(zfleet_transport
  src/frame_codec.cpp
  src/http2_session.cpp
)

add_library(zfleet::transport ALIAS zfleet_transport)

target_include_directories(zfleet_transport
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(zfleet_transport
  PUBLIC
    zfleet::nghttp2
)

target_compile_features(zfleet_transport PUBLIC cxx_std_20)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_transport_tests
    tests/frame_codec_test.cpp
  )

  target_link_libraries(zfleet_transport_tests
    PRIVATE
      Catch2::Catch2WithMain
      zfleet::transport
  )

  catch_discover_tests(zfleet_transport_tests)
endif()
