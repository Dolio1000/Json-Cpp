// =============================================================================
// json_writer.cpp —— 序列化实现
// -----------------------------------------------------------------------------
// 对 Value 树做一次深度优先遍历，边遍历边把文本 append 到同一个 std::string。
// 用“传入输出缓冲区”的方式而不是“层层返回字符串拼接”，可以避免大量临时对象，
// 这是实现序列化器的小技巧。
// =============================================================================

#include "internal/json_writer.h"

#include <cstddef>
#include <string>

#include "internal/json_utils.h"

namespace lenjson {

namespace {

// 写出“换行 + depth 层缩进”。紧凑模式下（indent < 0）不写任何东西。
void write_indent(std::string& out, const DumpOptions& options, std::size_t depth) {
    if (options.indent < 0) {
        return;
    }
    out.push_back('\n');
    out.append(static_cast<std::size_t>(options.indent) * depth, options.indent_char);
}

} // namespace

std::string Writer::write(const Value& value, const DumpOptions& options) {
    std::string out;
    write_value(out, value, 0, options);
    return out;
}

void Writer::write_value(std::string& out, const Value& value,
                         std::size_t depth, const DumpOptions& options) {
    switch (value.type()) {
        case JsonType::Null:
            out += "null";
            break;

        case JsonType::Boolean:
            out += value.as_bool() ? "true" : "false";
            break;

        case JsonType::Integer:
            out += utils::integer_to_string(value.as_integer());
            break;

        case JsonType::Real:
            out += utils::real_to_string(value.as_real());
            break;

        case JsonType::String:
            out.push_back('"');
            utils::escape_into(value.as_string(), out, options.ensure_ascii);
            out.push_back('"');
            break;

        case JsonType::Array: {
            const Value::Array& array = value.as_array();
            if (array.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (std::size_t i = 0; i < array.size(); ++i) {
                if (i > 0) out.push_back(',');
                write_indent(out, options, depth + 1);
                write_value(out, array[i], depth + 1, options);
            }
            write_indent(out, options, depth);
            out.push_back(']');
            break;
        }

        case JsonType::Object: {
            const Value::Object& object = value.as_object();
            if (object.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (std::size_t i = 0; i < object.size(); ++i) {
                if (i > 0) out.push_back(',');
                write_indent(out, options, depth + 1);
                out.push_back('"');
                utils::escape_into(object[i].first, out, options.ensure_ascii);
                out.push_back('"');
                out.push_back(':');
                if (options.indent >= 0) out.push_back(' ');   // 美化时冒号后留一个空格
                write_value(out, object[i].second, depth + 1, options);
            }
            write_indent(out, options, depth);
            out.push_back('}');
            break;
        }
    }
}

} // namespace lenjson
