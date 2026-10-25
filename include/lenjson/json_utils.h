#ifndef LENJSON_JSON_UTILS_H
#define LENJSON_JSON_UTILS_H

// =============================================================================
// json_utils.h —— 底层工具函数
// -----------------------------------------------------------------------------
// 这里放的是与“JSON 语义”无关、但序列化/反序列化必需的脏活：
//   1. 数字与字符串的互转（整数、浮点最短往返）；
//   2. UTF-8 编解码（JSON 的 \uXXXX 是 UTF-16 码元，字符串内部统一用 UTF-8）；
//   3. 字符串转义（把内容安全地写进 JSON 字符串字面量）。
// 把它单独抽出成模块，遵循“单一职责”：Value / Parser / Writer 都只依赖它，
// 而它不依赖任何其它模块，因此可以最先编写、也最容易被单元测试。
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "lenjson/json_export.h"

namespace lenjson {
namespace utils {

// ---------------------------------------------------------------------------
// 数字 -> 字符串
// ---------------------------------------------------------------------------

// 整数直接转十进制文本。
LENJSON_API std::string integer_to_string(long long value);

// 浮点数转“最短且能无损往返”的文本。
// 所谓“最短往返”：用尽量少的小数位数输出，但要保证 strtod(输出) 能精确还原原值。
// 例如 0.1 需要 17 位才能往返，而 3.5 用 2 位就够。
LENJSON_API std::string real_to_string(double value);

// ---------------------------------------------------------------------------
// 字符串 -> 整数
// ---------------------------------------------------------------------------

// 解析 [begin, end) 范围内的十进制整数。
// 返回 true 表示成功且未溢出；若因超出 long long 范围而失败，
// 会把 overflow 置为 true（调用方可据此退化为 double）。
LENJSON_API bool parse_integer(const char* begin, const char* end, long long& out, bool& overflow);

// ---------------------------------------------------------------------------
// UTF-8 编解码
// ---------------------------------------------------------------------------

// 把一个 Unicode 码点按 UTF-8 编码规则追加到 out 末尾。
LENJSON_API void append_utf8(std::string& out, std::uint32_t code_point);

// 从 s 的下标 i 处解码出一个 Unicode 码点。
// 成功时把 i 前进到下一个字符并返回 true；遇到非法字节序列返回 false。
LENJSON_API bool next_utf8(std::string_view s, std::size_t& i, std::uint32_t& code_point);

// ---------------------------------------------------------------------------
// 字符串转义
// ---------------------------------------------------------------------------

// 把 JSON 字符串的“内容”做转义后追加到 out（不含最外层的一对引号）。
// ensure_ascii 为 true 时，所有非 ASCII 字符都会被输出成 \uXXXX 形式，
// 这样生成的文本是纯 ASCII 的，适合放进只允许 ASCII 的老系统。
LENJSON_API void escape_into(std::string_view in, std::string& out, bool ensure_ascii);

// 单个十六进制字符转数值，非法字符返回 -1。
LENJSON_API int hex_value(char c);

} // namespace utils
} // namespace lenjson

#endif // LENJSON_JSON_UTILS_H
