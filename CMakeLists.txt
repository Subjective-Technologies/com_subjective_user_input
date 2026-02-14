cmake_minimum_required(VERSION 3.16)

project(subjective_user_input_c C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SRC_FILES
    src/input_unified.c
)

add_executable(input_unified ${SRC_FILES})

if(WIN32)
    target_compile_definitions(input_unified PRIVATE WIN32)
endif()

if(MSVC)
    target_compile_definitions(input_unified PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Platform output folder
if(WIN32)
    set(PLATFORM_DIR "windows")
elseif(APPLE)
    set(PLATFORM_DIR "macos")
else()
    set(PLATFORM_DIR "linux")
endif()

set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/build/${PLATFORM_DIR}")
set_target_properties(input_unified PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUTPUT_DIR}"
)

# OpenSSL
# Allow OPENSSL_ROOT_DIR to be set via environment variable or CMake variable
if(DEFINED ENV{OPENSSL_ROOT_DIR} AND NOT DEFINED OPENSSL_ROOT_DIR)
    set(OPENSSL_ROOT_DIR $ENV{OPENSSL_ROOT_DIR})
endif()

find_package(OpenSSL REQUIRED)
target_link_libraries(input_unified PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# libwebsockets
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LWS QUIET libwebsockets)
endif()

if(LWS_FOUND)
    target_include_directories(input_unified PRIVATE ${LWS_INCLUDE_DIRS})
    target_link_directories(input_unified PRIVATE ${LWS_LIBRARY_DIRS})
    target_link_libraries(input_unified PRIVATE ${LWS_LIBRARIES})
    target_compile_options(input_unified PRIVATE ${LWS_CFLAGS_OTHER})
    target_link_options(input_unified PRIVATE ${LWS_LDFLAGS_OTHER})
else()
    find_package(libwebsockets CONFIG REQUIRED)
    target_link_libraries(input_unified PRIVATE websockets_shared)
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(input_unified PRIVATE Threads::Threads m)
    # X11 + extensions
    pkg_check_modules(X11 REQUIRED x11)
    pkg_check_modules(XTST REQUIRED xtst)
    pkg_check_modules(XRANDR REQUIRED xrandr)
    pkg_check_modules(XFIXES REQUIRED xfixes)
    target_include_directories(input_unified PRIVATE
        ${X11_INCLUDE_DIRS}
        ${XTST_INCLUDE_DIRS}
        ${XRANDR_INCLUDE_DIRS}
        ${XFIXES_INCLUDE_DIRS}
    )
    target_link_libraries(input_unified PRIVATE
        ${X11_LIBRARIES}
        ${XTST_LIBRARIES}
        ${XRANDR_LIBRARIES}
        ${XFIXES_LIBRARIES}
    )
endif()

if(WIN32)
    target_link_libraries(input_unified PRIVATE ws2_32 user32)
endif()

if(APPLE)
    # Required for CGEvent/CFRunLoop symbols used by macOS input capture/injection.
    target_link_libraries(input_unified PRIVATE
        "-framework ApplicationServices"
        "-framework CoreFoundation"
    )
endif()

# Copy .env to output directory (always runs during build)
add_custom_target(copy_env ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/.env" "${OUTPUT_DIR}/.env"
    COMMENT "Copying .env to build output"
    DEPENDS "${CMAKE_SOURCE_DIR}/.env"
)
add_dependencies(input_unified copy_env)
