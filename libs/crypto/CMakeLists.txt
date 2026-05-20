find_package(OpenSSL REQUIRED)

add_library(zfleet_crypto
  src/sha256.cpp
)

add_library(zfleet::crypto ALIAS zfleet_crypto)

target_include_directories(zfleet_crypto
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(zfleet_crypto
  PRIVATE
    OpenSSL::Crypto
)

target_compile_features(zfleet_crypto PUBLIC cxx_std_20)

if(BUILD_TESTING)
  find_package(Catch2 CONFIG REQUIRED)
  include(Catch)

  add_executable(zfleet_crypto_tests
    tests/sha256_test.cpp
  )

  target_link_libraries(zfleet_crypto_tests
    PRIVATE
      Catch2::Catch2WithMain
      zfleet::crypto
      zfleet_test_support
  )

  target_compile_features(zfleet_crypto_tests PRIVATE cxx_std_20)

  catch_discover_tests(zfleet_crypto_tests)
endif()
