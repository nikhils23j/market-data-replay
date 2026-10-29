cmake_minimum_required(VERSION 3.20)
project(market_data_replay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(mdr src/book.cpp src/io.cpp src/replay.cpp)
target_include_directories(mdr PUBLIC include)
target_compile_options(mdr PRIVATE -Wall -Wextra -Wpedantic)

add_executable(mdr_replay src/main.cpp)
target_link_libraries(mdr_replay PRIVATE mdr)
target_compile_options(mdr_replay PRIVATE -Wall -Wextra -Wpedantic)

add_executable(mdr_bench src/bench.cpp)
target_link_libraries(mdr_bench PRIVATE mdr)
target_compile_options(mdr_bench PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_executable(mdr_tests tests/tests.cpp)
target_link_libraries(mdr_tests PRIVATE mdr)
target_compile_options(mdr_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME mdr_tests COMMAND mdr_tests)
