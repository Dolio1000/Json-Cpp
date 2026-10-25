# =============================================================================
# CMakeLists.txt —— 构建脚本（MSVC 与 GCC/Clang/MinGW 通用）
# -----------------------------------------------------------------------------
# 用法：
#   cmake -S . -B build
#   cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure   # 运行单元测试
#
# 产物：
#   动态库：lenjson.dll + 导入库（MSVC 为 lenjson.lib，MinGW 为 liblenjson.dll.a）
#   静态库：lenjson_static.lib（MinGW 为 liblenjson_static.a）
#
# ★ 关于符号可见性
#   本库的对外接口是纯 C，目标是让 g++ 编译的 DLL 能被 MSVC 安全调用。
#   为此动态库必须只导出 C 接口里的那几十个 C 符号，内部那套 C++ 实现
#   （lenjson::Value 等）一律留在库内部：
#     * Windows：靠 __declspec(dllexport)（头文件里的 LENJSON_API）逐个标注；
#       同时关掉 WINDOWS_EXPORT_ALL_SYMBOLS，防止 CMake 把所有符号都导出去。
#     * Linux / macOS：靠 -fvisibility=hidden 把内部符号藏起来，
#       只留 LENJSON_API 标注的 default 可见性。
#   内部 C++ 头文件放在 src/internal/，不对外发布。
#
# ★ 关于运行时
#   GCC/MinGW 的动态库默认加 -static（LENJSON_STATIC_RUNTIME），把 libstdc++ /
#   libgcc / libwinpthread 链进 DLL，这样 lenjson.dll 拷到只有 MSVC 的机器上也能跑。
#   另外 /utf-8 以 INTERFACE 方式导出给 MSVC 使用方：公开头文件是 UTF-8 中文注释，
#   不按 UTF-8 读会报 C1022。
# =============================================================================

cmake_minimum_required(VERSION 3.16)
# 同时启用 C：对外接口是纯 C 接口，需要一个真正的 C 编译单元来验证它
# （tests/test_lenjson_c.c 由 C 编译器编译，证明公开头文件没有 C++ 语法泄漏）。
project(lenjson VERSION 1.0.0 LANGUAGES CXX C)

# 统一使用 C++17：内部实现依赖 std::variant / std::string_view / if constexpr。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

# -----------------------------------------------------------------------------
# 开关
# -----------------------------------------------------------------------------
option(LENJSON_BUILD_SHARED   "构建动态链接库（Windows 下为 lenjson.dll）" ON)
option(LENJSON_BUILD_STATIC   "构建静态库（lenjson_static）" ON)
option(LENJSON_BUILD_EXAMPLES "构建示例程序" ON)
option(LENJSON_BUILD_TESTS    "构建单元测试" ON)
option(LENJSON_INSTALL        "生成安装规则与 CMake package 配置" OFF)
option(LENJSON_STATIC_RUNTIME "（GCC/MinGW）把 libstdc++ / libgcc 静态链接进动态库，产物可单独分发" ON)

if(NOT LENJSON_BUILD_SHARED AND NOT LENJSON_BUILD_STATIC)
    message(FATAL_ERROR "LENJSON_BUILD_SHARED 与 LENJSON_BUILD_STATIC 至少要打开一个")
endif()

# -----------------------------------------------------------------------------
# 源文件
# -----------------------------------------------------------------------------
# json_c_api.cpp 是唯一的对外导出面；其余文件是内部 C++ 实现。
set(LENJSON_SOURCES
    src/json_c_api.cpp
    src/json_value.cpp
    src/json_value_access.cpp
    src/json_parser.cpp
    src/json_writer.cpp
    src/json_utils_number.cpp
    src/json_utils_unicode.cpp
)

# 唯一对外发布的头文件。
set(LENJSON_PUBLIC_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lenjson/lenjson.h)

# -----------------------------------------------------------------------------
# 公共设置
# -----------------------------------------------------------------------------
function(lenjson_setup_target target)
    # 公开头文件目录：使用者只需 #include "lenjson/lenjson.h"。
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    # 告警选项用 PRIVATE：它们只对本库自己有意义，不该通过安装包传给使用者
    # （否则 MSVC 的使用方会收到 -Wall -Wextra 这种它不认识的参数，直接编译失败）。
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4)
    else()
        # GCC/Clang：打开常见告警，帮助发现潜在问题。
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()

    # 公开头文件里的中文注释是 UTF-8，MSVC 使用方必须以 UTF-8 读入，
    # 否则会按 GBK 解码并把注释末尾字节当成行继续符（报 C1022）。
    # 用生成器表达式写成“仅当使用方是 MSVC 且语言为 C/C++”时才加，
    # 这样安装包里的目标能给 MSVC 工程自动带上 /utf-8，对 GCC 工程又无影响。
    target_compile_options(${target}
        INTERFACE
            "$<$<COMPILE_LANG_AND_ID:C,MSVC>:/utf-8>"
            "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>"
    )
endfunction()

# -----------------------------------------------------------------------------
# 动态库
# -----------------------------------------------------------------------------
if(LENJSON_BUILD_SHARED)
    add_library(lenjson SHARED ${LENJSON_SOURCES})
    lenjson_setup_target(lenjson)

    # LENJSON_EXPORTS 只在“编译 DLL 自身”时定义 -> 头文件里展开成 dllexport。
    target_compile_definitions(lenjson PRIVATE LENJSON_EXPORTS)
    # LENJSON_SHARED 以 PUBLIC 传递到使用者 -> 使用方的头文件里展开成 dllimport。
    target_compile_definitions(lenjson PUBLIC LENJSON_SHARED)

    set_target_properties(lenjson PROPERTIES
        # 只导出显式标注的 C 符号，绝不把内部 C++ 符号一并导出去。
        WINDOWS_EXPORT_ALL_SYMBOLS OFF
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )

    if(MINGW)
        # MinGW 默认给 DLL 加 "lib" 前缀（liblenjson.dll）。这里去掉，
        # 让各工具链产出的 DLL 都叫 lenjson.dll——跨编译器使用时，
        # MSVC 侧生成的导入库与运行时要找的 DLL 名字才对得上。
        set_target_properties(lenjson PROPERTIES PREFIX "")
    endif()

    # GCC 运行时静态链接进 DLL 本身：否则 lenjson.dll 还依赖
    # libstdc++-6.dll / libgcc_s_seh-1.dll / libwinpthread-1.dll，
    # 而 MSVC 的机器上通常没有它们。加 -static 后只剩系统 DLL 依赖。
    # 只对动态库有意义（静态库的运行时由调用方的链接环节补充）。
    if(NOT MSVC AND LENJSON_STATIC_RUNTIME)
        target_link_options(lenjson PRIVATE -static)
    endif()
endif()

# -----------------------------------------------------------------------------
# 静态库
# -----------------------------------------------------------------------------
if(LENJSON_BUILD_STATIC)
    # 名字带 _static 后缀：MSVC 下导入库叫 lenjson.lib，静态库若同名会互相覆盖。
    add_library(lenjson_static STATIC ${LENJSON_SOURCES})
    lenjson_setup_target(lenjson_static)
    # 静态库不需要导出宏：代码是直接“搬进”最终程序里的。
    set_target_properties(lenjson_static PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
endif()

# -----------------------------------------------------------------------------
# 测试与示例的目标选择
# -----------------------------------------------------------------------------
# C 接口测试优先链接动态库：这样它同时充当“DLL 能否正常加载并调用”的验证。
if(TARGET lenjson)
    set(LENJSON_C_API_TARGET lenjson)
else()
    set(LENJSON_C_API_TARGET lenjson_static)
endif()

if(LENJSON_BUILD_EXAMPLES)
    add_executable(lenjson_demo examples/demo.cpp)
    target_link_libraries(lenjson_demo PRIVATE ${LENJSON_C_API_TARGET})
endif()

if(LENJSON_BUILD_TESTS)
    enable_testing()

    # C 接口测试：纯 C 源文件，只用公开头文件，链接动态库。
    add_executable(lenjson_c_tests tests/test_lenjson_c.c)
    target_link_libraries(lenjson_c_tests PRIVATE ${LENJSON_C_API_TARGET})
    add_test(NAME lenjson_c_tests COMMAND lenjson_c_tests)

    # 核心实现测试：直接测内部 C++ 类，因此链接静态库——
    # 动态库刻意不导出 C++ 符号（那正是跨编译器安全的代价）。
    if(TARGET lenjson_static)
        add_executable(lenjson_core_tests tests/test_lenjson.cpp)
        target_include_directories(lenjson_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(lenjson_core_tests PRIVATE lenjson_static)
        add_test(NAME lenjson_core_tests COMMAND lenjson_core_tests)
    else()
        message(STATUS "未构建静态库，跳过内部实现测试（lenjson_core_tests）")
    endif()
endif()

# -----------------------------------------------------------------------------
# MSVC 导入库定义文件
# -----------------------------------------------------------------------------
# MSVC 的 link.exe 无法直接使用 MinGW 生成的 .dll.a 导入库，
# 常规做法是用一份 .def 生成 MSVC 自己的导入库：
#     lib /def:lenjson.def /machine:x64 /out:lenjson.lib
# .def 里的 LIBRARY 一行写明运行时该去找哪个 DLL。
# 这里直接从公开头文件里提取导出函数名生成 .def，
# 保证它不会和头文件脱节（手工维护清单迟早会漏）。
set(LENJSON_DEF_CONTENT "LIBRARY lenjson.dll\nEXPORTS\n")
file(STRINGS ${LENJSON_PUBLIC_HEADER} LENJSON_API_DECLARATIONS REGEX "^LENJSON_API")
foreach(_decl IN LISTS LENJSON_API_DECLARATIONS)
    if(_decl MATCHES "LENJSON_API[^;]*[^A-Za-z0-9_]([A-Za-z_][A-Za-z0-9_]*)[ ]*\\(")
        string(APPEND LENJSON_DEF_CONTENT "    ${CMAKE_MATCH_1}\n")
    endif()
endforeach()
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lenjson.def CONTENT "${LENJSON_DEF_CONTENT}")

# -----------------------------------------------------------------------------
# 安装与导出
# -----------------------------------------------------------------------------
if(LENJSON_INSTALL)
    include(CMakePackageConfigHelpers)

    set(LENJSON_INSTALL_TARGETS)
    if(TARGET lenjson)
        list(APPEND LENJSON_INSTALL_TARGETS lenjson)
    endif()
    if(TARGET lenjson_static)
        list(APPEND LENJSON_INSTALL_TARGETS lenjson_static)
    endif()

    install(TARGETS ${LENJSON_INSTALL_TARGETS}
        EXPORT lenjsonTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(FILES ${LENJSON_PUBLIC_HEADER} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lenjson)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lenjson.def
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lenjson)

    install(EXPORT lenjsonTargets
        FILE lenjsonTargets.cmake
        NAMESPACE lenjson::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lenjson
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/lenjsonConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/lenjsonConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lenjson
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/lenjsonConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/lenjsonConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/lenjsonConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lenjson
    )
endif()
