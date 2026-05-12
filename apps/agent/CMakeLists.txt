add_executable(zfleet_agent
  src/main.cpp
)

target_link_libraries(zfleet_agent
  PRIVATE
    zfleet::core
    zfleet::protocol
    zfleet::platform
)

target_compile_features(zfleet_agent PRIVATE cxx_std_20)
