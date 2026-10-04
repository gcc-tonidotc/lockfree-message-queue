cmake_minimum_required(VERSION 3.20)
project(UltraLowLatencyLockFreeQueue VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optimization flags
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-O3 -march=native -Wall -Wextra -Wpedantic -pthread)
elseif (MSVC)
    add_compile_options(/O2 /arch:AVX2 /W4 /permissive-)
endif()

include_directories(include)

# Executables
add_executable(message_queue_demo src/main.cpp)
add_executable(test_spsc tests/test_spsc.cpp)
add_executable(test_mpmc tests/test_mpmc.cpp)
add_executable(false_sharing_bench benchmarks/false_sharing_bench.cpp)
add_executable(spsc_throughput benchmarks/spsc_throughput.cpp)
add_executable(spsc_latency_rtt benchmarks/spsc_latency_rtt.cpp)
add_executable(mpmc_bench benchmarks/mpmc_bench.cpp)

enable_testing()
add_test(NAME TestSPSC COMMAND test_spsc)
add_test(NAME TestMPMC COMMAND test_mpmc)
