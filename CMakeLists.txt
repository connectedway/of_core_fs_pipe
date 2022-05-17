cmake_minimum_required(VERSION 3.20.0)
project(of_core_fs_pipe VERSION 1.0.1 DESCRIPTION "OpenFiles Pipe Handler")

include_directories(
        ${of_core_BINARY_DIR}
        ${of_core_SOURCE_DIR}/include
        include
)

set(SRCS
        src/fs_pipe.c
        )

add_library(of_core_fs_pipe OBJECT ${SRCS})
set_property(TARGET of_core_fs_pipe PROPERTY POSITION_INDEPENDENT_CODE ON)

