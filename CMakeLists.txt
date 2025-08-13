cmake_minimum_required(VERSION 3.10)
project(rtsp_to_mse_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find necessary libraries
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
# For uWebSockets, we might need to specify paths if not in standard location
# For now, let's assume it can be found. We will add more robust finding logic later.

add_executable(rtsp_server src/main.cpp)

# Link libraries
target_include_directories(rtsp_server PUBLIC
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)

# uWebSockets requires linking against uSockets, and potentially zlib and libuv
target_link_libraries(rtsp_server PRIVATE
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    uSockets
    z
    uv
)
