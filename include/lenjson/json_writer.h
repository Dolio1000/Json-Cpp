#ifndef LENJSON_JSON_WRITER_H
#define LENJSON_JSON_WRITER_H

// =============================================================================
// json_writer.h —— 序列化：把 Value 写成 JSON 文本
// -----------------------------------------------------------------------------
// 序列化就是一次对 Value 树的深度优先遍历：遇到标量直接产出文本，
// 遇到数组/对象则先写括号，再递归处理每个元素/成员。
// =============================================================================

#include <string>

#include "lenjson/json_export.h"
#include "lenjson/json_value.h"

namespace lenjson {

// 序列化的可调项。
struct LENJSON_API DumpOptions {
    // 缩进宽度：-1 表示紧凑输出（无空格无换行）；>=0 表示美化输出，
    // 每一层用 indent 个 indent_char 缩进。
    int  indent = -1;
    char indent_char = ' ';

    // true 时把所有非 ASCII 字符转义成 \uXXXX，产出纯 ASCII 文本。
    bool ensure_ascii = false;
};

// 序列化器。
class LENJSON_API Writer {
public:
    static std::string write(const Value& value, const DumpOptions& options = DumpOptions());

private:
    // depth 为当前嵌套层级，仅用于计算缩进。
    static void write_value(std::string& out, const Value& value,
                            std::size_t depth, const DumpOptions& options);
};

} // namespace lenjson

#endif // LENJSON_JSON_WRITER_H
