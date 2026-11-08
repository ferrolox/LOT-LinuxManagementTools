cmake_minimum_required(VERSION 3.16)
project(TWMailer LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

add_executable(twmailer-server twmailer-server.c)
target_compile_options(twmailer-server PRIVATE -Wall -Wextra -Wpedantic)