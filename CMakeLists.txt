cmake_minimum_required(VERSION 3.28)

project(byte-code-compiler C)

set(CMAKE_C_STANDARD 11)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(clox main.c memory.c chunk.c value.c debug.c vm.c compiler.c scanner.c)
