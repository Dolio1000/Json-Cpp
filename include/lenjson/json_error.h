#ifndef LENJSON_JSON_ERROR_H
#define LENJSON_JSON_ERROR_H

// =============================================================================
// json_error.h —— 异常体系
// -----------------------------------------------------------------------------
// 设计思路（重点学习）：
//   一个库对外暴露的“错误”也是一种接口设计。这里采用“异常族”的做法：
//     1. 所有异常都继承自同一个基类 JsonException；
//     2. JsonException 又继承自标准库的 std::runtime_error。
//   这样调用方有两种粒度可选：
//     catch (const lenjson::ParseError&)  —— 只关心“解析失败”这一种情况；
//     catch (const lenjson::JsonException&) —— 兜底所有本库抛出的异常；
//     catch (const std::exception&)        —— 连标准库异常一起兜底。
//   继承 std::runtime_error 的好处是：what() 这类标准接口可以直接复用，
//   调用方无需为本库学习新的错误处理范式。
// =============================================================================

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#include "lenjson/json_export.h"

namespace lenjson {

// 库内所有异常的基类。
class LENJSON_API JsonException : public std::runtime_error {
public:
    explicit JsonException(const std::string& message) : std::runtime_error(message) {}
};

// -----------------------------------------------------------------------------
// ParseError —— 解析阶段出错。
// 相比普通异常，它额外记录了“错误发生的位置”（字节下标 / 行 / 列），
// 这样在解析配置文件、网络报文时能快速定位到具体哪一行有问题。
// -----------------------------------------------------------------------------
class LENJSON_API ParseError : public JsonException {
public:
    ParseError(std::string detail, std::size_t line, std::size_t column, std::size_t position)
        : JsonException(build_message(detail, line, column)),
          detail_(std::move(detail)),
          line_(line),
          column_(column),
          position_(position) {}

    // 出错原因的简短描述（不含位置前缀）。
    const std::string& detail() const noexcept { return detail_; }
    // 1 起算的行号。
    std::size_t line() const noexcept { return line_; }
    // 1 起算的列号。
    std::size_t column() const noexcept { return column_; }
    // 从输入开头算起的字节下标。
    std::size_t position() const noexcept { return position_; }

private:
    // 把“原因 + 位置”组装成一段可读的中文提示，作为 what() 的内容。
    static std::string build_message(const std::string& detail, std::size_t line, std::size_t column) {
        return "解析错误 (第 " + std::to_string(line) + " 行, 第 " + std::to_string(column)
             + " 列): " + detail;
    }

    std::string detail_;
    std::size_t line_;
    std::size_t column_;
    std::size_t position_;
};

// -----------------------------------------------------------------------------
// TypeException —— 类型不匹配。
// 例如对一个数字调用 as_string()，或对一个对象调用 as_array()。
// 把“类型错误”单独成类，方便调用方区分“数据格式错”与“解析错”。
// -----------------------------------------------------------------------------
class LENJSON_API TypeException : public JsonException {
public:
    explicit TypeException(const std::string& message) : JsonException(message) {}
};

// -----------------------------------------------------------------------------
// OutOfRange —— 越界 / 键不存在。
// 例如数组下标访问越界，或在对象里 at("不存在的键")。
// -----------------------------------------------------------------------------
class LENJSON_API OutOfRange : public JsonException {
public:
    explicit OutOfRange(const std::string& message) : JsonException(message) {}
};

} // namespace lenjson

#endif // LENJSON_JSON_ERROR_H
