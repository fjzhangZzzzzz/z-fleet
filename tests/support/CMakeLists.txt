add_library(zfleet_test_support
  test_util.cpp
)

target_include_directories(zfleet_test_support
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(zfleet_test_support PUBLIC cxx_std_20)
