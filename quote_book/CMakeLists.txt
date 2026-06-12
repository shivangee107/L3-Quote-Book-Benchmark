cmake_minimum_required(VERSION 3.16)
project(l3_quote_book_benchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(l3_quote_book_benchmark
    src/l3_quote_book_benchmark.cpp
)

target_include_directories(l3_quote_book_benchmark PRIVATE include)
target_compile_options(l3_quote_book_benchmark PRIVATE -Wall -Wextra -Wpedantic)
