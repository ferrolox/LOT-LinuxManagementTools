cmake_minimum_required(VERSION 4.3)
project(Projects C)

set(CMAKE_C_STANDARD 11)

include_directories(.)
include_directories(MyFind)

add_executable(Projects
        MyFind/main.c
        MyFind/main.c
        MyFind/main.c)
