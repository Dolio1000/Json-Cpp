# =============================================================================
# CMakeLists.txt —— 构建脚本（MSVC 与 GCC/Clang 通用）
# -----------------------------------------------------------------------------
# 用法：
#   cmake -S . -B build
#   cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure   # 运行单元测试
#
# 产物：同一份源码构建出两种库
#   动态库：lenjson.dll + 导入库（MSVC 为 lenjson.lib，MinGW 为 liblenjson.dll.a）
#   静态库：lenjson_static.lib（MinGW 为 liblenjson_static.a）
# =============================================================================

cmake_minimum_required(VERSION 3.16)
project(lenjson LANGUAGES CXX)

# 统一使用 C++17：本库依赖 std::variant / std::string_view / if constexpr。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -----------------------------------------------------------------------------
# 库的源文件：按功能模块拆分的 6 个实现文件（每个都控制在 600 行以内）。
# 动态库与静态库共用同一份源码。
# -----------------------------------------------------------------------------
set(LENJSON_SOURCES
    src/json_value.cpp
    src/json_value_access.cpp
    src/json_parser.cpp
    src/json_writer.cpp
    src/json_utils_number.cpp
    src/json_utils_unicode.cpp
)

# 两个开关默认都打开：一次配置即可同时得到动态库与静态库。
option(LENJSON_BUILD_SHARED "构建动态链接库（Windows 下为 lenjson.dll）" ON)
option(LENJSON_BUILD_STATIC "构建静态库（lenjson_static）" ON)

if(NOT LENJSON_BUILD_SHARED AND NOT LENJSON_BUILD_STATIC)
    message(FATAL_ERROR "LENJSON_BUILD_SHARED 与 LENJSON_BUILD_STATIC 至少要打开一个")
endif()

# -----------------------------------------------------------------------------
# 公共设置：包含目录 + 编译选项。抽成函数供两种库共用，避免重复。
# -----------------------------------------------------------------------------
function(lenjson_setup_target target)
    # 头文件目录对外公开，使用者只需 #include "lenjson/lenjson.h"。
    target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

    if(MSVC)
        # /utf-8：让 MSVC 把源码当作 UTF-8 读入，并把执行字符集也设为 UTF-8，
        #         否则源码里的中文注释/中文字符串在非 UTF-8 代码页下会出问题。
        target_compile_options(${target} PUBLIC /utf-8 /W4)
    else()
        # GCC/Clang：打开常见告警，帮助发现潜在问题。
        target_compile_options(${target} PUBLIC -Wall -Wextra)
    endif()
endfunction()

# -----------------------------------------------------------------------------
# 动态库：Windows 下产出 lenjson.dll 及其导入库
# -----------------------------------------------------------------------------
if(LENJSON_BUILD_SHARED)
    add_library(lenjson SHARED ${LENJSON_SOURCES})
    lenjson_setup_target(lenjson)

    # LENJSON_EXPORTS 只在“编译 DLL 自身”时定义 -> json_export.h 展开成 dllexport。
    target_compile_definitions(lenjson PRIVATE LENJSON_EXPORTS)
    # LENJSON_SHARED 用 PUBLIC 传递到使用者 -> 使用方的头文件里展开成 dllimport。
    target_compile_definitions(lenjson PUBLIC LENJSON_SHARED)
endif()

# -----------------------------------------------------------------------------
# 静态库
# -----------------------------------------------------------------------------
if(LENJSON_BUILD_STATIC)
    # 名字带 _static 后缀：MSVC 下导入库叫 lenjson.lib，静态库若同名会互相覆盖。
    add_library(lenjson_static STATIC ${LENJSON_SOURCES})
    lenjson_setup_target(lenjson_static)
    # 静态库不需要导出宏：代码是直接“搬进”最终程序里的。
endif()

# -----------------------------------------------------------------------------
# 示例程序与单元测试
# -----------------------------------------------------------------------------
# 优先链接动态库：这样它们同时充当“DLL 能否正常加载并调用”的验证。
if(TARGET lenjson)
    set(LENJSON_LINK_TARGET lenjson)
else()
    set(LENJSON_LINK_TARGET lenjson_static)
endif()

add_executable(lenjson_demo examples/demo.cpp)
target_link_libraries(lenjson_demo PRIVATE ${LENJSON_LINK_TARGET})

enable_testing()
add_executable(lenjson_tests tests/test_lenjson.cpp)
target_link_libraries(lenjson_tests PRIVATE ${LENJSON_LINK_TARGET})
add_test(NAME lenjson_tests COMMAND lenjson_tests)
