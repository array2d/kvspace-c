cmake_minimum_required(VERSION 3.10)
project(kvspace-c VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(GNUInstallDirs)

# 上游头路径：`xvalue_head.h` **不再 vendor**（用户裁决：只容许 dep kvspace 的头文件）。
# 相对仓库布局 `<repo>/../kvspace/include`，与 kvlang/layout 同做法；没有副本可漂移。
# CACHE：允许 `-DKVSPACE_UPSTREAM_INCLUDE_DIRECTORY=…` 覆盖（隔离 CI、导出树）。
# 不带 CACHE 时 `set()` 会无条件盖掉命令行传入的值——那样下面那条配置期断言
# 永远检查默认路径，反向对照跑不出来（实测：用错目录配置仍然通过）。
set(KVSPACE_UPSTREAM_INCLUDE_DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}/../kvspace/include"
    CACHE PATH "upstream kvspace include dir (used instead of a vendored copy)")

# 配置期断言：路径指向的必须是**新版** xvalue_head。
#
# 替代原 `ci/check_xvalue_head_sync.sh` 的判据。副本删除后，「副本与上游漂移」
# 不再可能发生；新的风险变成「这个路径指错了地方」——指到旧克隆、旧安装前缀、
# 或者一个空的 include 目录。那会让 kvspace-c 对着错误的头编译：能编过、能跑，
# 但按错误布局解释跨进程字节。所以判据从「两份一致」改成「这一份是对的」。
if(NOT EXISTS "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}/kvspace/xvalue_head.h")
    message(FATAL_ERROR
        "kvspace-c requires upstream kvspace headers at "
        "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}/kvspace/xvalue_head.h "
        "(no vendored copy: the header is used from the upstream clone directly). "
        "Clone array2d/kvspace as a sibling of kvspace-c, or set "
        "KVSPACE_UPSTREAM_INCLUDE_DIRECTORY.")
endif()
file(READ "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}/kvspace/xvalue_head.h"
     KVSPACE_UPSTREAM_HEADER_TEXT)
# 判据取一个**只在新布局存在**的常量：旧的 storetype 布局里没有它，所以命中
# 就等于「这份头是新的」。不取 `KVSPACE_XVALUE_HEAD_LEN`（两代都有，区分不了）。
# 上游 xvalue_head 的**实现**同样直接编译，不留副本。
#
# 为什么这条要单独断言：头缺失会在编译期炸得很明显，而 `.c` 缺失会变成
# 「链接期找不到 kvspaceXvalueHeadNew*」——错误信息指向调用方，不指向路径。
set(KVSPACE_UPSTREAM_IMPLEMENTATION
    "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}/../src/xvalue_head.c")
if(NOT EXISTS "${KVSPACE_UPSTREAM_IMPLEMENTATION}")
    message(FATAL_ERROR
        "kvspace-c compiles the upstream xvalue_head implementation directly "
        "(no vendored copy). Expected it at ${KVSPACE_UPSTREAM_IMPLEMENTATION}. "
        "Clone array2d/kvspace as a sibling of kvspace-c, or set "
        "KVSPACE_UPSTREAM_INCLUDE_DIRECTORY to its include/ directory.")
endif()

if(NOT KVSPACE_UPSTREAM_HEADER_TEXT MATCHES "padding\\[6\\]")
    message(FATAL_ERROR
        "kvspace-c requires the NEW xvalue_head layout (16-byte head with "
        "`padding[6]` at @10, no `reserved` / `layout` / tag 2). The header at "
        "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}/kvspace/xvalue_head.h does not "
        "declare `padding[6]`, so it is an older layout. "
        "Update the upstream kvspace clone.")
endif()

add_library(kvspace-c SHARED
    src/coord.c
    "${KVSPACE_UPSTREAM_IMPLEMENTATION}"
    src/kvspace.c
    src/durable_abi.c
)
target_include_directories(kvspace-c PUBLIC
    "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}")

# SONAME 版本：libkvspace-c.so.1（dispatch 前端按此 dlopen 后端）
set_target_properties(kvspace-c PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

# Internal headers are in src/; the public ABI header comes from upstream kvspace
# (no vendored copy — see KVSPACE_UPSTREAM_INCLUDE_DIRECTORY above).
# 两个依赖库走 header-only（src/kvspace.c 里 #define *_IMPLEMENTATION），故不链接 .so；
# 头的搜索路径由系统 include（/usr/include/blockmalloc、/usr/include/slotsboxmalloc）提供。

# 后端库固定装到 dispatch 前端 dlopen 的目录 <prefix>/lib/kvspace。
# 默认 prefix=/usr（对齐 ci/deps.sh 的 /usr/lib/kvspace），而非 /usr/local。
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "" FORCE)
endif()
install(TARGETS kvspace-c
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/kvspace
)

option(KVSPACE_BUILD_TESTS "Build tests (ctest)" OFF)
if(KVSPACE_BUILD_TESTS)
    enable_testing()
    # 测试构建启用内部观测/注入钩子（生产库不含）。
    target_compile_definitions(kvspace-c PRIVATE P2_TEST_HOOKS)
    add_executable(test_shm_split tests/test_shm_split.c)
    target_include_directories(test_shm_split PRIVATE src "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}")
    target_link_libraries(test_shm_split PRIVATE kvspace-c)
    add_test(NAME shm_split COMMAND test_shm_split)

    add_executable(test_shm_resize tests/test_shm_resize.c)
    target_include_directories(test_shm_resize PRIVATE src "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}")
    target_link_libraries(test_shm_resize PRIVATE kvspace-c)
    add_test(NAME shm_resize COMMAND test_shm_resize)

    add_executable(test_art_scan tests/test_art_scan.c)
    target_include_directories(test_art_scan PRIVATE src "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}")
    target_link_libraries(test_art_scan PRIVATE kvspace-c)
    add_test(NAME art_scan COMMAND test_art_scan)

    add_executable(test_handle_cache tests/test_handle_cache.c)
    target_include_directories(test_handle_cache PRIVATE src "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}")
    target_link_libraries(test_handle_cache PRIVATE kvspace-c)
    add_test(NAME handle_cache COMMAND test_handle_cache)

    add_executable(test_xvalue_head tests/test_xvalue_head.c
    "${KVSPACE_UPSTREAM_IMPLEMENTATION}")
    target_include_directories(test_xvalue_head PRIVATE src "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}")
    add_test(NAME xvalue_head COMMAND test_xvalue_head)

    # wire 金标准：把同一批操作的字节打印出来，供三路（前端 / C 后端 / durable）
    # 逐字节对拍。用例集合与输出格式以 kvspace/tests/xvalue_wire_golden.c 为准。
    add_executable(xvalue_wire_golden tests/xvalue_wire_golden.c
    "${KVSPACE_UPSTREAM_IMPLEMENTATION}")
    target_include_directories(xvalue_wire_golden PRIVATE src "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}")
    add_test(NAME xvalue_wire_golden COMMAND xvalue_wire_golden)
    add_executable(test_codec_abi tests/test_codec_abi.c)
    target_include_directories(test_codec_abi PRIVATE src "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}")
    target_link_libraries(test_codec_abi PRIVATE kvspace-c)
    add_test(NAME codec_abi COMMAND test_codec_abi)
    add_executable(test_abi_contract tests/test_abi_contract.c)
    target_include_directories(test_abi_contract PRIVATE src "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}")
    target_link_libraries(test_abi_contract PRIVATE kvspace-c)
    add_test(NAME abi_contract COMMAND test_abi_contract)
    add_executable(test_inplace_body_only tests/test_inplace_body_only.c)
    target_include_directories(test_inplace_body_only PRIVATE src "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}")
    target_link_libraries(test_inplace_body_only PRIVATE kvspace-c)
    add_test(NAME inplace_body_only COMMAND test_inplace_body_only)
    add_executable(test_set_part_fastpath tests/test_set_part_fastpath.c)
    target_include_directories(test_set_part_fastpath PRIVATE src "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}")
    target_link_libraries(test_set_part_fastpath PRIVATE kvspace-c)
    add_test(NAME set_part_fastpath COMMAND test_set_part_fastpath)

    add_executable(test_new_wire_shm tests/test_new_wire_shm.c)
    target_include_directories(test_new_wire_shm PRIVATE src "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}")
    target_link_libraries(test_new_wire_shm PRIVATE kvspace-c)
    add_test(NAME new_wire_shm COMMAND test_new_wire_shm)

    add_executable(test_slot_view tests/test_slot_view.c)
    target_include_directories(test_slot_view PRIVATE src "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}")
    target_link_libraries(test_slot_view PRIVATE kvspace-c)
    add_test(NAME slot_view COMMAND test_slot_view)

    add_executable(test_art_alignment tests/test_art_alignment.c)
    target_include_directories(test_art_alignment PRIVATE src "${KVSPACE_UPSTREAM_INCLUDE_DIRECTORY}")
    target_compile_definitions(test_art_alignment PRIVATE P2_TEST_HOOKS)
    target_link_libraries(test_art_alignment PRIVATE kvspace-c)
    add_test(NAME art_alignment COMMAND test_art_alignment)

    # xvalue_head 的「副本与上游逐字节一致」检查已删除：**本仓不再有副本**
    # （头与实现都直接取自上游，见顶部 KVSPACE_UPSTREAM_* 两个变量）。
    # 用户裁决「不允许 vendor」，kvspace-c 直接 `#include <kvspace/xvalue_head.h>`
    # 依赖上游那个文件本身（见顶部 KVSPACE_UPSTREAM_INCLUDE_DIRECTORY）——
    # 没有副本可以漂移，也就不需要「证明副本没漂移」的检查。
    # 删除的检查脚本保留在同名 .disabled 里，只为留下判据的来龙去脉，不参与构建。
endif()
