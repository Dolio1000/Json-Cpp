// =============================================================================
// json_parser.cpp —— 递归下降解析器实现
// -----------------------------------------------------------------------------
// 解析流程：parse() 入口 -> skip_whitespace -> parse_value -> 按首字符分派。
// 每个 parse_xxx 都遵守一个约定：进入时 pos_ 指向本结构的首个字符，
// 返回时 pos_ 指向本结构之后的第一个字符（尚未跳空白）。
// =============================================================================

#include "internal/json_parser.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#include "internal/json_utils.h"

namespace lenjson {

// -----------------------------------------------------------------------------
// 入口
// -----------------------------------------------------------------------------
Value Parser::parse(const std::string& text, const ParseOptions& options) {
    Parser parser(text, options);

    parser.skip_whitespace();
    if (parser.at_end()) {
        parser.fail("输入为空，缺少 JSON 值");
    }

    Value value = parser.parse_value(0);

    // 严格模式：一个合法的 JSON 文档在值之后不应再有其它内容。
    parser.skip_whitespace();
    if (!parser.at_end()) {
        parser.fail("解析结束后仍有多余内容");
    }
    return value;
}

// -----------------------------------------------------------------------------
// 辅助
// -----------------------------------------------------------------------------

bool Parser::at_end() const {
    return pos_ >= text_.size();
}

char Parser::peek() const {
    return pos_ < text_.size() ? text_[pos_] : '\0';
}

// 跳过空白字符；开启注释选项时，顺带跳过 // 行注释与 /* */ 块注释。
void Parser::skip_whitespace() {
    for (;;) {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }

        if (options_.allow_comments && pos_ + 1 < text_.size() && text_[pos_] == '/') {
            if (text_[pos_ + 1] == '/') {
                pos_ += 2;
                while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
                continue;
            }
            if (text_[pos_ + 1] == '*') {
                pos_ += 2;
                while (pos_ + 1 < text_.size() && !(text_[pos_] == '*' && text_[pos_ + 1] == '/')) {
                    ++pos_;
                }
                if (pos_ + 1 >= text_.size()) {
                    fail("块注释没有闭合（缺少 */）");
                }
                pos_ += 2;
                continue;
            }
        }
        break;
    }
}

// 以当前 pos_ 为位置，构造并抛出 ParseError。行/列在这里按需扫描得到。
void Parser::fail(const std::string& detail) const {
    std::size_t line = 1;
    std::size_t column = 1;
    for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
        if (text_[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    throw ParseError(detail, line, column, pos_);
}

// -----------------------------------------------------------------------------
// 字面量：true / false / null
// -----------------------------------------------------------------------------
void Parser::parse_literal(const char* literal, const char* name) {
    const std::size_t length = std::strlen(literal);
    if (text_.compare(pos_, length, literal) != 0) {
        fail(std::string("期望字面量 ") + name);
    }
    pos_ += length;
}

// -----------------------------------------------------------------------------
// 值分派
// -----------------------------------------------------------------------------
Value Parser::parse_value(std::size_t depth) {
    if (depth > options_.max_depth) {
        fail("嵌套层级超过上限 (" + std::to_string(options_.max_depth) + ")，可能是恶意输入");
    }

    char c = peek();
    switch (c) {
        case '{': return parse_object(depth);
        case '[': return parse_array(depth);
        case '"': return Value(parse_string());
        case 't': parse_literal("true", "true");   return Value(true);
        case 'f': parse_literal("false", "false"); return Value(false);
        case 'n': parse_literal("null", "null");   return Value(nullptr);
        default:
            if (c == '-' || (c >= '0' && c <= '9')) {
                return parse_number();
            }
            if (c == '\0') {
                fail("意外的输入结尾，缺少 JSON 值");
            }
            fail(std::string("无法识别的字符 '") + c + "'");
    }
    return Value();   // 不可达；仅为消除编译器告警
}

// -----------------------------------------------------------------------------
// 字符串
// -----------------------------------------------------------------------------
std::string Parser::parse_string() {
    ++pos_;   // 跳过开引号
    std::string out;

    for (;;) {
        if (at_end()) {
            fail("字符串没有闭合（缺少右引号 '\"'）");
        }
        unsigned char c = static_cast<unsigned char>(text_[pos_]);

        if (c == '"') {
            ++pos_;
            break;
        }

        if (c == '\\') {
            ++pos_;                       // 跳过反斜杠，指向转义字符
            if (at_end()) {
                fail("转义字符不完整");
            }
            char esc = text_[pos_];
            switch (esc) {
                case '"':  out.push_back('"');  ++pos_; break;
                case '\\': out.push_back('\\'); ++pos_; break;
                case '/':  out.push_back('/');  ++pos_; break;
                case 'b':  out.push_back('\b'); ++pos_; break;
                case 'f':  out.push_back('\f'); ++pos_; break;
                case 'n':  out.push_back('\n'); ++pos_; break;
                case 'r':  out.push_back('\r'); ++pos_; break;
                case 't':  out.push_back('\t'); ++pos_; break;
                case 'u': {
                    ++pos_;                        // 跳过 'u'
                    std::uint32_t cp = parse_hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // 高代理项：必须紧跟一个 \uXXXX 低代理项，二者合成 BMP 外的码点。
                        if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                            fail("UTF-16 代理对不完整：高代理项后应跟一个 \\uXXXX");
                        }
                        pos_ += 2;
                        std::uint32_t lo = parse_hex4();
                        if (lo < 0xDC00 || lo > 0xDFFF) {
                            fail("UTF-16 代理对无效：低代理项不在 0xDC00~0xDFFF 范围");
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("UTF-16 代理对无效：出现了孤立的低代理项");
                    }
                    utils::append_utf8(out, cp);   // 统一以 UTF-8 存入内存
                    break;
                }
                default:
                    fail(std::string("非法的转义序列 '\\") + esc + "'");
            }
        } else if (c < 0x20) {
            // JSON 规定：0x00~0x1F 的控制字符不能原样出现在字符串里。
            fail("字符串中不允许出现未转义的控制字符");
        } else {
            // 普通字节（含 UTF-8 多字节序列）原样拷贝。
            out.push_back(static_cast<char>(c));
            ++pos_;
        }
    }
    return out;
}

// 解析 \u 之后的 4 位十六进制数字，返回其数值。
std::uint32_t Parser::parse_hex4() {
    if (pos_ + 4 > text_.size()) {
        fail("\\u 转义需要 4 位十六进制数字");
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        int h = utils::hex_value(text_[pos_ + i]);
        if (h < 0) {
            fail("\\u 转义中出现了非十六进制字符");
        }
        value = (value << 4) | static_cast<std::uint32_t>(h);
    }
    pos_ += 4;
    return value;
}

// -----------------------------------------------------------------------------
// 数字
// -----------------------------------------------------------------------------
// 严格按 JSON 文法校验，再交给 strtoll/strtod 做实际转换：
//     number := '-'? int frac? exp?
//     int    := '0' | [1-9][0-9]*          （不允许前导 0，如 012）
//     frac   := '.' [0-9]+
//     exp    := [eE] [+-]? [0-9]+
Value Parser::parse_number() {
    const std::size_t start = pos_;

    if (peek() == '-') {
        ++pos_;
    }

    if (at_end()) {
        fail("数字不完整");
    }

    // 整数部分
    if (peek() == '0') {
        ++pos_;
    } else if (peek() >= '1' && peek() <= '9') {
        while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
    } else {
        fail("数字格式错误：整数部分不合法");
    }

    bool is_real = false;

    // 小数部分
    if (!at_end() && peek() == '.') {
        is_real = true;
        ++pos_;
        if (at_end() || !std::isdigit(static_cast<unsigned char>(peek()))) {
            fail("小数点后至少需要一位数字");
        }
        while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
    }

    // 指数部分
    if (!at_end() && (peek() == 'e' || peek() == 'E')) {
        is_real = true;
        ++pos_;
        if (!at_end() && (peek() == '+' || peek() == '-')) ++pos_;
        if (at_end() || !std::isdigit(static_cast<unsigned char>(peek()))) {
            fail("指数部分至少需要一位数字");
        }
        while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
    }

    const char* begin = text_.data() + start;
    const char* end   = text_.data() + pos_;

    // 没有小数/指数 -> 优先按整数存（保精度）。
    if (!is_real) {
        long long int_value = 0;
        bool overflow = false;
        if (utils::parse_integer(begin, end, int_value, overflow) && !overflow) {
            return Value(int_value);
        }
        // 整数超出 long long 范围时，退化为 double（与多数 JSON 库一致）。
    }

    double real_value = std::strtod(text_.c_str() + start, nullptr);
    return Value(real_value);
}

// -----------------------------------------------------------------------------
// 数组
// -----------------------------------------------------------------------------
Value Parser::parse_array(std::size_t depth) {
    ++pos_;   // 跳过 '['
    Value::Array array;

    skip_whitespace();
    if (peek() == ']') {   // 空数组
        ++pos_;
        return Value(std::move(array));
    }

    for (;;) {
        skip_whitespace();
        array.push_back(parse_value(depth + 1));   // 递归解析元素

        skip_whitespace();
        char c = peek();
        if (c == ',') {
            ++pos_;
            continue;
        }
        if (c == ']') {
            ++pos_;
            break;
        }
        if (c == '\0') {
            fail("数组没有闭合（缺少 ']'）");
        }
        fail("数组中期望 ',' 或 ']'");
    }
    return Value(std::move(array));
}

// -----------------------------------------------------------------------------
// 对象
// -----------------------------------------------------------------------------
Value Parser::parse_object(std::size_t depth) {
    ++pos_;   // 跳过 '{'
    Value::Object object;

    skip_whitespace();
    if (peek() == '}') {   // 空对象
        ++pos_;
        return Value(std::move(object));
    }

    for (;;) {
        skip_whitespace();
        if (peek() != '"') {
            fail("对象的键必须是字符串");
        }
        std::string key = parse_string();

        skip_whitespace();
        if (peek() != ':') {
            fail("对象的键之后期望 ':'");
        }
        ++pos_;

        skip_whitespace();
        Value value = parse_value(depth + 1);   // 递归解析值

        // 重复键：用后出现的值覆盖先前的值，但保留首次出现的位置。
        bool replaced = false;
        for (auto& kv : object) {
            if (kv.first == key) {
                kv.second = std::move(value);
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            object.emplace_back(std::move(key), std::move(value));
        }

        skip_whitespace();
        char c = peek();
        if (c == ',') {
            ++pos_;
            continue;
        }
        if (c == '}') {
            ++pos_;
            break;
        }
        if (c == '\0') {
            fail("对象没有闭合（缺少 '}'）");
        }
        fail("对象中期望 ',' 或 '}'");
    }
    return Value(std::move(object));
}

} // namespace lenjson
