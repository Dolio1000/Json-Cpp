#ifndef LENJSON_JSON_PARSER_H
#define LENJSON_JSON_PARSER_H

// =============================================================================
// json_parser.h —— 反序列化：把 JSON 文本解析成 Value
// -----------------------------------------------------------------------------
// ★ 核心思路：递归下降（recursive descent）
//   JSON 的文法本身就是递归的：
//       value  := object | array | string | number | 'true' | 'false' | 'null'
//       object := '{' (string ':' value (',' string ':' value)*)? '}'
//       array  := '[' (value (',' value)*)? ']'
//   于是“一个函数负责一种语法结构”，函数之间互相调用形成递归，
//   直接把文法翻译成代码即可，清晰且易于扩展。
//
//   本例把解析器实现为一个“状态机对象”：内部持有输入文本与当前读取位置，
//   每个 parse_xxx 方法推进位置并返回结果。解析入口是静态方法 parse()。
// =============================================================================

#include <cstddef>
#include <string>

#include "lenjson/json_export.h"
#include "lenjson/json_value.h"

namespace lenjson {

// 解析行为可调项。
struct LENJSON_API ParseOptions {
    // 最大嵌套深度。递归下降会随嵌套加深而消耗栈，恶意构造的超深 JSON
    // （如几万层 '['）可能把栈“撑爆”。设置上限可把崩溃变成可捕获的异常。
    std::size_t max_depth = 512;

    // 是否容忍 // 与 /* */ 注释（严格 JSON 不允许，但配置文件常用）。
    bool allow_comments = false;
};

// 解析器。对外只需要使用静态方法 parse()。
class LENJSON_API Parser {
public:
    // 解析入口：成功返回 Value，失败抛 ParseError（含行列位置）。
    static Value parse(const std::string& text, const ParseOptions& options = ParseOptions());

private:
    Parser(const std::string& text, const ParseOptions& options)
        : text_(text), options_(options) {}

    // ---- 各语法结构的解析 ----
    Value       parse_value(std::size_t depth);
    Value       parse_object(std::size_t depth);
    Value       parse_array(std::size_t depth);
    std::string parse_string();          // 前置条件：当前字符是 '"'
    Value       parse_number();          // 前置条件：当前字符是 '-' 或数字
    void        parse_literal(const char* literal, const char* name);
    std::uint32_t parse_hex4();          // 解析 \u 后面的 4 位十六进制

    // ---- 辅助 ----
    void skip_whitespace();              // 跳过空白（以及可选注释）
    bool at_end() const;
    char peek() const;                   // 看当前字符，越界返回 '\0'

    // 以当前位置构造并抛出 ParseError（[[noreturn]] 帮助编译器做分支分析）。
    [[noreturn]] void fail(const std::string& detail) const;

    const std::string& text_;
    ParseOptions       options_;
    std::size_t        pos_ = 0;         // 当前读取到的字节下标
};

} // namespace lenjson

#endif // LENJSON_JSON_PARSER_H
