#ifndef LENJSON_LENJSON_H
#define LENJSON_LENJSON_H

// =============================================================================
// lenjson.h —— 对外公开的 C 接口（本库唯一的公开头文件）
// -----------------------------------------------------------------------------
// 为什么对外只用 C 接口？
//   C++ 的 ABI 没有统一标准。两个不同的编译器（MSVC vs MinGW/g++ vs clang）
//   对同一个类的“内存布局、名字修饰、异常处理、STL 容器实现”都可能不同。
//   只要有一个 C++ 对象（哪怕只是 std::string）跨过 DLL 边界，
//   就会出现“这边分配、那边释放”的堆损坏，症状是随机崩溃而不是编译错误。
//
//   C 接口则没有这个问题：
//     * 函数名不修饰（C++ 有 name mangling，C 没有）；
//     * 参数与返回值只用固定宽度整数、指针和定长 POD 结构体；
//     * 字符串缓冲区一律由本库分配、也由本库释放；
//     * 异常在库内部被捕获，转换成状态码返回。
//   于是 g++ 编译出的 DLL 可以被 MSVC 程序安全调用。
//
// 使用方式：
//   C   ：#include "lenjson/lenjson.h"
//   C++ ：#include "lenjson/lenjson.h"      （同样是 C 接口，可直接用）
//
// 链接动态库时需要定义 LENJSON_SHARED（CMake 的 lenjson::lenjson 目标会自动带上）；
// 链接静态库时不需要定义任何宏。
// =============================================================================

#include <stddef.h>
#include <stdint.h>

// -----------------------------------------------------------------------------
// 符号导入 / 导出宏
// -----------------------------------------------------------------------------
// 三种场景：
//   1. 编译静态库或直接编译源码：宏展开为空，符号就是普通的全局符号；
//   2. 编译动态库自身：展开为 __declspec(dllexport)，符号才会进导出表；
//   3. 使用动态库：展开为 __declspec(dllimport)，编译器改走导入表跳转。
// LENJSON_SHARED 由 CMake 以 PUBLIC 方式定义在使用者的编译命令里，
// LENJSON_EXPORTS 只在编译动态库自身时以 PRIVATE 方式定义。
// -----------------------------------------------------------------------------
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

    // Linux / macOS：默认导出全部符号；开启 -fvisibility=hidden 后，
    // 这行标注负责把公开接口重新“点亮”，同时把内部 C++ 符号藏起来。
    #if defined(LENJSON_SHARED) && defined(__GNUC__)
        #define LENJSON_API __attribute__((visibility("default")))
    #else
        #define LENJSON_API
    #endif

#endif

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// 基础类型
// -----------------------------------------------------------------------------

// JSON 值的类型。整数与浮点分开，因为二者的取值与输出规则不同。
typedef enum lenjson_type {
    LENJSON_TYPE_NULL    = 0,
    LENJSON_TYPE_BOOLEAN = 1,
    LENJSON_TYPE_INTEGER = 2,
    LENJSON_TYPE_REAL    = 3,
    LENJSON_TYPE_STRING  = 4,
    LENJSON_TYPE_ARRAY   = 5,
    LENJSON_TYPE_OBJECT  = 6
} lenjson_type;

// 函数返回的状态码。LENJSON_OK 表示成功，其余表示失败原因。
// 需要更详细的说明时，调用 lenjson_last_error_message() 取文本，
// 需要出错位置时调用 lenjson_last_error_info() 取行 / 列 / 字节下标。
typedef enum lenjson_status {
    LENJSON_OK                     = 0,
    LENJSON_ERROR_INVALID_ARGUMENT = 1,   // 传入了非法参数（空指针等）
    LENJSON_ERROR_PARSE             = 2,   // JSON 文本格式错误
    LENJSON_ERROR_TYPE              = 3,   // 类型不匹配
    LENJSON_ERROR_OUT_OF_BOUNDS     = 4,   // 下标越界或键不存在
    LENJSON_ERROR_MEMORY            = 5,   // 内存分配失败
    LENJSON_ERROR_INTERNAL          = 6    // 其它内部错误
} lenjson_status;

// 不透明句柄。指向一个 JSON 值节点；具体结构只在库内部可见。
// 调用方永远不要解引用它，只用本文件提供的函数操作。
typedef struct lenjson_value lenjson_value;

// 解析选项。用 lenjson_parse_options_default() 取默认值。
typedef struct lenjson_parse_options {
    // 最大嵌套深度。递归下降会随嵌套加深而消耗栈，恶意构造的超深 JSON
    // 可能把栈撑爆。设上限可把崩溃变成一个可查的状态码。
    // 传 0 表示使用默认值（512）。
    size_t max_depth;

    // 非 0 表示容忍 // 与 /* */ 注释（严格 JSON 不允许，但配置文件常用）。
    int allow_comments;
} lenjson_parse_options;

// 序列化选项。用 lenjson_dump_options_default() 取默认值。
typedef struct lenjson_dump_options {
    // 缩进宽度：负数表示紧凑输出（无空格无换行）；
    // >= 0 表示美化输出，每一层缩进 indent 个 indent_char 个字符。
    int  indent;

    // 缩进字符，默认是空格。
    char indent_char;

    // 非 0 表示把所有非 ASCII 字符转义成 \uXXXX，产出纯 ASCII 文本。
    int  ensure_ascii;
} lenjson_dump_options;

// 出错位置。line / column 从 1 起算，position 是从输入开头算起的字节下标。
typedef struct lenjson_error_info {
    size_t line;
    size_t column;
    size_t position;
} lenjson_error_info;

// 长度参数的特殊值：表示按 C 字符串处理（用 strlen 计算长度）。
#define LENJSON_NUL_TERMINATED ((size_t)-1)

// -----------------------------------------------------------------------------
// 解析与序列化
// -----------------------------------------------------------------------------

// 解析 JSON 文本。成功返回新的根句柄，失败返回 NULL。
//   text    ：JSON 文本，UTF-8 编码
//   length  ：字节数；传 LENJSON_NUL_TERMINATED 表示按 strlen 计算
//   status  ：可选的出参，传 NULL 表示不关心（失败原因仍可从 last-error 取）
// 返回的根句柄必须用 lenjson_value_free 释放。
LENJSON_API lenjson_value* lenjson_parse(const char* text, size_t length, lenjson_status* status);

// 带选项的解析。options 传 NULL 表示使用默认选项。
LENJSON_API lenjson_value* lenjson_parse_ex(const char* text, size_t length,
                                             const lenjson_parse_options* options,
                                             lenjson_status* status);

// 序列化为 JSON 文本。成功返回新分配的 C 字符串（以 '\0' 结尾），
// 失败返回 NULL。结果必须用 lenjson_free_string 释放——
// 分配与释放在同一个库内部完成，调用方不要自己 free。
//   options  ：可选的出参，传 NULL 表示使用默认选项（紧凑输出）
//   out_length：可选的出参，接收字节数（JSON 字符串里可能有 \u0000，
//               所以文本长度不一定等于 strlen）
LENJSON_API char* lenjson_dump(const lenjson_value* value, const lenjson_dump_options* options,
                               size_t* out_length);

// 释放 lenjson_dump / lenjson_value_get_string_copy 返回的字符串。
LENJSON_API void lenjson_free_string(char* text);

// -----------------------------------------------------------------------------
// 选项默认值
// -----------------------------------------------------------------------------

LENJSON_API lenjson_parse_options lenjson_parse_options_default(void);
LENJSON_API lenjson_dump_options  lenjson_dump_options_default(void);

// -----------------------------------------------------------------------------
// 生命周期
// -----------------------------------------------------------------------------

// 释放句柄。
//   根句柄：由本库拥有，释放时整棵 JSON 树一起销毁；
//   子句柄：只是指向父节点里的一个元素，释放它不会影响父节点里的数据。
// 无论是哪种，都必须恰好释放一次。
LENJSON_API void lenjson_value_free(lenjson_value* value);

// 深拷贝。返回一个新的、拥有自己数据的根句柄，需自行释放。
LENJSON_API lenjson_value* lenjson_value_clone(const lenjson_value* value);

// -----------------------------------------------------------------------------
// 构造
// -----------------------------------------------------------------------------
// 全部返回新的根句柄（拥有自己的数据），失败返回 NULL，需自行释放。

LENJSON_API lenjson_value* lenjson_value_new_null(void);
LENJSON_API lenjson_value* lenjson_value_new_bool(int value);
LENJSON_API lenjson_value* lenjson_value_new_int(int64_t value);
LENJSON_API lenjson_value* lenjson_value_new_double(double value);
// length 传 LENJSON_NUL_TERMINATED 表示按 strlen 计算；允许字符串内含 '\0'。
LENJSON_API lenjson_value* lenjson_value_new_string(const char* text, size_t length);
LENJSON_API lenjson_value* lenjson_value_new_array(void);
LENJSON_API lenjson_value* lenjson_value_new_object(void);

// -----------------------------------------------------------------------------
// 类型判断
// -----------------------------------------------------------------------------

LENJSON_API lenjson_type lenjson_value_type(const lenjson_value* value);
// 类型枚举的可读名字，例如 "integer"。
LENJSON_API const char*   lenjson_type_name(lenjson_type type);

LENJSON_API int lenjson_value_is_null(const lenjson_value* value);
LENJSON_API int lenjson_value_is_bool(const lenjson_value* value);
LENJSON_API int lenjson_value_is_integer(const lenjson_value* value);
LENJSON_API int lenjson_value_is_real(const lenjson_value* value);
LENJSON_API int lenjson_value_is_number(const lenjson_value* value);   // 整数或浮点
LENJSON_API int lenjson_value_is_string(const lenjson_value* value);
LENJSON_API int lenjson_value_is_array(const lenjson_value* value);
LENJSON_API int lenjson_value_is_object(const lenjson_value* value);

// -----------------------------------------------------------------------------
// 取值
// -----------------------------------------------------------------------------

// 布尔。类型不符返回 LENJSON_ERROR_TYPE，*out 不被修改。
LENJSON_API lenjson_status lenjson_value_get_bool(const lenjson_value* value, int* out);

// 整数。浮点会向零截断；类型不符返回 LENJSON_ERROR_TYPE。
LENJSON_API lenjson_status lenjson_value_get_int(const lenjson_value* value, int64_t* out);

// 浮点。整数会提升为 double；类型不符返回 LENJSON_ERROR_TYPE。
LENJSON_API lenjson_status lenjson_value_get_double(const lenjson_value* value, double* out);

// 字符串内容。返回指向值内部缓冲区的指针（借用，不需要释放），
// 有效期到该值被修改或释放为止。类型不符返回 NULL。
// out_length 可为 NULL；非 NULL 时接收字节数（可能包含内嵌的 '\0'）。
LENJSON_API const char* lenjson_value_get_string(const lenjson_value* value, size_t* out_length);

// 字符串内容的一份独立拷贝。返回值必须用 lenjson_free_string 释放。
// 类型不符返回 NULL。
LENJSON_API char* lenjson_value_get_string_copy(const lenjson_value* value, size_t* out_length);

// 元素个数：数组 / 对象为成员数，字符串为字节长度，其它类型为 0。
LENJSON_API size_t lenjson_value_size(const lenjson_value* value);
LENJSON_API int    lenjson_value_empty(const lenjson_value* value);

// -----------------------------------------------------------------------------
// 访问子节点
// -----------------------------------------------------------------------------
// 返回的子句柄指向父节点里的元素（不复制数据），可以继续对它调用
// get_item / get_member。子句柄也要用 lenjson_value_free 释放，
// 但释放它不会销毁父节点里的数据。
// 取值失败返回 NULL，原因见 lenjson_last_error_message()。

// 数组下标。越界返回 NULL（状态 LENJSON_ERROR_OUT_OF_BOUNDS）。
LENJSON_API lenjson_value* lenjson_value_get_item(const lenjson_value* array, size_t index,
                                                   lenjson_status* status);

// 对象成员。键不存在返回 NULL（状态 LENJSON_ERROR_OUT_OF_BOUNDS）。
LENJSON_API lenjson_value* lenjson_value_get_member(const lenjson_value* object, const char* key,
                                                     lenjson_status* status);

// 对象是否含某个键。非 0 表示含有。
LENJSON_API int lenjson_value_has_member(const lenjson_value* object, const char* key);

// -----------------------------------------------------------------------------
// 修改
// -----------------------------------------------------------------------------
// 这三个写入函数在成功后会把 child 的内容“搬”进父节点：
//   * child 必须是自己创建的句柄（根句柄）。借用来的子句柄属于父节点，
//     把它搬走会把父节点的子树掏空，所以这里直接拒绝——
//     需要时请先用 lenjson_value_clone 复制一份。
//   * 搬完之后 child 变成一个空值（相当于 C++ 的移动后状态），
//     但它仍然需要调用 lenjson_value_free 释放一次，否则会漏掉那点内存。
//   * 写入失败时父节点不做任何改动，child 原样保留。
// 换句话说：句柄的释放责任始终在调用方手里，函数只搬走内容，不接管句柄。

// 写数组第 index 项；index 超出当前长度时，中间空位自动填 null。
LENJSON_API lenjson_status lenjson_value_set_item(lenjson_value* array, size_t index,
                                                   lenjson_value* child);

// 写对象成员；键已存在则覆盖原来的值。
LENJSON_API lenjson_status lenjson_value_set_member(lenjson_value* object, const char* key,
                                                     lenjson_value* child);

// 往数组末尾追加一项。
LENJSON_API lenjson_status lenjson_value_push_back(lenjson_value* array, lenjson_value* child);

// 删除对象成员。键不存在返回 LENJSON_ERROR_OUT_OF_BOUNDS。
LENJSON_API lenjson_status lenjson_value_erase_member(lenjson_value* object, const char* key);

// 删除数组第 index 项。越界返回 LENJSON_ERROR_OUT_OF_BOUNDS。
LENJSON_API lenjson_status lenjson_value_erase_item(lenjson_value* array, size_t index);

// 清空内容：容器清空元素，标量变为 null。
LENJSON_API void lenjson_value_clear(lenjson_value* value);

// -----------------------------------------------------------------------------
// 比较
// -----------------------------------------------------------------------------

// 语义相等：对象按“键集合”比较（与顺序无关）；整数与浮点按数值比较。
// 非 0 表示相等。
LENJSON_API int lenjson_value_equal(const lenjson_value* a, const lenjson_value* b);

// -----------------------------------------------------------------------------
// 错误信息
// -----------------------------------------------------------------------------
// 错误信息按线程保存。任一函数失败后，可用它取详细原因；
// 下一次调用任一 API 会覆盖它（成功时会被清空）。

// 错误文本，UTF-8 编码。没有错误时返回空字符串。
// 有效期到本线程下一次调用 API 为止，不要缓存。
LENJSON_API const char* lenjson_last_error_message(void);

// 出错位置。解析失败时行列信息有效，其它错误为 0。
LENJSON_API lenjson_error_info lenjson_last_error_info(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // LENJSON_LENJSON_H
