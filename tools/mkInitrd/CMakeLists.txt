cmake_minimum_required(VERSION 3.20)
project(mkInitrd CXX)

set(CMAKE_CXX_STANDARD 17)

add_executable(mkInitrd main.cpp)

target_compile_options(mkInitrd PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:-Oz>
    $<$<CXX_COMPILER_ID:GNU>:-Os>
)

target_link_options(mkInitrd PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:GNU>>:-s>
)
