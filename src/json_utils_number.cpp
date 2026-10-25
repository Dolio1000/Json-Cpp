// =============================================================================
// json_utils_number.cpp —— 数字与字符串的互转
// -----------------------------------------------------------------------------
// 这一块看着简单，其实藏着不少“坑”，是很好的学习素材：
//   * 整数要保精度，只有在超出 long long 范围时才退化为 double；
//   * 浮点要用“最短往返”输出——位数太少会丢精度，位数太多又难读。
// =============================================================================

#include "internal/json_utils.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace lenjson {
namespace utils {

// -----------------------------------------------------------------------------
// 整数 -> 字符串
// -----------------------------------------------------------------------------
std::string integer_to_string(long long value) {
    char buffer[32];   // long long 最多 20 位十进制（含符号），32 足够
    int length = std::snprintf(buffer, sizeof(buffer), "%lld", value);
    return std::string(buffer, static_cast<std::size_t>(length));
}

// -----------------------------------------------------------------------------
// 字符串 -> 整数
// -----------------------------------------------------------------------------
// 用 strtoll 完成转换，并借助 errno 判断是否溢出（ERANGE）。
bool parse_integer(const char* begin, const char* end, long long& out, bool& overflow) {
    overflow = false;

    // strtoll 需要以 '\0' 结尾的字符串，这里复制一份到临时缓冲。
    std::string text(begin, end);

    errno = 0;
    char* stop = nullptr;
    long long value = std::strtoll(text.c_str(), &stop, 10);

    if (errno == ERANGE) {
        overflow = true;      // 超出 long long 表示范围
        return false;
    }
    if (stop != text.c_str() + text.size()) {
        return false;         // 未能完整解析（正常情况下不会发生）
    }

    out = value;
    return true;
}

// -----------------------------------------------------------------------------
// 浮点 -> 字符串（最短往返）
// -----------------------------------------------------------------------------
// 思路：从 1 位有效数字开始，逐步增加到 17 位，一旦“输出再读回来”与原值完全
// 相等就立刻停下，这样得到的就是“能无损还原的最短字符串”。
// 17 是因为 double 的最坏情况需要 17 位有效数字才能唯一确定。
std::string real_to_string(double value) {
    // JSON 没有 NaN / Infinity 的概念，遇到时退化为 null（保证输出仍是合法 JSON）。
    if (std::isnan(value) || std::isinf(value)) {
        return "null";
    }

    char buffer[64];
    int length = 0;
    for (int precision = 1; precision <= 17; ++precision) {
        length = std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        if (std::strtod(buffer, nullptr) == value) {
            break;   // 已经能精确还原，停止增加精度
        }
    }

    std::string result(buffer, static_cast<std::size_t>(length));

    // 若结果看起来像整数（没有小数点也没有指数），补一个 ".0"，
    // 以免序列化再解析后从“浮点”变成“整数”，破坏往返类型一致性。
    if (result.find_first_of(".eE") == std::string::npos) {
        result += ".0";
    }
    return result;
}

} // namespace utils
} // namespace lenjson
