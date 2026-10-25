// =============================================================================
// json_value_access.cpp —— Value 的“元素访问 / 增删改 / 比较 / 序列化转发”
// -----------------------------------------------------------------------------
// 与 json_value.cpp 的分工：这里只管“拿到值之后怎么用”，
// 不涉及对象生命周期与类型系统本身。拆成两个文件能让每个文件都短小清晰。
// =============================================================================

#include "lenjson/json_value.h"

#include <ostream>
#include <utility>

#include "lenjson/json_parser.h"
#include "lenjson/json_writer.h"

namespace lenjson {

// =============================================================================
// 容器访问
// =============================================================================
// 设计取舍：非 const 的 as_array()/as_object() 在遇到 null 时会“就地变身”，
// 让 Value v; v["a"] = 1; 这样的构建写法用起来很顺手；
// 但如果当前已经是“别的类型”，说明调用方逻辑有问题，果断抛异常。

Value::Array& Value::as_array() {
    if (auto p = std::get_if<ArrayPtr>(&data_)) {
        if (!*p) *p = std::make_unique<Array>();   // 兼容被移动过的空壳
        return **p;
    }
    if (is_null()) {
        data_ = std::make_unique<Array>();
        return *std::get<ArrayPtr>(data_);
    }
    throw TypeException(std::string("as_array：当前类型是 ") + to_string(type()) + "，不是数组");
}

const Value::Array& Value::as_array() const {
    if (auto p = std::get_if<ArrayPtr>(&data_)) {
        if (*p) return **p;
    }
    throw TypeException(std::string("as_array：当前类型是 ") + to_string(type()) + "，不是数组");
}

Value::Object& Value::as_object() {
    if (auto p = std::get_if<ObjectPtr>(&data_)) {
        if (!*p) *p = std::make_unique<Object>();
        return **p;
    }
    if (is_null()) {
        data_ = std::make_unique<Object>();
        return *std::get<ObjectPtr>(data_);
    }
    throw TypeException(std::string("as_object：当前类型是 ") + to_string(type()) + "，不是对象");
}

const Value::Object& Value::as_object() const {
    if (auto p = std::get_if<ObjectPtr>(&data_)) {
        if (*p) return **p;
    }
    throw TypeException(std::string("as_object：当前类型是 ") + to_string(type()) + "，不是对象");
}

// =============================================================================
// 元素访问
// =============================================================================

Value& Value::operator[](std::size_t index) {
    Array& array = as_array();
    if (index >= array.size()) {
        array.resize(index + 1);   // 中间空位自动填充为 null
    }
    return array[index];
}

const Value& Value::operator[](std::size_t index) const {
    const Array& array = as_array();
    if (index >= array.size()) {
        throw OutOfRange("数组下标越界: " + std::to_string(index));
    }
    return array[index];
}

Value& Value::operator[](const std::string& key) {
    Object& object = as_object();
    // 线性查找：保持插入顺序的代价。日常体量足够；若键极多可另建哈希索引。
    for (auto& kv : object) {
        if (kv.first == key) return kv.second;
    }
    object.emplace_back(key, Value());
    return object.back().second;
}

const Value& Value::operator[](const std::string& key) const {
    const Value* found = find(key);
    if (!found) throw OutOfRange("对象中不存在键: " + key);
    return *found;
}

Value& Value::at(std::size_t index) {
    Array& array = as_array();
    if (index >= array.size()) {
        throw OutOfRange("数组下标越界: " + std::to_string(index));
    }
    return array[index];
}

const Value& Value::at(std::size_t index) const {
    const Array& array = as_array();
    if (index >= array.size()) {
        throw OutOfRange("数组下标越界: " + std::to_string(index));
    }
    return array[index];
}

Value& Value::at(const std::string& key) {
    Value* found = find(key);
    if (!found) throw OutOfRange("对象中不存在键: " + key);
    return *found;
}

const Value& Value::at(const std::string& key) const {
    const Value* found = find(key);
    if (!found) throw OutOfRange("对象中不存在键: " + key);
    return *found;
}

// =============================================================================
// 增删改
// =============================================================================

void Value::push_back(Value value) {
    as_array().push_back(std::move(value));
}

Value* Value::find(const std::string& key) noexcept {
    if (auto p = std::get_if<ObjectPtr>(&data_)) {
        if (*p) {
            for (auto& kv : **p) {
                if (kv.first == key) return &kv.second;
            }
        }
    }
    return nullptr;
}

const Value* Value::find(const std::string& key) const noexcept {
    if (auto p = std::get_if<ObjectPtr>(&data_)) {
        if (*p) {
            for (const auto& kv : **p) {
                if (kv.first == key) return &kv.second;
            }
        }
    }
    return nullptr;
}

bool Value::contains(const std::string& key) const noexcept {
    return find(key) != nullptr;
}

bool Value::erase(const std::string& key) {
    if (auto p = std::get_if<ObjectPtr>(&data_)) {
        if (*p) {
            Object& object = **p;
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (it->first == key) {
                    object.erase(it);
                    return true;
                }
            }
        }
    }
    return false;
}

void Value::erase(std::size_t index) {
    Array& array = as_array();
    if (index >= array.size()) {
        throw OutOfRange("数组下标越界: " + std::to_string(index));
    }
    array.erase(array.begin() + static_cast<std::ptrdiff_t>(index));
}

std::size_t Value::size() const noexcept {
    if (auto p = std::get_if<ArrayPtr>(&data_))  return *p ? (**p).size() : 0;
    if (auto p = std::get_if<ObjectPtr>(&data_)) return *p ? (**p).size() : 0;
    if (auto s = std::get_if<std::string>(&data_)) return s->size();
    return 0;
}

bool Value::empty() const noexcept {
    return size() == 0;
}

void Value::clear() {
    if (auto p = std::get_if<ArrayPtr>(&data_)) {
        if (*p) (*p)->clear();
        return;
    }
    if (auto p = std::get_if<ObjectPtr>(&data_)) {
        if (*p) (*p)->clear();
        return;
    }
    data_ = nullptr;   // 标量清空后退化为 null
}

// =============================================================================
// 比较与交换
// =============================================================================

bool Value::operator==(const Value& other) const {
    // 数字：整数之间精确比较；只要出现浮点，就按数值比较。
    // 这样 Value(1) == Value(1.0) 成立，往返测试也更省心。
    if (is_integer() && other.is_integer()) {
        return as_integer() == other.as_integer();
    }
    if (is_number() && other.is_number()) {
        return as_real() == other.as_real();
    }
    if (type() != other.type()) {
        return false;
    }
    switch (type()) {
        case JsonType::Null:
            return true;
        case JsonType::Boolean:
            return as_bool() == other.as_bool();
        case JsonType::String:
            return as_string() == other.as_string();
        case JsonType::Array: {
            const Array& a = as_array();
            const Array& b = other.as_array();
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (!(a[i] == b[i])) return false;
            }
            return true;
        }
        case JsonType::Object: {
            const Object& a = as_object();
            const Object& b = other.as_object();
            if (a.size() != b.size()) return false;
            // 对象按“键集合”比较，与插入顺序无关。
            for (const auto& kv : a) {
                const Value* v = other.find(kv.first);
                if (v == nullptr || !(*v == kv.second)) return false;
            }
            return true;
        }
        default:
            return false;   // Integer/Real 已在上方处理
    }
}

void Value::swap(Value& other) noexcept {
    data_.swap(other.data_);
}

void swap(Value& a, Value& b) noexcept {
    a.swap(b);
}

// =============================================================================
// 序列化 / 反序列化（转发到对应模块）
// =============================================================================

std::string Value::dump(int indent, bool ensure_ascii) const {
    DumpOptions options;
    options.indent = indent;
    options.ensure_ascii = ensure_ascii;
    return Writer::write(*this, options);
}

Value Value::parse(const std::string& text) {
    return Parser::parse(text);
}

std::string dump(const Value& value, int indent, bool ensure_ascii) {
    return value.dump(indent, ensure_ascii);
}

Value parse(std::string_view text) {
    return Parser::parse(std::string(text));
}

std::ostream& operator<<(std::ostream& os, const Value& value) {
    os << value.dump();
    return os;
}

} // namespace lenjson
