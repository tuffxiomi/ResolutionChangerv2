if(NOT DEFINED ENV{PACKAGE_FILE})
    message(FATAL_ERROR "PACKAGE_FILE environment variable is missing")
endif()

set(PACKAGE "$ENV{PACKAGE_FILE}")

if(NOT EXISTS "${PACKAGE}")
    message(FATAL_ERROR "Package was not created: ${PACKAGE}")
endif()

file(SIZE "${PACKAGE}" PACKAGE_SIZE)
if(PACKAGE_SIZE LESS 1024)
    message(FATAL_ERROR "Package looks too small: ${PACKAGE_SIZE} bytes")
endif()

message(STATUS "Verified package: ${PACKAGE}")
