add_executable(zfleet_server
  src/main.cpp
)

target_link_libraries(zfleet_server
  PRIVATE
    zfleet::core
    zfleet::protocol
    zfleet::platform
)

target_compile_features(zfleet_server PRIVATE cxx_std_20)
