# tutorial-c 统一 kvspace* ABI，链接 libkvspace（dispatch 前端，shm:// 后端）

add_executable(01_basic 01_basic.c)
target_link_libraries(01_basic kvspace)

add_executable(02_cpp 02_cpp.cpp)
target_link_libraries(02_cpp kvspace)

add_executable(05_integrity 05_integrity.c)
target_link_libraries(05_integrity kvspace)

add_executable(06_multiprocess 06_multiprocess.c)
target_link_libraries(06_multiprocess kvspace)
