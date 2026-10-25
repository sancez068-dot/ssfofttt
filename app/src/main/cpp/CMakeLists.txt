cmake_minimum_required(VERSION 3.22.1)
project(km14runtime C)

# One C translation unit contains both aim-assist and spotted-flag radar.
# The output filename matches the native library referenced by the current panel APK.
add_library(logger SHARED km14_combined_v2_4.c)
set_target_properties(logger PROPERTIES OUTPUT_NAME "lol")

target_compile_options(logger PRIVATE
    -Wall -Wextra
    -fno-omit-frame-pointer
)

target_link_libraries(logger
    android
    log
    dl
    m
)
