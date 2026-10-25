// =============================================================================
// json_c_api.cpp —— C 接口的实现（本 DLL 唯一的对外导出面）
// -----------------------------------------------------------------------------
// 这个文件把内部那套 C++ 实现（lenjson::Value / Parser / Writer）包成一层
// 纯 C 的 ABI。跨 DLL 边界的规矩只有四条，全部在本文件里落实：
//
//   1. 边界上只出现：指针、int64_t、double、定长 POD 结构体、const char*；
//   2. 字符串缓冲区一律由本库 new、由本库 delete（分配与释放在同一个 CRT 里）；
//   3. 内部抛出的异常全部在本地被捕获，翻译成状态码，绝不跨边界；
//   4. 导出表里只有 extern "C" 的函数，内部 C++ 符号一律不导出。
//
// ★ 句柄模型
//   公开头文件里 lenjson_value 是不透明结构，这里给出它的完整定义：
//       struct lenjson_value { lenjson::Value* node; bool owned; };
//   owned == true  -> 根句柄，本句柄拥有 node，释放时整棵树一起销毁；
//   owned == false -> 子句柄，node 属于父句柄，释放时只丢掉这层包装。
//   调用方因此只需要记一条规则：凡是 API 返回的句柄，都释放一次。
//
// ★ 为什么子句柄要“包一层”而不是直接返回 Value*
//   直接返回节点指针的话，调用方就无法区分“根”和“子”，
//   lenjson_value_free 也就无法决定要不要销毁节点——误释放一个子句柄
//   就会让父节点里的数据变成悬垂指针。包一层把这个区分藏在库里，
//   调用方不必记忆“借用 / 拥有”的差别。
// =============================================================================

#include "lenjson/lenjson.h"

#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "internal/json_error.h"
#include "internal/json_parser.h"
#include "internal/json_value.h"
#include "internal/json_writer.h"

// -----------------------------------------------------------------------------
// 句柄的完整定义（公开头文件里只声明了不透明的前置声明）
// -----------------------------------------------------------------------------
struct lenjson_value {
    lenjson::Value* node;   // 指向 JSON 树中的节点
    bool            owned;  // true = 本句柄拥有 node
};

namespace {

// 内部信号：参数非法。单独成类，避免和“类型不匹配”混为一谈。
struct InvalidArgument {
    std::string message;
};

// -----------------------------------------------------------------------------
// 线程局部的错误信息
// -----------------------------------------------------------------------------
// 按线程保存，天然线程安全：不同线程各自记录自己的最后一次错误，
// 不需要任何锁。代价是错误文本在“本线程下一次调用 API”后就失效，
// 调用方若要保留必须自己拷贝一份。
// -----------------------------------------------------------------------------
struct ErrorState {
    std::string       message;
    lenjson_error_info info;
};

thread_local ErrorState g_error;

void set_error(lenjson_status status, const std::string& message,
               size_t line = 0, size_t column = 0, size_t position = 0) {
    g_error.message = message;
    g_error.info.line     = line;
    g_error.info.column   = column;
    g_error.info.position = position;
    (void)status;
}

void clear_error() {
    g_error.message.clear();
    g_error.info.line     = 0;
    g_error.info.column   = 0;
    g_error.info.position = 0;
}

lenjson_status set_status(lenjson_status* out, lenjson_status value) {
    if (out != nullptr) {
        *out = value;
    }
    return value;
}

// -----------------------------------------------------------------------------
// 异常 -> 状态码
// -----------------------------------------------------------------------------
// 把一次可能抛异常的内部调用包起来。成功时清空错误信息并返回 LENJSON_OK；
// 失败时按异常类型翻译成状态码，并把详细信息写进线程局部错误信息。
// 注意 catch 的顺序：子类必须排在基类前面。
// -----------------------------------------------------------------------------
template <typename Fn>
lenjson_status guard(lenjson_status* status, Fn&& fn) {
    try {
        fn();
    } catch (const InvalidArgument& e) {
        set_error(LENJSON_ERROR_INVALID_ARGUMENT, e.message);
        return set_status(status, LENJSON_ERROR_INVALID_ARGUMENT);
    } catch (const lenjson::ParseError& e) {
        set_error(LENJSON_ERROR_PARSE, e.what(), e.line(), e.column(), e.position());
        return set_status(status, LENJSON_ERROR_PARSE);
    } catch (const lenjson::TypeException& e) {
        set_error(LENJSON_ERROR_TYPE, e.what());
        return set_status(status, LENJSON_ERROR_TYPE);
    } catch (const lenjson::OutOfRange& e) {
        set_error(LENJSON_ERROR_OUT_OF_BOUNDS, e.what());
        return set_status(status, LENJSON_ERROR_OUT_OF_BOUNDS);
    } catch (const lenjson::JsonException& e) {
        set_error(LENJSON_ERROR_INTERNAL, e.what());
        return set_status(status, LENJSON_ERROR_INTERNAL);
    } catch (const std::bad_alloc&) {
        set_error(LENJSON_ERROR_MEMORY, "内存分配失败");
        return set_status(status, LENJSON_ERROR_MEMORY);
    } catch (const std::exception& e) {
        set_error(LENJSON_ERROR_INTERNAL, e.what());
        return set_status(status, LENJSON_ERROR_INTERNAL);
    } catch (...) {
        set_error(LENJSON_ERROR_INTERNAL, "未知的内部错误");
        return set_status(status, LENJSON_ERROR_INTERNAL);
    }
    clear_error();
    return set_status(status, LENJSON_OK);
}

// -----------------------------------------------------------------------------
// 句柄工厂
// -----------------------------------------------------------------------------

// 造一个拥有自己数据的根句柄。
lenjson_value* make_owned(lenjson::Value&& value) {
    std::unique_ptr<lenjson::Value> node(new lenjson::Value(std::move(value)));
    lenjson_value* handle = new lenjson_value{node.get(), true};
    node.release();   // 所有权移交给 handle
    return handle;
}

// 造一个指向已有节点的子句柄（不复制数据）。
lenjson_value* make_borrowed(lenjson::Value* node) {
    return new lenjson_value{node, false};
}

// 参数校验：句柄不能为空。
lenjson::Value& require_node(const lenjson_value* handle, const char* what) {
    if (handle == nullptr || handle->node == nullptr) {
        throw InvalidArgument{std::string("参数为空：") + what};
    }
    return *handle->node;
}

// 取出待写入的子节点，并确认它是“可转移所有权”的根句柄。
// 借用句柄（子句柄）里的数据属于父节点，把它搬走会把父节点的子树掏空，
// 因此这里直接拒绝，让调用方显式 clone 一份。
lenjson::Value& require_transferable(lenjson_value* child, const char* what) {
    if (child == nullptr || child->node == nullptr) {
        throw InvalidArgument{std::string("参数为空：") + what};
    }
    if (!child->owned) {
        throw InvalidArgument{
            std::string(what) + "：只能接收自己创建的值；借用来的句柄请先用 lenjson_value_clone 复制一份"};
    }
    return *child->node;
}

// 字符串长度：LENJSON_NUL_TERMINATED 表示按 strlen 计算。
size_t resolve_length(const char* text, size_t length) {
    if (length == LENJSON_NUL_TERMINATED) {
        return std::strlen(text);
    }
    return length;
}

} // namespace

// =============================================================================
// 解析与序列化
// =============================================================================

LENJSON_API lenjson_value* lenjson_parse_ex(const char* text, size_t length,
                                             const lenjson_parse_options* options,
                                             lenjson_status* status) {
    set_status(status, LENJSON_OK);

    if (text == nullptr) {
        set_error(LENJSON_ERROR_INVALID_ARGUMENT, "待解析的文本为空");
        set_status(status, LENJSON_ERROR_INVALID_ARGUMENT);
        return nullptr;
    }

    const std::string input(text, resolve_length(text, length));

    lenjson::ParseOptions opts;
    if (options != nullptr) {
        // max_depth 传 0 表示“用默认值”，而不是“禁止嵌套”。
        opts.max_depth      = options->max_depth != 0 ? options->max_depth : 512;
        opts.allow_comments = options->allow_comments != 0;
    }

    lenjson_value* result = nullptr;
    const lenjson_status rc = guard(status, [&]() {
        lenjson::Value parsed = lenjson::Parser::parse(input, opts);
        result = make_owned(std::move(parsed));
    });
    return rc == LENJSON_OK ? result : nullptr;
}

LENJSON_API lenjson_value* lenjson_parse(const char* text, size_t length, lenjson_status* status) {
    return lenjson_parse_ex(text, length, nullptr, status);
}

LENJSON_API char* lenjson_dump(const lenjson_value* value, const lenjson_dump_options* options,
                               size_t* out_length) {
    if (out_length != nullptr) {
        *out_length = 0;
    }

    char* text = nullptr;
    const lenjson_status rc = guard(nullptr, [&]() {
        const lenjson::Value& node = require_node(value, "value");

        lenjson::DumpOptions opts;
        if (options != nullptr) {
            opts.indent       = options->indent;
            opts.indent_char  = options->indent_char != '\0' ? options->indent_char : ' ';
            opts.ensure_ascii = options->ensure_ascii != 0;
        }

        const std::string dumped = lenjson::Writer::write(node, opts);

        // 由本库分配、由本库释放：调用方永远不要自己 free()。
        char* buffer = new char[dumped.size() + 1];
        std::memcpy(buffer, dumped.data(), dumped.size());
        buffer[dumped.size()] = '\0';

        if (out_length != nullptr) {
            *out_length = dumped.size();
        }
        text = buffer;
    });
    return rc == LENJSON_OK ? text : nullptr;
}

LENJSON_API void lenjson_free_string(char* text) {
    delete[] text;
}

// =============================================================================
// 选项默认值
// =============================================================================

LENJSON_API lenjson_parse_options lenjson_parse_options_default(void) {
    lenjson_parse_options options;
    options.max_depth      = 512;
    options.allow_comments = 0;
    return options;
}

LENJSON_API lenjson_dump_options lenjson_dump_options_default(void) {
    lenjson_dump_options options;
    options.indent       = -1;   // 默认紧凑输出
    options.indent_char  = ' ';
    options.ensure_ascii = 0;
    return options;
}

// =============================================================================
// 生命周期
// =============================================================================

LENJSON_API void lenjson_value_free(lenjson_value* value) {
    if (value == nullptr) {
        return;
    }
    if (value->owned) {
        delete value->node;   // 根句柄：连整棵树一起销毁
    }
    delete value;
}

LENJSON_API lenjson_value* lenjson_value_clone(const lenjson_value* value) {
    lenjson_value* result = nullptr;
    const lenjson_status rc = guard(nullptr, [&]() {
        const lenjson::Value& node = require_node(value, "value");
        result = make_owned(lenjson::Value(node));   // 深拷贝
    });
    return rc == LENJSON_OK ? result : nullptr;
}

// =============================================================================
// 构造
// =============================================================================

LENJSON_API lenjson_value* lenjson_value_new_null(void) {
    lenjson_value* result = nullptr;
    const lenjson_status rc = guard(nullptr, [&]() {
        result = make_owned(lenjson::Value());
    });
    return rc == LENJSON_OK ? result : nullptr;
}

LENJSON_API lenjson_value* lenjson_value_new_bool(int value) {
    lenjson_value* result = nullptr;
    const lenjson_status rc = guard(nullptr, [&]() {
        result = make_owned(lenjson::Value(value != 0));
    });
    return rc == LENJSON_OK ? result : nullptr;
}

LENJSON_API lenjson_value* lenjson_value_new_int(int64_t value) {
    lenjson_value* result = nullptr;
    const lenjson_status rc = guard(nullptr, [&]() {
        result = make_owned(lenjson::Value(static_cast<long long>(value)));
    });
    return rc == LENJSON_OK ? result : nullptr;
}

LENJSON_API lenjson_value* lenjson_value_new_double(double value) {
    lenjson_value* result = nullptr;
    const lenjson_status rc = guard(nullptr, [&]() {
        result = make_owned(lenjson::Value(value));
    });
    return rc == LENJSON_OK ? result : nullptr;
}

LENJSON_API lenjson_value* lenjson_value_new_string(const char* text, size_t length) {
    lenjson_value* result = nullptr;
    const lenjson_status rc = guard(nullptr, [&]() {
        if (text == nullptr) {
            throw InvalidArgument{"字符串内容为空"};
        }
        const std::string content(text, resolve_length(text, length));
        result = make_owned(lenjson::Value(content));
    });
    return rc == LENJSON_OK ? result : nullptr;
}

LENJSON_API lenjson_value* lenjson_value_new_array(void) {
    lenjson_value* result = nullptr;
    const lenjson_status rc = guard(nullptr, [&]() {
        result = make_owned(lenjson::Value(lenjson::Value::Array{}));
    });
    return rc == LENJSON_OK ? result : nullptr;
}

LENJSON_API lenjson_value* lenjson_value_new_object(void) {
    lenjson_value* result = nullptr;
    const lenjson_status rc = guard(nullptr, [&]() {
        result = make_owned(lenjson::Value(lenjson::Value::Object{}));
    });
    return rc == LENJSON_OK ? result : nullptr;
}

// =============================================================================
// 类型判断
// =============================================================================

LENJSON_API lenjson_type lenjson_value_type(const lenjson_value* value) {
    if (value == nullptr || value->node == nullptr) {
        set_error(LENJSON_ERROR_INVALID_ARGUMENT, "参数为空：value");
        return LENJSON_TYPE_NULL;
    }
    clear_error();
    // 内部枚举与 C 枚举的取值顺序一一对应，可以直接转换。
    return static_cast<lenjson_type>(value->node->type());
}

LENJSON_API const char* lenjson_type_name(lenjson_type type) {
    switch (type) {
        case LENJSON_TYPE_NULL:    return "null";
        case LENJSON_TYPE_BOOLEAN: return "boolean";
        case LENJSON_TYPE_INTEGER: return "integer";
        case LENJSON_TYPE_REAL:    return "real";
        case LENJSON_TYPE_STRING:  return "string";
        case LENJSON_TYPE_ARRAY:   return "array";
        case LENJSON_TYPE_OBJECT:  return "object";
    }
    return "unknown";
}

LENJSON_API int lenjson_value_is_null(const lenjson_value* value) {
    return (value != nullptr && value->node != nullptr && value->node->is_null()) ? 1 : 0;
}

LENJSON_API int lenjson_value_is_bool(const lenjson_value* value) {
    return (value != nullptr && value->node != nullptr && value->node->is_bool()) ? 1 : 0;
}

LENJSON_API int lenjson_value_is_integer(const lenjson_value* value) {
    return (value != nullptr && value->node != nullptr && value->node->is_integer()) ? 1 : 0;
}

LENJSON_API int lenjson_value_is_real(const lenjson_value* value) {
    return (value != nullptr && value->node != nullptr && value->node->is_real()) ? 1 : 0;
}

LENJSON_API int lenjson_value_is_number(const lenjson_value* value) {
    return (value != nullptr && value->node != nullptr && value->node->is_number()) ? 1 : 0;
}

LENJSON_API int lenjson_value_is_string(const lenjson_value* value) {
    return (value != nullptr && value->node != nullptr && value->node->is_string()) ? 1 : 0;
}

LENJSON_API int lenjson_value_is_array(const lenjson_value* value) {
    return (value != nullptr && value->node != nullptr && value->node->is_array()) ? 1 : 0;
}

LENJSON_API int lenjson_value_is_object(const lenjson_value* value) {
    return (value != nullptr && value->node != nullptr && value->node->is_object()) ? 1 : 0;
}

// =============================================================================
// 取值
// =============================================================================

LENJSON_API lenjson_status lenjson_value_get_bool(const lenjson_value* value, int* out) {
    return guard(nullptr, [&]() {
        const lenjson::Value& node = require_node(value, "value");
        if (out == nullptr) {
            throw InvalidArgument{"参数为空：out"};
        }
        *out = node.as_bool() ? 1 : 0;
    });
}

LENJSON_API lenjson_status lenjson_value_get_int(const lenjson_value* value, int64_t* out) {
    return guard(nullptr, [&]() {
        const lenjson::Value& node = require_node(value, "value");
        if (out == nullptr) {
            throw InvalidArgument{"参数为空：out"};
        }
        *out = static_cast<int64_t>(node.as_integer());
    });
}

LENJSON_API lenjson_status lenjson_value_get_double(const lenjson_value* value, double* out) {
    return guard(nullptr, [&]() {
        const lenjson::Value& node = require_node(value, "value");
        if (out == nullptr) {
            throw InvalidArgument{"参数为空：out"};
        }
        *out = node.as_real();
    });
}

LENJSON_API const char* lenjson_value_get_string(const lenjson_value* value, size_t* out_length) {
    if (out_length != nullptr) {
        *out_length = 0;
    }

    const char* text = nullptr;
    const lenjson_status rc = guard(nullptr, [&]() {
        const lenjson::Value& node = require_node(value, "value");
        const std::string& content = node.as_string();
        if (out_length != nullptr) {
            *out_length = content.size();
        }
        text = content.c_str();   // 借用：指向值内部的缓冲区，不需要释放
    });
    return rc == LENJSON_OK ? text : nullptr;
}

LENJSON_API char* lenjson_value_get_string_copy(const lenjson_value* value, size_t* out_length) {
    if (out_length != nullptr) {
        *out_length = 0;
    }

    char* text = nullptr;
    const lenjson_status rc = guard(nullptr, [&]() {
        const lenjson::Value& node = require_node(value, "value");
        const std::string& content = node.as_string();

        char* buffer = new char[content.size() + 1];
        std::memcpy(buffer, content.data(), content.size());
        buffer[content.size()] = '\0';

        if (out_length != nullptr) {
            *out_length = content.size();
        }
        text = buffer;
    });
    return rc == LENJSON_OK ? text : nullptr;
}

LENJSON_API size_t lenjson_value_size(const lenjson_value* value) {
    if (value == nullptr || value->node == nullptr) {
        set_error(LENJSON_ERROR_INVALID_ARGUMENT, "参数为空：value");
        return 0;
    }
    clear_error();
    return value->node->size();
}

LENJSON_API int lenjson_value_empty(const lenjson_value* value) {
    if (value == nullptr || value->node == nullptr) {
        set_error(LENJSON_ERROR_INVALID_ARGUMENT, "参数为空：value");
        return 1;
    }
    clear_error();
    return value->node->empty() ? 1 : 0;
}

// =============================================================================
// 访问子节点
// =============================================================================

LENJSON_API lenjson_value* lenjson_value_get_item(const lenjson_value* array, size_t index,
                                                   lenjson_status* status) {
    set_status(status, LENJSON_OK);

    lenjson_value* result = nullptr;
    const lenjson_status rc = guard(status, [&]() {
        lenjson::Value& node = require_node(array, "array");
        if (!node.is_array()) {
            throw lenjson::TypeException("lenjson_value_get_item：目标不是数组");
        }
        lenjson::Value::Array& items = node.as_array();
        if (index >= items.size()) {
            throw lenjson::OutOfRange("数组下标越界: " + std::to_string(index));
        }
        result = make_borrowed(&items[index]);
    });
    return rc == LENJSON_OK ? result : nullptr;
}

LENJSON_API lenjson_value* lenjson_value_get_member(const lenjson_value* object, const char* key,
                                                     lenjson_status* status) {
    set_status(status, LENJSON_OK);

    lenjson_value* result = nullptr;
    const lenjson_status rc = guard(status, [&]() {
        lenjson::Value& node = require_node(object, "object");
        if (key == nullptr) {
            throw InvalidArgument{"参数为空：key"};
        }
        if (!node.is_object()) {
            throw lenjson::TypeException("lenjson_value_get_member：目标不是对象");
        }
        lenjson::Value* found = node.find(key);
        if (found == nullptr) {
            throw lenjson::OutOfRange("对象中不存在键: " + std::string(key));
        }
        result = make_borrowed(found);
    });
    return rc == LENJSON_OK ? result : nullptr;
}

LENJSON_API int lenjson_value_has_member(const lenjson_value* object, const char* key) {
    if (object == nullptr || object->node == nullptr || key == nullptr) {
        set_error(LENJSON_ERROR_INVALID_ARGUMENT, "参数为空");
        return 0;
    }
    clear_error();
    return object->node->contains(key) ? 1 : 0;
}

// =============================================================================
// 修改
// =============================================================================

LENJSON_API lenjson_status lenjson_value_set_item(lenjson_value* array, size_t index,
                                                   lenjson_value* child) {
    return guard(nullptr, [&]() {
        lenjson::Value& node = require_node(array, "array");
        lenjson::Value& source = require_transferable(child, "child");

        if (!node.is_array() && !node.is_null()) {
            throw lenjson::TypeException("lenjson_value_set_item：目标不是数组");
        }
        lenjson::Value::Array& items = node.as_array();
        if (index >= items.size()) {
            items.resize(index + 1);   // 中间空位填 null
        }
        items[index] = std::move(source);
        // child 的节点被搬空了（移动后是一个空值），但内存本身还在，
        // 调用方照常释放它即可，不会泄漏、也不会二次释放。
    });
}

LENJSON_API lenjson_status lenjson_value_set_member(lenjson_value* object, const char* key,
                                                     lenjson_value* child) {
    return guard(nullptr, [&]() {
        lenjson::Value& node = require_node(object, "object");
        lenjson::Value& source = require_transferable(child, "child");

        if (key == nullptr) {
            throw InvalidArgument{"参数为空：key"};
        }
        if (!node.is_object() && !node.is_null()) {
            throw lenjson::TypeException("lenjson_value_set_member：目标不是对象");
        }

        lenjson::Value::Object& members = node.as_object();
        for (auto& member : members) {
            if (member.first == key) {
                member.second = std::move(source);
                return;
            }
        }
        members.emplace_back(std::string(key), std::move(source));
    });
}

LENJSON_API lenjson_status lenjson_value_push_back(lenjson_value* array, lenjson_value* child) {
    return guard(nullptr, [&]() {
        lenjson::Value& node = require_node(array, "array");
        lenjson::Value& source = require_transferable(child, "child");

        if (!node.is_array() && !node.is_null()) {
            throw lenjson::TypeException("lenjson_value_push_back：目标不是数组");
        }
        node.as_array().push_back(std::move(source));
    });
}

LENJSON_API lenjson_status lenjson_value_erase_member(lenjson_value* object, const char* key) {
    return guard(nullptr, [&]() {
        lenjson::Value& node = require_node(object, "object");
        if (key == nullptr) {
            throw InvalidArgument{"参数为空：key"};
        }
        if (!node.is_object()) {
            throw lenjson::TypeException("lenjson_value_erase_member：目标不是对象");
        }
        if (!node.erase(key)) {
            throw lenjson::OutOfRange("对象中不存在键: " + std::string(key));
        }
    });
}

LENJSON_API lenjson_status lenjson_value_erase_item(lenjson_value* array, size_t index) {
    return guard(nullptr, [&]() {
        lenjson::Value& node = require_node(array, "array");
        if (!node.is_array()) {
            throw lenjson::TypeException("lenjson_value_erase_item：目标不是数组");
        }
        node.erase(index);   // 越界时内部抛 OutOfRange
    });
}

LENJSON_API void lenjson_value_clear(lenjson_value* value) {
    guard(nullptr, [&]() {
        lenjson::Value& node = require_node(value, "value");
        node.clear();
    });
}

// =============================================================================
// 比较
// =============================================================================

LENJSON_API int lenjson_value_equal(const lenjson_value* a, const lenjson_value* b) {
    if (a == nullptr || a->node == nullptr || b == nullptr || b->node == nullptr) {
        set_error(LENJSON_ERROR_INVALID_ARGUMENT, "参数为空");
        return 0;
    }
    clear_error();
    return (*a->node == *b->node) ? 1 : 0;
}

// =============================================================================
// 错误信息
// =============================================================================

LENJSON_API const char* lenjson_last_error_message(void) {
    return g_error.message.c_str();
}

LENJSON_API lenjson_error_info lenjson_last_error_info(void) {
    return g_error.info;
}
