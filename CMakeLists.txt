cmake_minimum_required(VERSION 3.10)
project(kvspace-c-freeze LANGUAGES NONE)

# 本仓已冻结：SHM 存储引擎并入 kvspace 仓。
#
# 实现位置：~/github.com/array2d/kvspace/src/shm/（kvspace.c、coord.c、内部头）
# 测试位置：~/github.com/array2d/kvspace/tests/shm/
# 构建入口：~/github.com/array2d/kvspace 的 CMake target `kvspace-c`
#           （产物仍为 libkvspace-c.so.1，dispatch 前端按此名 dlopen）
#
# 这里只留一条明确的失败信息，而不是让 cmake 报「没有 CMakeLists」那种
# 看不出原因的错——误配的人应当一眼看到「已冻结，去 kvspace 仓」。
message(FATAL_ERROR
    "kvspace-c 已冻结，实现见 kvspace/src/shm。"
    "构建请用 array2d/kvspace 仓的 kvspace-c target。")
