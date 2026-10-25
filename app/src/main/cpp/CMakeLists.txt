cmake_minimum_required(VERSION 3.22.1)
project(km14runtime C)

add_library(logger SHARED
    km14_aimassist_v1.c
    km14_spotted_flag.c
)

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
