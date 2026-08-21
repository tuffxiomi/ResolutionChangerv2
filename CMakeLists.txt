cmake_minimum_required(VERSION 3.22)

project(ResolutionChanger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(MOD_ID "resolutionchanger" CACHE STRING "Package directory ID")
set(MOD_NAME "ResolutionChanger" CACHE STRING "Display name")
set(MOD_AUTHOR "xiomi" CACHE STRING "Author")
set(MOD_VERSION "1.0.0" CACHE STRING "Version")
set(MOD_LIBRARY_NAME "ResolutionChanger" CACHE STRING "Library name without lib prefix or .so")

include(FetchContent)

FetchContent_Declare(
    preloader_android
    GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
    GIT_TAG 0.2.2
    SOURCE_SUBDIR cmake/preloader-sdk
)

FetchContent_MakeAvailable(preloader_android)

FetchContent_GetProperties(preloader_android)
set(PRELOADER_INCLUDE_DIR "${preloader_android_SOURCE_DIR}/include")

if(NOT ANDROID)
    message(FATAL_ERROR "ResolutionChanger is an Android native mod and must be configured with the Android toolchain.")
endif()

if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a" AND
   NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "armeabi-v7a")
    message(FATAL_ERROR "Unsupported Android ABI: ${CMAKE_ANDROID_ARCH_ABI}")
endif()

if(NOT TARGET preloader)
    add_subdirectory(
        "${preloader_android_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/_preloader_android"
    )
endif()

add_library(${MOD_LIBRARY_NAME} SHARED
    src/main.cpp
)

set_target_properties(${MOD_LIBRARY_NAME} PROPERTIES
    OUTPUT_NAME "${MOD_LIBRARY_NAME}"
)

target_include_directories(${MOD_LIBRARY_NAME}
    PRIVATE
        "${PRELOADER_INCLUDE_DIR}"
)

target_compile_options(${MOD_LIBRARY_NAME}
    PRIVATE
        -fvisibility=hidden
        -fvisibility-inlines-hidden
        -fno-rtti
        -ffunction-sections
        -fdata-sections
)

target_link_options(${MOD_LIBRARY_NAME}
    PRIVATE
        -Wl,--gc-sections
        -Wl,-z,max-page-size=16384
)

find_library(ANDROID_LIB android REQUIRED)
find_library(EGL_LIB EGL REQUIRED)

target_link_libraries(${MOD_LIBRARY_NAME}
    PRIVATE
        preloader
        ${ANDROID_LIB}
        ${EGL_LIB}
        dl
)

set(PACKAGE_ROOT "${CMAKE_BINARY_DIR}/package")
set(PACKAGE_MOD_DIR "${PACKAGE_ROOT}/${MOD_ID}")
set(PACKAGE_FILE "${CMAKE_BINARY_DIR}/${MOD_ID}-${MOD_VERSION}-${CMAKE_ANDROID_ARCH_ABI}.levipack")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json.in"
    "${CMAKE_BINARY_DIR}/manifest.json"
    @ONLY
)

add_custom_target(levi_package
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${PACKAGE_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PACKAGE_MOD_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "$<TARGET_FILE:${MOD_LIBRARY_NAME}>"
            "${PACKAGE_MOD_DIR}/lib${MOD_LIBRARY_NAME}.so"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_BINARY_DIR}/manifest.json"
            "${PACKAGE_MOD_DIR}/manifest.json"
    COMMAND "${CMAKE_COMMAND}" -E chdir
            "${PACKAGE_ROOT}"
            "${CMAKE_COMMAND}" -E tar
            "cf" "${PACKAGE_FILE}" --format=zip "${MOD_ID}"
    DEPENDS ${MOD_LIBRARY_NAME}
    COMMENT "Packaging ResolutionChanger"
    VERBATIM
)

add_custom_target(verify_package ALL
    COMMAND "${CMAKE_COMMAND}" -E env
            PACKAGE_FILE="${PACKAGE_FILE}"
            "${CMAKE_COMMAND}" -P
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_package.cmake"
    DEPENDS levi_package
    COMMENT "Verifying levipack package"
    VERBATIM
)
