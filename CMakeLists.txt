cmake_minimum_required(VERSION 3.28)

project(byte-code-compiler C)

set(CMAKE_C_STANDARD 11)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(byte-code-compiler main.c memory.c chunk.c value.c debug.c)
