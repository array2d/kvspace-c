cmake_minimum_required(VERSION 3.10)
project(kvspace-c VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(GNUInstallDirs)

add_library(kvspace-c SHARED
    src/xvalue.c
    src/kvspace.c
    src/durable_abi.c
)

target_include_directories(kvspace-c
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(kvspace-c
    PUBLIC blockmalloc slotsboxmalloc
)

add_subdirectory(tutorial)

install(TARGETS kvspace-c
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
