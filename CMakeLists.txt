cmake_minimum_required(VERSION 3.16)

project(portable_file_watcher_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_executable(portable_file_watcher_tests
    portable_file_watcher_tests.cpp
)

target_include_directories(portable_file_watcher_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

if (MSVC)
    target_compile_options(portable_file_watcher_tests PRIVATE /W4)
else()
    target_compile_options(portable_file_watcher_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

add_test(
    NAME portable_file_watcher_tests
    COMMAND $<TARGET_FILE:portable_file_watcher_tests>
)
