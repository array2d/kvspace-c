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

# SONAME 版本：libkvspace-c.so.1（dispatch 前端按此 dlopen 后端）
set_target_properties(kvspace-c PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

# 无公共头：内部头在 src/（kvspace_shm.h、xvalue.h），权威 ABI 头在 kvspace 仓库。
target_link_libraries(kvspace-c
    PUBLIC blockmalloc slotsboxmalloc
)

install(TARGETS kvspace-c
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

option(KVSPACE_BUILD_TESTS "Build tests (ctest)" OFF)
if(KVSPACE_BUILD_TESTS)
    enable_testing()
    add_executable(test_shm_split tests/test_shm_split.c)
    target_include_directories(test_shm_split PRIVATE src)
    target_link_libraries(test_shm_split PRIVATE kvspace-c)
    add_test(NAME shm_split COMMAND test_shm_split)
endif()
