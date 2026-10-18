cmake_minimum_required(VERSION 3.20)
project(quantactive_hft_sim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# -DQC_SANITIZE=address | thread | none
set(QC_SANITIZE "none" CACHE STRING "Sanitizer to enable: none, address, thread")

add_library(qc_options INTERFACE)
target_compile_options(qc_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(QC_SANITIZE STREQUAL "address")
  target_compile_options(qc_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  target_link_options(qc_options INTERFACE -fsanitize=address,undefined)
elseif(QC_SANITIZE STREQUAL "thread")
  target_compile_options(qc_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(qc_options INTERFACE -fsanitize=thread)
endif()

find_package(Threads REQUIRED)

file(GLOB QC_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM QC_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
add_library(qc STATIC ${QC_SOURCES})
target_include_directories(qc PUBLIC include)
target_link_libraries(qc PUBLIC qc_options Threads::Threads)

add_executable(qc_cli src/main.cpp)
set_target_properties(qc_cli PROPERTIES OUTPUT_NAME qc)
target_link_libraries(qc_cli PRIVATE qc)

add_executable(qc_bench bench/bench_main.cpp)
target_link_libraries(qc_bench PRIVATE qc)

enable_testing()
file(GLOB QC_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
add_executable(qc_tests ${QC_TEST_SOURCES})
target_link_libraries(qc_tests PRIVATE qc)
target_include_directories(qc_tests PRIVATE tests)
target_compile_definitions(qc_tests PRIVATE QC_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME unit COMMAND qc_tests)
