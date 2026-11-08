# haiku specific compile definitions

add_compile_definitions(SUNSHINE_PLATFORM="haiku")

# Haiku exposes BSD extensions (getifaddrs, pthread_setname_np, ...) behind _DEFAULT_SOURCE.
# A strict -std=c++23 build defines __STRICT_ANSI__, which would otherwise hide them.
add_compile_definitions(_DEFAULT_SOURCE)

# BSD compatibility headers (ifaddrs.h) live outside the default header search path on some
# Haiku releases, so add the directory explicitly when it exists.
find_path(HAIKU_BSD_HEADERS_DIR ifaddrs.h
        HINTS
            /boot/system/develop/headers/bsd
            /boot/system/develop/headers/compatibility/bsd)
if(HAIKU_BSD_HEADERS_DIR)
    include_directories(SYSTEM "${HAIKU_BSD_HEADERS_DIR}")
endif()

# Vulkan Video encoding (h264_vulkan / hevc_vulkan / av1_vulkan through FFmpeg).
# FFmpeg loads libvulkan at runtime, so machines without a Vulkan video driver fall back to
# software encoding automatically. No Vulkan library is linked into Sunshine itself.
if(NOT DEFINED SUNSHINE_ENABLE_VULKAN OR SUNSHINE_ENABLE_VULKAN)
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_BUILD_VULKAN=1)
    message(STATUS "Haiku: Vulkan Video encoder enabled (selected at runtime when the GPU driver supports it)")
endif()

list(APPEND PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/haiku/app_context.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/haiku/app_context.h"
        "${CMAKE_SOURCE_DIR}/src/platform/haiku/audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/haiku/audio_tap.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/haiku/audio_tap.h"
        "${CMAKE_SOURCE_DIR}/src/platform/haiku/display.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/haiku/input.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/haiku/mdns.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/haiku/mdns.h"
        "${CMAKE_SOURCE_DIR}/src/platform/haiku/misc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/haiku/publish.cpp")

# libbe: app_server (BScreen, BBitmap), libmedia: Media Kit audio tap,
# libnetwork: sockets and getifaddrs, libbsd: BSD helpers.
list(APPEND PLATFORM_LIBRARIES
        be
        media
        network
        bsd)
