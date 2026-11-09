cmake_minimum_required(VERSION 3.20)
project(market_data_replay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# e.g. -DMDR_SANITIZE=address,undefined  or  -DMDR_SANITIZE=thread
set(MDR_SANITIZE "" CACHE STRING "Sanitizers to enable (comma separated)")

set(MDR_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(MDR_SANITIZE)
  add_compile_options(-fsanitize=${MDR_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
  add_link_options(-fsanitize=${MDR_SANITIZE})
endif()

add_library(mdr src/generator.cpp src/files.cpp)
target_include_directories(mdr PUBLIC include)
target_compile_options(mdr PRIVATE ${MDR_WARNINGS})

foreach(tool mdr_gen:gen_main mdr_replay:replay_main mdr_bench:bench mdr_fuzz:fuzz)
  string(REPLACE ":" ";" parts ${tool})
  list(GET parts 0 name)
  list(GET parts 1 src)
  add_executable(${name} src/${src}.cpp)
  target_link_libraries(${name} PRIVATE mdr)
  target_compile_options(${name} PRIVATE ${MDR_WARNINGS})
endforeach()

add_executable(mdr_tests tests/tests.cpp)
target_link_libraries(mdr_tests PRIVATE mdr)
target_compile_options(mdr_tests PRIVATE ${MDR_WARNINGS})

enable_testing()
add_test(NAME unit COMMAND mdr_tests)
add_test(NAME fuzz_smoke COMMAND mdr_fuzz --seeds 3 --events 20000 --sessions 4 --session-messages 5000)
