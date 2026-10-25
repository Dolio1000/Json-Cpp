#ifndef LENJSON_JSON_EXPORT_H
#define LENJSON_JSON_EXPORT_H

// =============================================================================
// json_export.h —— 动态库的符号导入 / 导出宏
// -----------------------------------------------------------------------------
// 同一份头文件要同时服务于三种场景：
//   1. 编译静态库：所有符号都是普通的全局符号，宏展开为空；
//   2. 编译动态库（Windows 的 DLL）：符号必须标记 __declspec(dllexport)，
//      才会被写进 DLL 的导出表，否则链接时报“无法解析的外部符号”；
//   3. 使用动态库：符号标记 __declspec(dllimport)，编译器据此生成
//      “经由导入表跳转”的调用，链接的是导入库（import library）。
//
// 这三种状态靠两个宏区分，它们都由 CMake 定义：
//   LENJSON_SHARED  —— 在“动态库目标”上以 PUBLIC 方式定义，
//                      因此库自身和使用者的编译命令里都有它；
//   LENJSON_EXPORTS —— 只在“编译动态库自身”时以 PRIVATE 方式定义。
//
// 组合结果：
//   有 LENJSON_SHARED + 有 LENJSON_EXPORTS -> dllexport（我们在编 DLL）
//   有 LENJSON_SHARED + 无 LENJSON_EXPORTS -> dllimport（别人在用 DLL）
//   没有 LENJSON_SHARED                     -> 什么都不加（静态库或直接编译源码）
// =============================================================================

#if defined(_WIN32) || defined(__CYGWIN__)

    #if defined(LENJSON_SHARED)
        #if defined(LENJSON_EXPORTS)
            #define LENJSON_API __declspec(dllexport)
        #else
            #define LENJSON_API __declspec(dllimport)
        #endif
    #else
        #define LENJSON_API
    #endif

#else

    // Linux/macOS 下默认导出全部符号；若将来显式开启 -fvisibility=hidden，
    // 这行标注就负责把公开接口重新“点亮”。
    #if defined(LENJSON_SHARED) && defined(__GNUC__)
        #define LENJSON_API __attribute__((visibility("default")))
    #else
        #define LENJSON_API
    #endif

#endif

// -----------------------------------------------------------------------------
// MSVC 的两条“噪音”告警（只在动态库模式下出现）：
//   C4251：导出的类里有未导出的成员类型（这里是 std::string / std::variant 等标准库类型）；
//   C4275：导出的类继承了未导出的基类（这里是 std::runtime_error）。
// 它们想提醒的风险是：客户端与 DLL 若使用不兼容的标准库实现，跨边界传递 STL 类型会出错。
// 本库的前提是两边用同一个编译器与同一套运行库（MSVC 默认 /MD 即满足），
// 因此这两条告警在此场景下是安全的，统一关闭，避免淹没真正有价值的信息。
// 注：静态库（没有 LENJSON_SHARED）不做任何屏蔽，保持 /W4 的完整检查力度。
// -----------------------------------------------------------------------------
#if defined(_MSC_VER) && defined(LENJSON_SHARED)
    #pragma warning(disable : 4251 4275)
#endif

#endif // LENJSON_JSON_EXPORT_H
