// =============================================================================
// test_lenjson.cpp —— 内部 C++ 实现的单元测试
// -----------------------------------------------------------------------------
// 不依赖任何测试框架，用最简单的宏统计通过/失败数量，方便在任意平台上直接运行。
// 覆盖：解析、往返一致、数字精度、Unicode/转义、访问与修改、比较、错误定位。
//
// ★ 本文件测的是“内部实现”，因此直接 include 了 src/internal/ 下的头文件，
//   并且链接的是静态库——动态库刻意不导出 C++ 符号，那正是跨编译器安全的代价。
//   对外接口本身由 test_lenjson_c.c（纯 C）覆盖。
// =============================================================================

#include <iostream>
#include <string>

#include "internal/json_error.h"
#include "internal/json_parser.h"
#include "internal/json_value.h"
#include "internal/json_writer.h"

// ----------------------------- 极简测试框架 -----------------------------
static int g_total = 0;
static int g_failed = 0;

#define CHECK(expr)                                                             \
    do {                                                                        \
        ++g_total;                                                              \
        if (!(expr)) {                                                          \
            ++g_failed;                                                         \
            std::cout << "  [失败] " << #expr << "  (" << __LINE__ << " 行)\n"; \
        }                                                                       \
    } while (0)

#define CHECK_EQ(actual, expected)                                              \
    do {                                                                        \
        ++g_total;                                                              \
        if (!((actual) == (expected))) {                                        \
            ++g_failed;                                                         \
            std::cout << "  [失败] " << #actual << " == " << #expected          \
                      << "  (" << __LINE__ << " 行)\n";                         \
        }                                                                       \
    } while (0)

#define CHECK_THROWS(expr, ExceptionType)                                       \
    do {                                                                        \
        ++g_total;                                                              \
        bool threw = false;                                                     \
        try {                                                                   \
            (void)(expr);                                                       \
        } catch (const ExceptionType&) {                                        \
            threw = true;                                                       \
        } catch (...) {                                                         \
        }                                                                       \
        if (!threw) {                                                           \
            ++g_failed;                                                         \
            std::cout << "  [失败] 未抛出 " << #ExceptionType << ": " << #expr  \
                      << "  (" << __LINE__ << " 行)\n";                         \
        }                                                                       \
    } while (0)

namespace {
using lenjson::JsonType;
using lenjson::Value;

// 以显式字节构造 UTF-8 文本，避免不同编译器/源文件编码带来的歧义。
const std::string kZhongWen = "\xE4\xB8\xAD\xE6\x96\x87";          // “中文”
const std::string kClef     = "\xF0\x9D\x84\x9E";                   // U+1D11E（𝄞）

void test_parse_basic() {
    std::cout << "test_parse_basic\n";
    CHECK(lenjson::parse("null").is_null());
    CHECK_EQ(lenjson::parse("true").as_bool(), true);
    CHECK_EQ(lenjson::parse("false").as_bool(), false);
    CHECK_EQ(lenjson::parse("42").get<int>(), 42);
    CHECK_EQ(lenjson::parse("-7").get<int>(), -7);
    CHECK_EQ(lenjson::parse("  \"hi\"  ").as_string(), std::string("hi"));  // 前后空白
    CHECK(lenjson::parse("[]").is_array());
    CHECK_EQ(lenjson::parse("[]").size(), std::size_t(0));
    CHECK(lenjson::parse("{}").is_object());
    CHECK_EQ(lenjson::parse("{}").size(), std::size_t(0));
}

void test_parse_numbers() {
    std::cout << "test_parse_numbers\n";

    // 整数保持精确
    const char* big = "9223372036854775807";                 // long long 最大值
    CHECK(lenjson::parse(big).is_integer());
    CHECK_EQ(lenjson::parse(big).as_integer(), 9223372036854775807LL);

    // 超出 long long 范围 -> 退化为 double
    CHECK(lenjson::parse("999999999999999999999999999").is_real());

    // 浮点
    CHECK(lenjson::parse("3.14").is_real());
    CHECK_EQ(lenjson::parse("3.14").as_real(), 3.14);
    CHECK(lenjson::parse("1e3").is_real());
    CHECK_EQ(lenjson::parse("1e3").as_real(), 1000.0);
    CHECK_EQ(lenjson::parse("-2.5E-2").as_real(), -0.025);

    // 非法数字
    CHECK_THROWS(lenjson::parse("012"), lenjson::ParseError);     // 前导 0
    CHECK_THROWS(lenjson::parse("1."), lenjson::ParseError);      // 小数点后无数字
    CHECK_THROWS(lenjson::parse("1e"), lenjson::ParseError);      // 指数无数字
    CHECK_THROWS(lenjson::parse("-"), lenjson::ParseError);       // 只有负号
}

void test_parse_strings() {
    std::cout << "test_parse_strings\n";

    // 各种转义
    CHECK_EQ(lenjson::parse("\"a\\nb\"").as_string(), std::string("a\nb"));
    CHECK_EQ(lenjson::parse("\"tab\\tend\"").as_string(), std::string("tab\tend"));
    CHECK_EQ(lenjson::parse("\"quote\\\"end\"").as_string(), std::string("quote\"end"));
    CHECK_EQ(lenjson::parse("\"back\\\\slash\"").as_string(), std::string("back\\slash"));
    CHECK_EQ(lenjson::parse("\"slash\\/ok\"").as_string(), std::string("slash/ok"));

    // 非法转义 / 未闭合 / 控制字符
    CHECK_THROWS(lenjson::parse("\"bad\\x\""), lenjson::ParseError);
    CHECK_THROWS(lenjson::parse("\"unterminated"), lenjson::ParseError);
    CHECK_THROWS(lenjson::parse("\"line\nbreak\""), lenjson::ParseError);   // 裸换行
}

void test_unicode() {
    std::cout << "test_unicode\n";

    // \uXXXX -> UTF-8
    CHECK_EQ(lenjson::parse("\"\\u4e2d\\u6587\"").as_string(), kZhongWen);

    // 代理对 -> BMP 之外的码点
    CHECK_EQ(lenjson::parse("\"\\uD834\\uDD1E\"").as_string(), kClef);

    // 孤立代理项应当报错
    CHECK_THROWS(lenjson::parse("\"\\uD834\""), lenjson::ParseError);
    CHECK_THROWS(lenjson::parse("\"\\uDD1E\""), lenjson::ParseError);

    // \uXXXX 的大小写都要支持
    CHECK_EQ(lenjson::parse("\"\\u0041\"").as_string(), std::string("A"));
    CHECK_EQ(lenjson::parse("\"\\u00e9\"").as_string(), std::string("\xC3\xA9"));  // é
}

void test_serialize() {
    std::cout << "test_serialize\n";

    // 转义输出
    Value s = std::string("a\"b\\c\n");
    CHECK_EQ(s.dump(), std::string("\"a\\\"b\\\\c\\n\""));

    // 控制字符 -> \u00XX
    Value ctrl = std::string("\x01");
    CHECK_EQ(ctrl.dump(), std::string("\"\\u0001\""));

    // 默认保留 UTF-8
    Value cn = kZhongWen;
    CHECK_EQ(cn.dump(), std::string("\"") + kZhongWen + "\"");

    // ensure_ascii：中文 -> \uXXXX，代理对 -> 两个 \uXXXX
    CHECK_EQ(cn.dump(-1, true), std::string("\"\\u4e2d\\u6587\""));
    CHECK_EQ(Value(kClef).dump(-1, true), std::string("\"\\ud834\\udd1e\""));

    // 浮点：整数形态要补 .0，避免往返后变成整数
    CHECK_EQ(Value(1.0).dump(), std::string("1.0"));
    CHECK_EQ(Value(0.1).dump(), std::string("0.1"));

    // 美化输出
    Value obj;
    obj["a"] = 1;
    obj["b"] = Value::Array{1, 2};
    const std::string pretty = obj.dump(2);
    CHECK_EQ(pretty, std::string("{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2\n  ]\n}"));

    // 空容器
    CHECK_EQ(Value(Value::Array{}).dump(), std::string("[]"));
    CHECK_EQ(Value(Value::Object{}).dump(), std::string("{}"));
}

void test_roundtrip() {
    std::cout << "test_roundtrip\n";

    const char* samples[] = {
        "null",
        "true",
        "12345",
        "3.14159",
        "-0.5",
        "\"hello\"",
        "[]",
        "{}",
        "[1,2,3]",
        "{\"a\":1,\"b\":[true,false,null]}",
        "{\"nested\":{\"x\":{\"y\":[1,2,{\"z\":\"deep\"}]}}}",
    };
    for (const char* sample : samples) {
        Value first = lenjson::parse(sample);
        Value second = lenjson::parse(first.dump());
        CHECK(first == second);
    }
}

void test_access_and_mutation() {
    std::cout << "test_access_and_mutation\n";

    // 数组下标越界自动扩容
    Value arr;
    arr[2] = 7;
    CHECK_EQ(arr.size(), std::size_t(3));
    CHECK(arr[0].is_null());
    CHECK_EQ(arr[2].get<int>(), 7);

    // 对象下标自动创建 / 嵌套创建
    Value obj;
    obj["a"] = 1;
    obj["b"]["c"] = 2;
    CHECK_EQ(obj["a"].get<int>(), 1);
    CHECK_EQ(obj["b"]["c"].get<int>(), 2);
    CHECK(obj.contains("a"));
    CHECK(!obj.contains("zzz"));

    // find 返回指针
    CHECK(obj.find("a") != nullptr);
    CHECK(obj.find("nope") == nullptr);

    // push_back / erase
    obj["list"] = Value::Array{};
    obj["list"].push_back(10);
    obj["list"].push_back(20);
    CHECK_EQ(obj["list"].size(), std::size_t(2));
    obj["list"].erase(std::size_t(0));
    CHECK_EQ(obj["list"][0].get<int>(), 20);
    CHECK(obj.erase("a"));
    CHECK(!obj.erase("a"));    // 再删一次应失败

    // at 越界抛异常
    CHECK_THROWS(obj.at("missing"), lenjson::OutOfRange);
    CHECK_THROWS(arr.at(99), lenjson::OutOfRange);

    // 类型错误
    CHECK_THROWS(Value(5).as_string(), lenjson::TypeException);
    CHECK_THROWS(Value("x").as_integer(), lenjson::TypeException);

    // get_or 默认值
    CHECK_EQ(Value(5).get_or<std::string>("默认"), std::string("默认"));
    CHECK_EQ(Value(5).get_or<int>(-1), 5);

    // size/empty/clear
    Value v = Value::Array{1, 2, 3};
    CHECK(!v.empty());
    v.clear();
    CHECK(v.empty());
    CHECK(v.is_array());       // 清空元素后仍是数组

    // 字符串的 size 是长度
    CHECK_EQ(Value("abcd").size(), std::size_t(4));
}

void test_copy_and_move() {
    std::cout << "test_copy_and_move\n";

    // 深拷贝：副本与原件互不影响
    Value original = lenjson::parse("{\"a\":[1,2,3]}");
    Value copy = original;
    copy["a"][0] = 100;
    CHECK_EQ(original["a"][0].get<int>(), 1);
    CHECK_EQ(copy["a"][0].get<int>(), 100);

    // 移动后原对象变为 null
    Value moved = std::move(original);
    CHECK(original.is_null());
    CHECK_EQ(moved["a"][0].get<int>(), 1);

    // 拷贝赋值（含自赋值）
    Value x = 1;
    Value y = 2;
    y = x;
    CHECK_EQ(y.get<int>(), 1);
    x = x;
    CHECK_EQ(x.get<int>(), 1);
}

void test_comparison_and_order() {
    std::cout << "test_comparison_and_order\n";

    // 数值比较：整数与浮点数值相等即相等
    CHECK(Value(1) == Value(1.0));
    CHECK(Value(2) != Value(3));

    // 对象比较与顺序无关
    Value a = lenjson::parse("{\"x\":1,\"y\":2}");
    Value b = lenjson::parse("{\"y\":2,\"x\":1}");
    CHECK(a == b);

    // 但序列化会保留各自的插入顺序
    CHECK_EQ(a.dump(), std::string("{\"x\":1,\"y\":2}"));
    CHECK_EQ(b.dump(), std::string("{\"y\":2,\"x\":1}"));

    // 重复键：后者覆盖前者，只保留一个键
    Value dup = lenjson::parse("{\"k\":1,\"k\":2}");
    CHECK_EQ(dup.size(), std::size_t(1));
    CHECK_EQ(dup["k"].get<int>(), 2);
}

void test_errors() {
    std::cout << "test_errors\n";

    // 结构错误
    CHECK_THROWS(lenjson::parse("{"), lenjson::ParseError);           // 对象未闭合
    CHECK_THROWS(lenjson::parse("[1,2,"), lenjson::ParseError);       // 数组未闭合
    CHECK_THROWS(lenjson::parse("[1,]"), lenjson::ParseError);        // 尾随逗号
    CHECK_THROWS(lenjson::parse("{a:1}"), lenjson::ParseError);       // 键未加引号
    CHECK_THROWS(lenjson::parse("{\"a\" 1}"), lenjson::ParseError);   // 缺少冒号
    CHECK_THROWS(lenjson::parse("1 2"), lenjson::ParseError);         // 多余内容
    CHECK_THROWS(lenjson::parse(""), lenjson::ParseError);            // 空输入

    // 错误位置：第 1 行第 7 列（"{\"a\": }" 中 '}' 的位置）
    try {
        lenjson::parse("{\"a\": }");
        CHECK(false);
    } catch (const lenjson::ParseError& e) {
        CHECK_EQ(e.line(), std::size_t(1));
        CHECK_EQ(e.column(), std::size_t(7));
    }

    // 多行输入的行号
    try {
        lenjson::parse("{\n  \"a\": 1,\n  \"b\": }\n}");
        CHECK(false);
    } catch (const lenjson::ParseError& e) {
        CHECK_EQ(e.line(), std::size_t(3));
    }

    // 嵌套过深 -> 用 ParseError 而不是崩溃
    std::string deep;
    for (int i = 0; i < 600; ++i) deep += '[';
    for (int i = 0; i < 600; ++i) deep += ']';
    CHECK_THROWS(lenjson::parse(deep), lenjson::ParseError);

    // 兜底：所有异常都可用 JsonException 捕获
    CHECK_THROWS(lenjson::parse("["), lenjson::JsonException);
}

void test_comments_option() {
    std::cout << "test_comments_option\n";

    const std::string text = "{\n  // 行注释\n  \"a\": 1, /* 块注释 */\n  \"b\": 2\n}";

    // 默认不允许注释
    CHECK_THROWS(lenjson::parse(text), lenjson::ParseError);

    // 打开选项后允许
    lenjson::ParseOptions options;
    options.allow_comments = true;
    Value v = lenjson::Parser::parse(text, options);
    CHECK_EQ(v["a"].get<int>(), 1);
    CHECK_EQ(v["b"].get<int>(), 2);
}

} // namespace

int main() {
    test_parse_basic();
    test_parse_numbers();
    test_parse_strings();
    test_unicode();
    test_serialize();
    test_roundtrip();
    test_access_and_mutation();
    test_copy_and_move();
    test_comparison_and_order();
    test_errors();
    test_comments_option();

    std::cout << "\n==============================\n";
    std::cout << "用例总数: " << g_total << "，失败: " << g_failed << "\n";
    if (g_failed == 0) {
        std::cout << "全部通过 ✔\n";
    }
    return g_failed == 0 ? 0 : 1;
}
