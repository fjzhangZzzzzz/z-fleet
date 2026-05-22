find_package(PkgConfig REQUIRED)
pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)

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
    PkgConfig::NGHTTP2
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
