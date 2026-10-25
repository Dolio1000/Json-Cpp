/* =============================================================================
 * test_lenjson_c.c —— 对外 C 接口的单元测试
 * -----------------------------------------------------------------------------
 * 这个文件刻意写成纯 C（用 .c 扩展名、由 C 编译器编译），有两个目的：
 *   1. 证明公开头文件确实是 C 接口，没有任何 C++ 语法泄漏；
 *   2. 从调用方的视角检验 C ABI 的行为是否正确。
 *
 * 它链接的是动态库，因此同时验证了“DLL 能被正常加载并调用”。
 * 不依赖任何测试框架，用最简单的宏统计通过/失败数量。
 * ============================================================================= */

#include <stdio.h>
#include <string.h>

#include "lenjson/lenjson.h"

/* ----------------------------- 极简测试框架 ----------------------------- */
static int g_total = 0;
static int g_failed = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        ++g_total;                                                               \
        if (!(expr)) {                                                           \
            ++g_failed;                                                          \
            printf("  [失败] %s  (%d 行)\n", #expr, __LINE__);                   \
        }                                                                        \
    } while (0)

/* 以显式字节构造 UTF-8 文本，避免不同编译器/源文件编码带来的歧义。 */
static const char kZhongWen[] = "\xE4\xB8\xAD\xE6\x96\x87";   /* “中文” */
static const char kXiaoMing[] = "\xE5\xB0\x8F\xE6\x98\x8E";   /* “小明” */
static const char kClef[]     = "\xF0\x9D\x84\x9E";           /* U+1D11E（𝄞） */

/* -------------------------------------------------------------------------- */

static void test_parse_basic(void) {
    printf("test_parse_basic\n");
    lenjson_status st = LENJSON_OK;
    lenjson_value* v = NULL;

    v = lenjson_parse("null", LENJSON_NUL_TERMINATED, &st);
    CHECK(st == LENJSON_OK);
    CHECK(v != NULL);
    CHECK(lenjson_value_is_null(v) == 1);
    CHECK(lenjson_value_type(v) == LENJSON_TYPE_NULL);
    lenjson_value_free(v);

    v = lenjson_parse("true", LENJSON_NUL_TERMINATED, &st);
    CHECK(lenjson_value_is_bool(v) == 1);
    int flag = 0;
    CHECK(lenjson_value_get_bool(v, &flag) == LENJSON_OK);
    CHECK(flag == 1);
    lenjson_value_free(v);

    v = lenjson_parse("42", LENJSON_NUL_TERMINATED, &st);
    int64_t number = 0;
    CHECK(lenjson_value_get_int(v, &number) == LENJSON_OK);
    CHECK(number == 42);
    CHECK(lenjson_value_is_integer(v) == 1);
    lenjson_value_free(v);

    v = lenjson_parse("3.5", LENJSON_NUL_TERMINATED, &st);
    double real = 0.0;
    CHECK(lenjson_value_get_double(v, &real) == LENJSON_OK);
    CHECK(real == 3.5);
    CHECK(lenjson_value_is_real(v) == 1);
    lenjson_value_free(v);

    v = lenjson_parse("\"hi\"", LENJSON_NUL_TERMINATED, &st);
    size_t length = 0;
    const char* text = lenjson_value_get_string(v, &length);
    CHECK(text != NULL);
    CHECK(length == 2);
    CHECK(text != NULL && strcmp(text, "hi") == 0);
    lenjson_value_free(v);

    v = lenjson_parse("[]", LENJSON_NUL_TERMINATED, &st);
    CHECK(lenjson_value_is_array(v) == 1);
    CHECK(lenjson_value_size(v) == 0);
    CHECK(lenjson_value_empty(v) == 1);
    lenjson_value_free(v);

    v = lenjson_parse("{}", LENJSON_NUL_TERMINATED, &st);
    CHECK(lenjson_value_is_object(v) == 1);
    CHECK(lenjson_value_size(v) == 0);
    lenjson_value_free(v);
}

static void test_parse_errors(void) {
    printf("test_parse_errors\n");
    lenjson_status st = LENJSON_OK;

    /* 格式错误：状态码是 PARSE，行列信息可用 */
    lenjson_value* v = lenjson_parse("{bad}", LENJSON_NUL_TERMINATED, &st);
    CHECK(v == NULL);
    CHECK(st == LENJSON_ERROR_PARSE);
    CHECK(strlen(lenjson_last_error_message()) > 0);

    /* 出错位置：第 1 行第 7 列（"{\"a\": }" 中 '}' 的位置） */
    v = lenjson_parse("{\"a\": }", LENJSON_NUL_TERMINATED, &st);
    CHECK(v == NULL);
    CHECK(st == LENJSON_ERROR_PARSE);
    {
        lenjson_error_info info = lenjson_last_error_info();
        CHECK(info.line == 1);
        CHECK(info.column == 7);
        CHECK(info.position == 6);
    }

    /* 多行输入的行号 */
    v = lenjson_parse("{\n  \"a\": 1,\n  \"b\": }\n}", LENJSON_NUL_TERMINATED, &st);
    CHECK(v == NULL);
    {
        lenjson_error_info info = lenjson_last_error_info();
        CHECK(info.line == 3);
    }

    /* 空指针参数 */
    v = lenjson_parse(NULL, LENJSON_NUL_TERMINATED, &st);
    CHECK(v == NULL);
    CHECK(st == LENJSON_ERROR_INVALID_ARGUMENT);

    /* 解析成功后错误信息被清空 */
    v = lenjson_parse("1", LENJSON_NUL_TERMINATED, &st);
    CHECK(v != NULL);
    CHECK(strlen(lenjson_last_error_message()) == 0);
    lenjson_value_free(v);
}

static void test_nested_access(void) {
    printf("test_nested_access\n");
    lenjson_status st = LENJSON_OK;

    lenjson_value* root = lenjson_parse(
        "{\"name\":\"小明\",\"age\":18,\"tags\":[\"a\",\"b\"],\"ok\":true}",
        LENJSON_NUL_TERMINATED, &st);
    CHECK(root != NULL);
    CHECK(lenjson_value_size(root) == 4);

    /* 对象成员 */
    lenjson_value* name = lenjson_value_get_member(root, "name", &st);
    CHECK(name != NULL);
    CHECK(st == LENJSON_OK);
    size_t length = 0;
    const char* text = lenjson_value_get_string(name, &length);
    CHECK(text != NULL && length == 6);
    CHECK(text != NULL && strcmp(text, kXiaoMing) == 0);

    /* 释放子句柄不影响父节点里的数据 */
    lenjson_value_free(name);

    /* 整数成员 */
    lenjson_value* age = lenjson_value_get_member(root, "age", &st);
    int64_t number = 0;
    CHECK(lenjson_value_get_int(age, &number) == LENJSON_OK);
    CHECK(number == 18);
    lenjson_value_free(age);

    /* 布尔成员 */
    lenjson_value* ok = lenjson_value_get_member(root, "ok", &st);
    int flag = 0;
    CHECK(lenjson_value_get_bool(ok, &flag) == LENJSON_OK);
    CHECK(flag == 1);
    lenjson_value_free(ok);

    /* 数组元素 */
    lenjson_value* tags = lenjson_value_get_member(root, "tags", &st);
    CHECK(lenjson_value_is_array(tags) == 1);
    CHECK(lenjson_value_size(tags) == 2);

    lenjson_value* first = lenjson_value_get_item(tags, 0, &st);
    CHECK(first != NULL);
    text = lenjson_value_get_string(first, NULL);
    CHECK(text != NULL && strcmp(text, "a") == 0);
    lenjson_value_free(first);

    /* 越界 */
    CHECK(lenjson_value_get_item(tags, 9, &st) == NULL);
    CHECK(st == LENJSON_ERROR_OUT_OF_BOUNDS);

    /* 不存在的键 */
    CHECK(lenjson_value_get_member(root, "missing", &st) == NULL);
    CHECK(st == LENJSON_ERROR_OUT_OF_BOUNDS);
    CHECK(lenjson_value_has_member(root, "missing") == 0);
    CHECK(lenjson_value_has_member(root, "name") == 1);

    lenjson_value_free(tags);
    lenjson_value_free(root);
}

static void test_type_errors(void) {
    printf("test_type_errors\n");
    lenjson_status st = LENJSON_OK;

    lenjson_value* v = lenjson_parse("123", LENJSON_NUL_TERMINATED, &st);
    CHECK(v != NULL);

    /* 类型不符：状态码是 TYPE，取值不应改动输出参数 */
    CHECK(lenjson_value_get_string(v, NULL) == NULL);
    CHECK(st == LENJSON_OK);   /* get_string 没有 status 出参 */

    int flag = 1;
    CHECK(lenjson_value_get_bool(v, &flag) == LENJSON_ERROR_TYPE);
    CHECK(flag == 1);

    /* 整数取浮点：按数值提升，不算类型错误 */
    double real = 1.0;
    CHECK(lenjson_value_get_double(v, &real) == LENJSON_OK);
    CHECK(real == 123.0);

    /* 字符串取数值才是类型错误 */
    lenjson_value* text_value = lenjson_parse("\"abc\"", LENJSON_NUL_TERMINATED, &st);
    CHECK(text_value != NULL);
    real = 1.0;
    CHECK(lenjson_value_get_double(text_value, &real) == LENJSON_ERROR_TYPE);
    CHECK(real == 1.0);
    lenjson_value_free(text_value);

    lenjson_value_free(v);
}

static void test_mutation(void) {
    printf("test_mutation\n");
    lenjson_status st = LENJSON_OK;

    /* 构造一个对象 */
    lenjson_value* root = lenjson_value_new_object();
    CHECK(root != NULL);
    CHECK(lenjson_value_is_object(root) == 1);

    /* 写入成员：child 的内容被搬进父节点 */
    lenjson_value* name = lenjson_value_new_string("Alice", LENJSON_NUL_TERMINATED);
    CHECK(lenjson_value_set_member(root, "name", name) == LENJSON_OK);
    lenjson_value_free(name);   /* 搬空之后照样要释放一次 */

    lenjson_value* age = lenjson_value_new_int(30);
    CHECK(lenjson_value_set_member(root, "age", age) == LENJSON_OK);
    lenjson_value_free(age);

    /* 读回来 */
    lenjson_value* got = lenjson_value_get_member(root, "name", &st);
    CHECK(got != NULL);
    const char* text = lenjson_value_get_string(got, NULL);
    CHECK(text != NULL && strcmp(text, "Alice") == 0);
    lenjson_value_free(got);

    /* 覆盖同名成员 */
    lenjson_value* other = lenjson_value_new_string("Bob", LENJSON_NUL_TERMINATED);
    CHECK(lenjson_value_set_member(root, "name", other) == LENJSON_OK);
    lenjson_value_free(other);
    CHECK(lenjson_value_size(root) == 2);
    got = lenjson_value_get_member(root, "name", &st);
    text = lenjson_value_get_string(got, NULL);
    CHECK(text != NULL && strcmp(text, "Bob") == 0);
    lenjson_value_free(got);

    /* 数组：追加与按下标写入 */
    lenjson_value* list = lenjson_value_new_array();
    lenjson_value* item = lenjson_value_new_int(1);
    CHECK(lenjson_value_push_back(list, item) == LENJSON_OK);
    lenjson_value_free(item);
    item = lenjson_value_new_int(3);
    CHECK(lenjson_value_set_item(list, 2, item) == LENJSON_OK);
    lenjson_value_free(item);
    CHECK(lenjson_value_size(list) == 3);   /* 中间空位自动补 null */
    CHECK(lenjson_value_is_null(lenjson_value_get_item(list, 1, &st)) == 1);

    /* 删除 */
    CHECK(lenjson_value_erase_member(root, "age") == LENJSON_OK);
    CHECK(lenjson_value_size(root) == 1);
    CHECK(lenjson_value_erase_member(root, "age") == LENJSON_ERROR_OUT_OF_BOUNDS);
    CHECK(lenjson_value_erase_item(list, 0) == LENJSON_OK);
    CHECK(lenjson_value_size(list) == 2);
    CHECK(lenjson_value_erase_item(list, 99) == LENJSON_ERROR_OUT_OF_BOUNDS);

    /* 借用来的句柄不能直接搬进父节点 */
    lenjson_value* borrowed = lenjson_value_get_item(list, 0, &st);
    CHECK(borrowed != NULL);
    lenjson_value* dest = lenjson_value_new_array();
    CHECK(lenjson_value_push_back(dest, borrowed) == LENJSON_ERROR_INVALID_ARGUMENT);
    lenjson_value_free(borrowed);
    lenjson_value_free(dest);

    lenjson_value_free(list);
    lenjson_value_free(root);
}

static void test_dump(void) {
    printf("test_dump\n");
    lenjson_status st = LENJSON_OK;

    lenjson_value* v = lenjson_parse("{\"a\":1,\"b\":[1,2]}", LENJSON_NUL_TERMINATED, &st);
    CHECK(v != NULL);

    /* 默认：紧凑 */
    size_t length = 0;
    char* text = lenjson_dump(v, NULL, &length);
    CHECK(text != NULL);
    CHECK(text != NULL && strcmp(text, "{\"a\":1,\"b\":[1,2]}") == 0);
    CHECK(length == strlen(text));
    lenjson_free_string(text);

    /* 美化：2 空格缩进 */
    lenjson_dump_options options = lenjson_dump_options_default();
    options.indent = 2;
    text = lenjson_dump(v, &options, &length);
    CHECK(text != NULL);
    CHECK(text != NULL && strcmp(text,
        "{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2\n  ]\n}") == 0);
    lenjson_free_string(text);

    /* ensure_ascii */
    lenjson_value* cn = lenjson_value_new_string(kZhongWen, strlen(kZhongWen));
    options = lenjson_dump_options_default();
    options.ensure_ascii = 1;
    text = lenjson_dump(cn, &options, &length);
    CHECK(text != NULL);
    CHECK(text != NULL && strcmp(text, "\"\\u4e2d\\u6587\"") == 0);
    lenjson_free_string(text);

    /* 不转义时保留 UTF-8 */
    options = lenjson_dump_options_default();
    text = lenjson_dump(cn, &options, &length);
    CHECK(text != NULL);
    CHECK(text != NULL && strlen(text) == strlen(kZhongWen) + 2);
    CHECK(text != NULL && text[0] == '"');
    CHECK(text != NULL && memcmp(text + 1, kZhongWen, strlen(kZhongWen)) == 0);
    lenjson_free_string(text);

    /* 代理对：\uD834\uDD1E <-> U+1D11E */
    lenjson_value* clef = lenjson_parse("\"\\uD834\\uDD1E\"", LENJSON_NUL_TERMINATED, &st);
    CHECK(clef != NULL);
    {
        const char* clef_text = lenjson_value_get_string(clef, &length);
        CHECK(clef_text != NULL && length == strlen(kClef));
        CHECK(clef_text != NULL && memcmp(clef_text, kClef, strlen(kClef)) == 0);
    }
    options = lenjson_dump_options_default();
    options.ensure_ascii = 1;
    text = lenjson_dump(clef, &options, &length);
    CHECK(text != NULL);
    CHECK(text != NULL && strcmp(text, "\"\\ud834\\udd1e\"") == 0);
    lenjson_free_string(text);
    lenjson_value_free(clef);

    lenjson_value_free(cn);
    lenjson_value_free(v);
}

static void test_roundtrip_and_compare(void) {
    printf("test_roundtrip_and_compare\n");
    lenjson_status st = LENJSON_OK;

    const char* samples[] = {
        "null", "true", "12345", "3.14159", "-0.5", "\"hello\"",
        "[]", "{}", "[1,2,3]", "{\"a\":1,\"b\":[true,false,null]}",
        "{\"nested\":{\"x\":{\"y\":[1,2,{\"z\":\"deep\"}]}}}"
    };
    size_t i;
    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        lenjson_value* first = lenjson_parse(samples[i], LENJSON_NUL_TERMINATED, &st);
        CHECK(first != NULL);
        size_t length = 0;
        char* text = lenjson_dump(first, NULL, &length);
        lenjson_value* second = lenjson_parse(text, length, &st);
        CHECK(second != NULL);
        CHECK(lenjson_value_equal(first, second) == 1);
        lenjson_free_string(text);
        lenjson_value_free(first);
        lenjson_value_free(second);
    }

    /* 对象比较与顺序无关，但序列化保留各自的插入顺序 */
    lenjson_value* a = lenjson_parse("{\"x\":1,\"y\":2}", LENJSON_NUL_TERMINATED, &st);
    lenjson_value* b = lenjson_parse("{\"y\":2,\"x\":1}", LENJSON_NUL_TERMINATED, &st);
    CHECK(lenjson_value_equal(a, b) == 1);
    {
        size_t length = 0;
        char* text = lenjson_dump(a, NULL, &length);
        CHECK(text != NULL && strcmp(text, "{\"x\":1,\"y\":2}") == 0);
        lenjson_free_string(text);
        text = lenjson_dump(b, NULL, &length);
        CHECK(text != NULL && strcmp(text, "{\"y\":2,\"x\":1}") == 0);
        lenjson_free_string(text);
    }
    lenjson_value_free(a);
    lenjson_value_free(b);
}

static void test_clone_is_independent(void) {
    printf("test_clone_is_independent\n");
    lenjson_status st = LENJSON_OK;

    lenjson_value* original = lenjson_parse("{\"a\":[1,2,3]}", LENJSON_NUL_TERMINATED, &st);
    CHECK(original != NULL);

    lenjson_value* copy = lenjson_value_clone(original);
    CHECK(copy != NULL);
    CHECK(lenjson_value_equal(original, copy) == 1);

    /* 改副本不影响原件 */
    lenjson_value* list = lenjson_value_get_member(copy, "a", &st);
    lenjson_value* item = lenjson_value_new_int(100);
    CHECK(lenjson_value_set_item(list, 0, item) == LENJSON_OK);
    lenjson_value_free(item);
    lenjson_value_free(list);

    list = lenjson_value_get_member(original, "a", &st);
    lenjson_value* first = lenjson_value_get_item(list, 0, &st);
    int64_t number = 0;
    CHECK(lenjson_value_get_int(first, &number) == LENJSON_OK);
    CHECK(number == 1);
    lenjson_value_free(first);
    lenjson_value_free(list);

    lenjson_value_free(copy);
    lenjson_value_free(original);
}

static void test_options(void) {
    printf("test_options\n");
    lenjson_status st = LENJSON_OK;

    /* 允许注释 */
    const char* text = "{\n  // 行注释\n  \"a\": 1, /* 块注释 */\n  \"b\": 2\n}";

    lenjson_value* v = lenjson_parse(text, LENJSON_NUL_TERMINATED, &st);
    CHECK(v == NULL);
    CHECK(st == LENJSON_ERROR_PARSE);

    lenjson_parse_options options = lenjson_parse_options_default();
    options.allow_comments = 1;
    v = lenjson_parse_ex(text, strlen(text), &options, &st);
    CHECK(v != NULL);
    CHECK(st == LENJSON_OK);
    lenjson_value_free(v);

    /* 嵌套过深 -> 状态码，而不是崩溃 */
    {
        char deep[1200];
        size_t i;
        for (i = 0; i < 600; ++i) { deep[i] = '['; }
        for (i = 0; i < 600; ++i) { deep[600 + i] = ']'; }
        v = lenjson_parse(deep, 1200, &st);
        CHECK(v == NULL);
        CHECK(st == LENJSON_ERROR_PARSE);
    }

    /* 默认选项：max_depth = 512，allow_comments = 0 */
    options = lenjson_parse_options_default();
    CHECK(options.max_depth == 512);
    CHECK(options.allow_comments == 0);

    {
        lenjson_dump_options dump_options = lenjson_dump_options_default();
        CHECK(dump_options.indent == -1);
        CHECK(dump_options.indent_char == ' ');
        CHECK(dump_options.ensure_ascii == 0);
    }
}

static void test_type_names(void) {
    printf("test_type_names\n");
    CHECK(strcmp(lenjson_type_name(LENJSON_TYPE_NULL), "null") == 0);
    CHECK(strcmp(lenjson_type_name(LENJSON_TYPE_BOOLEAN), "boolean") == 0);
    CHECK(strcmp(lenjson_type_name(LENJSON_TYPE_INTEGER), "integer") == 0);
    CHECK(strcmp(lenjson_type_name(LENJSON_TYPE_REAL), "real") == 0);
    CHECK(strcmp(lenjson_type_name(LENJSON_TYPE_STRING), "string") == 0);
    CHECK(strcmp(lenjson_type_name(LENJSON_TYPE_ARRAY), "array") == 0);
    CHECK(strcmp(lenjson_type_name(LENJSON_TYPE_OBJECT), "object") == 0);
}

int main(void) {
    test_parse_basic();
    test_parse_errors();
    test_nested_access();
    test_type_errors();
    test_mutation();
    test_dump();
    test_roundtrip_and_compare();
    test_clone_is_independent();
    test_options();
    test_type_names();

    printf("\n==============================\n");
    printf("用例总数: %d，失败: %d\n", g_total, g_failed);
    if (g_failed == 0) {
        printf("全部通过\n");
    }
    return g_failed == 0 ? 0 : 1;
}
