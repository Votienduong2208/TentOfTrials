cmake_minimum_required(VERSION 3.16)

project(frailbox C)

add_executable(frailbox
    main.c
    src/arena.c
    src/buddy.c
    src/logger.c
    src/sandbox.c
)

target_include_directories(frailbox PRIVATE include)
target_compile_definitions(frailbox PRIVATE _GNU_SOURCE USE_BUDDY_ALLOCATOR)
target_compile_options(frailbox PRIVATE -Wall -Wextra -Werror)
