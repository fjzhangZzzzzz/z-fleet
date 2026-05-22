if(NOT DEFINED ZFLEET_FILE)
  message(FATAL_ERROR "ZFLEET_FILE is required")
endif()

if(NOT DEFINED ZFLEET_MAX_BYTES)
  message(FATAL_ERROR "ZFLEET_MAX_BYTES is required")
endif()

if(NOT EXISTS "${ZFLEET_FILE}")
  message(FATAL_ERROR "File does not exist: ${ZFLEET_FILE}")
endif()

file(SIZE "${ZFLEET_FILE}" actual_bytes)

if(actual_bytes GREATER ZFLEET_MAX_BYTES)
  math(EXPR actual_mib "(${actual_bytes} + 1048575) / 1048576")
  math(EXPR max_mib "(${ZFLEET_MAX_BYTES} + 1048575) / 1048576")
  message(FATAL_ERROR
    "${ZFLEET_FILE} is ${actual_bytes} bytes (${actual_mib} MiB), "
    "exceeding the ${ZFLEET_MAX_BYTES} byte (${max_mib} MiB) limit"
  )
endif()

message(STATUS
  "${ZFLEET_FILE} size ${actual_bytes} bytes is within "
  "${ZFLEET_MAX_BYTES} byte limit"
)
