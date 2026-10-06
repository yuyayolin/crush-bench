cmake_minimum_required(VERSION 3.10)
project(aurorabench_portable CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../entry/src/main/cpp)

add_executable(aurorabench_portable
    main_portable.cpp
    ../entry/src/main/cpp/bench_cpu.cpp)

if (MSVC)
    target_compile_options(aurorabench_portable PRIVATE /O2)
else()
    target_compile_options(aurorabench_portable PRIVATE -O2)
endif()

find_package(Threads REQUIRED)
target_link_libraries(aurorabench_portable PRIVATE Threads::Threads)
