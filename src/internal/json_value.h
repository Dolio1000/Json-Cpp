#ifndef LENJSON_JSON_VALUE_H
#define LENJSON_JSON_VALUE_H

// =============================================================================
// json_value.h —— JSON 值的核心类型 Value
// -----------------------------------------------------------------------------
// 这是整个库的“心脏”。JSON 允许的值有 6 种：
//     null、布尔、数字、字符串、数组、对象。
// 一个 C++ 变量要在运行期承载这 6 种之一，就需要一个“动态类型”。
//
// ★ 核心思路：带标签的联合体（tagged union）
//   最朴素的实现是手写一个 union：
//       struct Storage {
//           bool        b;
//           long long   i;
//           double      d;
//           std::string s;          // 非平凡类型
//           Array*      a;          // 递归类型
//           Object*     o;
//       };
//   但 union 不会帮我们调用构造/析构，非平凡成员必须用 placement new 手动
//   构造、手动析构，还要自己维护“当前是哪种类型”的标签并实现拷贝/移动。
//   C++17 的 std::variant 正是把这套易错的样板封装好了，因此这里直接用它。
//   （想深入原理，可以试着用上面的 union 手写一份等价的 Storage 作为练习。）
//
// ★ 核心思路：递归类型为什么必须用指针
//   Value 内部要能装 Array（元素又是 Value），这是一个“递归定义”。
//   如果直接在 Value 里放 std::vector<Value>，则在 Value 还没定义完时就要
//   实例化 std::vector<Value>，属于“不完整类型递归”，标准并不保证可移植
//   （MSVC 与 GCC 的行为就有差异）。解决办法是加一层“间接层”——用指针：
//   unique_ptr<Array> 的点位大小是固定的（与 Array 是否完整无关），
//   于是 Value 的定义得以自洽。nlohmann/json 等主流库也是同样的思路。
//
// ★ 核心思路：值语义
//   JSON 值应当像 int 一样可以自由拷贝，而不是像 shared_ptr 那样共享。
//   由于 unique_ptr 不可拷贝，我们手动实现拷贝构造 = 深拷贝整棵树（clone），
//   这就是 Rule of Five 的典型应用场景。
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "json_error.h"

namespace lenjson {

// JSON 的 6 种类型，外加“整数/浮点”细分。
// 下层实现保证：这个枚举的取值顺序与 Value 内部 variant 的备选顺序一一对应，
// 因此 type() 可以极快地用 variant::index() 得到。
enum class JsonType : std::uint8_t {
    Null = 0,
    Boolean,
    Integer,
    Real,
    String,
    Array,
    Object
};

// 把类型枚举转成人类可读的名字，主要用于拼装错误信息。
const char* to_string(JsonType type) noexcept;

// -----------------------------------------------------------------------------
// Value —— 一个可以存放任意 JSON 值的动态类型。
// -----------------------------------------------------------------------------
// 本类是内部实现，不对外导出：C 接口通过不透明句柄 lenjson_value 访问它。
// 这样 DLL 的导出表里只有 C 符号，g++ 编译的库才能被 MSVC 安全调用。
class Value {
public:
    // 数组：顺序容器，元素仍是 Value（递归）。
    using Array = std::vector<Value>;
    // 对象：为“保持插入顺序”，用“键值对的有序向量”而不是 std::map。
    // 代价是按键查找为 O(n)；对日常体量的 JSON 完全够用，换来的是
    // 序列化输出顺序与解析时一致（std::map 会按字典序打乱顺序）。
    using Object = std::vector<std::pair<std::string, Value>>;

private:
    using ArrayPtr = std::unique_ptr<Array>;    // 递归类型的间接层
    using ObjectPtr = std::unique_ptr<Object>;

    // 真正存放数据的地方。备选顺序必须与 JsonType 保持一致（见上方说明）。
    using Storage = std::variant<std::nullptr_t, bool, long long, double,
                                 std::string, ArrayPtr, ObjectPtr>;

public:
    // ===================== 构造函数 =====================
    Value() noexcept;                                   // 默认构造为 null
    Value(std::nullptr_t) noexcept;                     // Value(nullptr)
    Value(bool value) noexcept;                         // 布尔

    // 整数：用模板承接所有整型（int/short/long/unsigned…），统一存成 long long。
    // 之所以不直接写 Value(int)/Value(long long) 多个重载，是因为那样
    // Value(5) 会在 int、long long、double 之间产生二义性（都是“转换”级别）。
    template <typename Int,
              typename = std::enable_if_t<std::is_integral<Int>::value &&
                                          !std::is_same<Int, bool>::value>>
    Value(Int value) noexcept : data_(static_cast<long long>(value)) {}

    Value(double value) noexcept;                       // 浮点

    // 字符串：覆盖 C 字符串、std::string、std::string_view 三种常见来源。
    Value(const char* text);
    Value(const std::string& text);
    Value(std::string&& text) noexcept;
    Value(std::string_view text);

    // 容器
    Value(const Array& array);
    Value(Array&& array) noexcept;
    Value(const Object& object);
    Value(Object&& object) noexcept;

    // {1, 2, 3} 会被当成数组构造。
    // 注意：对象请用 Value::Object{ {"k", v}, ... } 或 v["k"] = x 来构造。
    // 不用“花括号对象字面量”，是为了避免 {"k", v} 既可解释成“两元素数组”、
    // 又可解释成“一个键值对”而产生的二义性——这是很多 JSON 库的坑。
    Value(std::initializer_list<Value> items);

    // ===================== Rule of Five =====================
    // 因为 unique_ptr 不可拷贝，编译器默认生成的拷贝会被删除，
    // 这里必须手写“深拷贝”以维持值语义。
    Value(const Value& other);                  // 深拷贝
    Value(Value&& other) noexcept;              // 窃取内部指针
    Value& operator=(const Value& other);       // 拷贝赋值（copy-and-swap）
    Value& operator=(Value&& other) noexcept;   // 移动赋值
    ~Value();

    // ===================== 类型查询 =====================
    JsonType type() const noexcept;
    bool is_null()    const noexcept;
    bool is_bool()    const noexcept;
    bool is_integer() const noexcept;
    bool is_real()    const noexcept;
    bool is_number()  const noexcept;    // 整数或浮点
    bool is_string()  const noexcept;
    bool is_array()   const noexcept;
    bool is_object()  const noexcept;

    // ===================== 取值（类型不符抛 TypeException）=====================
    bool               as_bool() const;
    long long          as_integer() const;   // 浮点会被截断取整
    double             as_real() const;      // 整数会提升为浮点
    const std::string& as_string() const;
    double             as_number() const;    // 数值统一取 double

    // 模板取值：get<int>() / get<double>() / get<bool>() / get<std::string>()。
    template <typename T> T get() const;
    // 带默认值：类型不符时不抛异常，返回 fallback。
    template <typename T> T get_or(const T& fallback) const;

    // ===================== 容器访问 =====================
    // 非 const 版本：若当前是 null，会自动“变身”为空数组/空对象，方便就地构建。
    Array&        as_array();
    const Array&  as_array() const;
    Object&       as_object();
    const Object& as_object() const;

    // ===================== 元素访问 =====================
    // 数组下标：null 自动转为数组；下标越界时自动扩容（便于构建）。
    // 越界插入会以 null 填充中间空位。
    Value&       operator[](std::size_t index);
    const Value& operator[](std::size_t index) const;   // const：越界抛 OutOfRange

    // 对象下标：null 自动转为对象；键不存在则插入一个 null。
    Value&       operator[](const std::string& key);
    const Value& operator[](const std::string& key) const;  // const：无键抛 OutOfRange

    // at：带边界检查的访问，越界 / 无键抛 OutOfRange。
    Value&       at(std::size_t index);
    const Value& at(std::size_t index) const;
    Value&       at(const std::string& key);
    const Value& at(const std::string& key) const;

    // 便捷修改
    void push_back(Value value);                 // 追加数组元素
    Value*       find(const std::string& key) noexcept;        // 返回指针，找不到为 nullptr
    const Value* find(const std::string& key) const noexcept;
    bool contains(const std::string& key) const noexcept;      // 对象是否含某键
    bool erase(const std::string& key);                        // 删除对象键，返回是否删掉
    void erase(std::size_t index);                             // 删除数组元素

    std::size_t size() const noexcept;   // 数组/对象元素个数；字符串长度；其它为 0
    bool empty() const noexcept;
    void clear();                        // 清空内容：容器清空元素，标量变为 null

    // ===================== 比较与交换 =====================
    // 语义相等：对象按“键集合”比较（与顺序无关）；整数与浮点按数值比较。
    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const { return !(*this == other); }
    void swap(Value& other) noexcept;

    // ===================== 序列化 / 反序列化 =====================
    std::string dump(int indent = -1, bool ensure_ascii = false) const;
    static Value parse(const std::string& text);

private:
    // 深拷贝的具体实现（供拷贝构造复用）。
    void copy_from(const Value& other);

    Storage data_;
};

// -----------------------------------------------------------------------------
// 便捷的自由函数（与成员函数等价，写起来更顺手）
// -----------------------------------------------------------------------------
void swap(Value& a, Value& b) noexcept;
Value parse(std::string_view text);
std::string dump(const Value& value, int indent = -1, bool ensure_ascii = false);
std::ostream& operator<<(std::ostream& os, const Value& value);

// =============================================================================
// 模板成员的定义（必须放在头文件里，才能被各编译单元实例化）
// =============================================================================

// 通用取值：按目标类型分派到底层访问函数。if constexpr 让分支在编译期裁剪，
// 不会为不相关的类型生成代码（这是 C++17 对比 C++11 SFINAE 的显著改进）。
template <typename T>
T Value::get() const {
    if constexpr (std::is_same<T, bool>::value) {
        return as_bool();
    } else if constexpr (std::is_same<T, std::string>::value) {
        return as_string();
    } else if constexpr (std::is_integral<T>::value) {
        return static_cast<T>(as_integer());
    } else if constexpr (std::is_floating_point<T>::value) {
        return static_cast<T>(as_real());
    } else {
        // 依赖型表达式：只有真正实例化到这个分支时才会触发断言。
        static_assert(sizeof(T) == 0,
                      "Value::get<T> 只支持 bool / 整数 / 浮点 / std::string");
    }
}

// 带默认值的取值：内部直接用 try/catch 实现，简单直观。
template <typename T>
T Value::get_or(const T& fallback) const {
    try {
        return get<T>();
    } catch (const JsonException&) {
        return fallback;
    }
}

} // namespace lenjson

#endif // LENJSON_JSON_VALUE_H
