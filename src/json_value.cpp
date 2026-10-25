// =============================================================================
// json_value.cpp —— Value 的“生命周期 + 类型 + 取值”部分
// -----------------------------------------------------------------------------
// 本文件负责：
//   * 各类构造函数
//   * Rule of Five（含深拷贝 clone）
//   * 类型查询与类型转换
// “元素访问 / 增删改 / 比较 / 序列化转发”放在 json_value_access.cpp，
// 这样两个文件各自职责单一、体量都控制在 600 行以内。
// =============================================================================

#include "lenjson/json_value.h"

#include <utility>

namespace lenjson {

// 类型枚举 -> 可读名字。
const char* to_string(JsonType type) noexcept {
    switch (type) {
        case JsonType::Null:    return "null";
        case JsonType::Boolean: return "boolean";
        case JsonType::Integer: return "integer";
        case JsonType::Real:    return "real";
        case JsonType::String:  return "string";
        case JsonType::Array:   return "array";
        case JsonType::Object:  return "object";
    }
    return "unknown";
}

// =============================================================================
// 构造函数
// =============================================================================
// 每个构造函数都只是一次对 data_(variant) 的赋值，variant 会自动记录
// “当前是哪个备选类型”，这正是它相比裸 union 省心的地方。

Value::Value() noexcept : data_(nullptr) {}
Value::Value(std::nullptr_t) noexcept : data_(nullptr) {}
Value::Value(bool value) noexcept : data_(value) {}
Value::Value(double value) noexcept : data_(value) {}

Value::Value(const char* text)
    : data_(std::string(text != nullptr ? text : "")) {}

Value::Value(const std::string& text) : data_(text) {}
Value::Value(std::string&& text) noexcept : data_(std::move(text)) {}
Value::Value(std::string_view text) : data_(std::string(text)) {}

// 数组/对象：在堆上 new 一份，再交给 unique_ptr 管理（RAII）。
Value::Value(const Array& array)
    : data_(std::make_unique<Array>(array)) {}
Value::Value(Array&& array) noexcept
    : data_(std::make_unique<Array>(std::move(array))) {}
Value::Value(const Object& object)
    : data_(std::make_unique<Object>(object)) {}
Value::Value(Object&& object) noexcept
    : data_(std::make_unique<Object>(std::move(object))) {}

// {1,2,3} => 数组。交给 vector 的 initializer_list 构造函数即可。
Value::Value(std::initializer_list<Value> items)
    : data_(std::make_unique<Array>(items)) {}

// =============================================================================
// Rule of Five
// =============================================================================

// 拷贝构造 = 深拷贝。这是“值语义”的关键：两个 Value 完全独立，
// 修改其中一个不会影响另一个（与 int 的行为一致）。
Value::Value(const Value& other) {
    copy_from(other);
}

// 移动构造 = 窃取对方的内部指针（O(1)）。被移动的对象要重置成 null，
// 否则它会留下一个“类型是数组/对象、但内部指针为空”的坏状态。
Value::Value(Value&& other) noexcept : data_(std::move(other.data_)) {
    other.data_ = nullptr;
}

// 拷贝赋值：copy-and-swap 惯用法。
// 好处是天然自赋值安全，且当拷贝抛出异常时不会破坏 *this 的原有内容。
Value& Value::operator=(const Value& other) {
    if (this != &other) {
        Value temp(other);   // 先拷贝一份（可能抛异常，此时 *this 不变）
        swap(temp);          // 交换后，temp 析构掉旧内容
    }
    return *this;
}

Value& Value::operator=(Value&& other) noexcept {
    if (this != &other) {
        data_ = std::move(other.data_);
        other.data_ = nullptr;
    }
    return *this;
}

Value::~Value() = default;

// 深拷贝的具体实现：按类型分派，容器类型递归拷贝整棵子树。
void Value::copy_from(const Value& other) {
    switch (other.type()) {
        case JsonType::Null:
            data_ = nullptr;
            break;
        case JsonType::Boolean:
            data_ = std::get<bool>(other.data_);
            break;
        case JsonType::Integer:
            data_ = std::get<long long>(other.data_);
            break;
        case JsonType::Real:
            data_ = std::get<double>(other.data_);
            break;
        case JsonType::String:
            data_ = std::get<std::string>(other.data_);
            break;
        case JsonType::Array: {
            const auto& ptr = std::get<ArrayPtr>(other.data_);
            data_ = ptr ? std::make_unique<Array>(*ptr) : ArrayPtr{};
            break;
        }
        case JsonType::Object: {
            const auto& ptr = std::get<ObjectPtr>(other.data_);
            data_ = ptr ? std::make_unique<Object>(*ptr) : ObjectPtr{};
            break;
        }
    }
}

// =============================================================================
// 类型查询
// =============================================================================
// variant 的备选顺序与 JsonType 严格对应，因此 index() 可直接转型。

JsonType Value::type() const noexcept {
    return static_cast<JsonType>(data_.index());
}

bool Value::is_null()    const noexcept { return std::holds_alternative<std::nullptr_t>(data_); }
bool Value::is_bool()    const noexcept { return std::holds_alternative<bool>(data_); }
bool Value::is_integer() const noexcept { return std::holds_alternative<long long>(data_); }
bool Value::is_real()    const noexcept { return std::holds_alternative<double>(data_); }
bool Value::is_number()  const noexcept { return is_integer() || is_real(); }
bool Value::is_string()  const noexcept { return std::holds_alternative<std::string>(data_); }
bool Value::is_array()   const noexcept { return std::holds_alternative<ArrayPtr>(data_); }
bool Value::is_object()  const noexcept { return std::holds_alternative<ObjectPtr>(data_); }

// =============================================================================
// 取值
// =============================================================================

bool Value::as_bool() const {
    if (auto p = std::get_if<bool>(&data_)) return *p;
    throw TypeException(std::string("as_bool：当前类型是 ") + to_string(type()) + "，不是布尔");
}

// 数字统一按“数值”处理：整数直接返回，浮点截断取整（向零取整）。
long long Value::as_integer() const {
    if (auto p = std::get_if<long long>(&data_)) return *p;
    if (auto d = std::get_if<double>(&data_))    return static_cast<long long>(*d);
    throw TypeException(std::string("as_integer：当前类型是 ") + to_string(type()) + "，不是数字");
}

double Value::as_real() const {
    if (auto d = std::get_if<double>(&data_))    return *d;
    if (auto p = std::get_if<long long>(&data_)) return static_cast<double>(*p);
    throw TypeException(std::string("as_real：当前类型是 ") + to_string(type()) + "，不是数字");
}

const std::string& Value::as_string() const {
    if (auto p = std::get_if<std::string>(&data_)) return *p;
    throw TypeException(std::string("as_string：当前类型是 ") + to_string(type()) + "，不是字符串");
}

double Value::as_number() const {
    return as_real();
}

} // namespace lenjson
