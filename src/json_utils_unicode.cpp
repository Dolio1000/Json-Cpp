// =============================================================================
// json_utils_unicode.cpp —— UTF-8 编解码与字符串转义
// -----------------------------------------------------------------------------
// 为什么需要这一块？
//   * JSON 文本里字符串可能是 \u4e2d 这样的转义，而内存里我们统一用 UTF-8 存；
//   * 序列化时又要把 UTF-8 内容安全地“转义”回文本，必要时还能转成纯 ASCII。
//   * \uXXXX 表示的是一个 UTF-16 码元，BMP 之外的字符需要用“代理对”表示，
//     所以编解码时要注意 0x10000 以上的码点（4 字节 UTF-8）。
// =============================================================================

#include "lenjson/json_utils.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lenjson {
namespace utils {

// 十六进制字符 -> 数值；非法返回 -1。
int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 把一个 Unicode 码点按 UTF-8 规则编码后追加到 out。
// UTF-8 的编码规则（用二进制看位数）：
//   码点 <= 0x7F      : 0xxxxxxx
//   码点 <= 0x7FF     : 110xxxxx 10xxxxxx
//   码点 <= 0xFFFF    : 1110xxxx 10xxxxxx 10xxxxxx
//   码点 <= 0x10FFFF  : 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point <= 0x7F) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

// 从 s[i] 处解码一个 UTF-8 码点。成功则把 i 前进并返回 true。
bool next_utf8(std::string_view s, std::size_t& i, std::uint32_t& code_point) {
    if (i >= s.size()) {
        return false;
    }
    unsigned char c0 = static_cast<unsigned char>(s[i]);

    std::size_t extra = 0;   // 后续“续接字节”的个数
    std::uint32_t value = 0;

    if (c0 < 0x80) {
        code_point = c0;
        i += 1;
        return true;
    } else if ((c0 & 0xE0) == 0xC0) {
        extra = 1;
        value = c0 & 0x1F;
    } else if ((c0 & 0xF0) == 0xE0) {
        extra = 2;
        value = c0 & 0x0F;
    } else if ((c0 & 0xF8) == 0xF0) {
        extra = 3;
        value = c0 & 0x07;
    } else {
        return false;        // 非法首字节
    }

    if (i + extra >= s.size()) {
        return false;        // 字节数不足
    }

    for (std::size_t k = 1; k <= extra; ++k) {
        unsigned char ck = static_cast<unsigned char>(s[i + k]);
        if ((ck & 0xC0) != 0x80) {
            return false;    // 不是合法的续接字节
        }
        value = (value << 6) | (ck & 0x3F);
    }

    code_point = value;
    i += extra + 1;
    return true;
}

namespace {

// 把码点输出成 \uXXXX 形式（4 位小写十六进制）。
void append_hex4(std::string& out, std::uint32_t code_unit) {
    static const char digits[] = "0123456789abcdef";
    out += "\\u";
    out.push_back(digits[(code_unit >> 12) & 0xF]);
    out.push_back(digits[(code_unit >> 8) & 0xF]);
    out.push_back(digits[(code_unit >> 4) & 0xF]);
    out.push_back(digits[code_unit & 0xF]);
}

} // namespace

// 把字符串内容做 JSON 转义后追加到 out（不含最外层引号）。
void escape_into(std::string_view in, std::string& out, bool ensure_ascii) {
    for (std::size_t i = 0; i < in.size();) {
        unsigned char c = static_cast<unsigned char>(in[i]);

        // 1) 需要转义的短序列
        switch (c) {
            case '"':  out += "\\\""; ++i; continue;
            case '\\': out += "\\\\"; ++i; continue;
            case '\b': out += "\\b";  ++i; continue;
            case '\f': out += "\\f";  ++i; continue;
            case '\n': out += "\\n";  ++i; continue;
            case '\r': out += "\\r";  ++i; continue;
            case '\t': out += "\\t";  ++i; continue;
            default: break;
        }

        // 2) 其余控制字符统一写成 \u00XX
        if (c < 0x20) {
            append_hex4(out, c);
            ++i;
            continue;
        }

        // 3) 普通 ASCII 直接输出
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }

        // 4) 非 ASCII：不需要转义时原样拷贝 UTF-8 字节
        if (!ensure_ascii) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }

        // 5) 需要 ASCII 化：解码出码点，再按 UTF-16 编码规则输出 \uXXXX
        std::uint32_t cp = 0;
        std::size_t next = i;
        if (!next_utf8(in, next, cp)) {
            // 非法 UTF-8：退化为逐字节原样输出，避免数据丢失
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        if (cp <= 0xFFFF) {
            append_hex4(out, cp);
        } else {
            // BMP 之外：拆成一对代理项
            std::uint32_t t = cp - 0x10000;
            append_hex4(out, 0xD800 + (t >> 10));
            append_hex4(out, 0xDC00 + (t & 0x3FF));
        }
        i = next;
    }
}

} // namespace utils
} // namespace lenjson
