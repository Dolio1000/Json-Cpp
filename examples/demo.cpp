// =============================================================================
// demo.cpp —— 用对外的 C 接口走一遍完整流程
// -----------------------------------------------------------------------------
// 这个示例刻意只使用 include/lenjson/lenjson.h 里的 C 接口，
// 因此它同时也是“MSVC 程序如何调用 g++ 编译的 DLL”的写法示范。
// =============================================================================

#include <cstdio>

#include "lenjson/lenjson.h"

namespace {

// 读取一个字符串成员；不存在或类型不对时返回 0。
const char* get_string(const lenjson_value* object, const char* key) {
    lenjson_status status = LENJSON_OK;
    lenjson_value* member = lenjson_value_get_member(object, key, &status);
    if (member == nullptr) {
        return nullptr;
    }
    // lenjson_value_get_string 返回的是借用指针：它指向 member 内部，
    // member 被释放后就失效了。演示里我们只用它打印，不保存。
    const char* text = lenjson_value_get_string(member, nullptr);
    static char buffer[256];
    if (text == nullptr) {
        lenjson_value_free(member);
        return nullptr;
    }
    std::snprintf(buffer, sizeof(buffer), "%s", text);
    lenjson_value_free(member);
    return buffer;
}

void demo_parse_and_read() {
    std::printf("== 1. 解析并读取 ==\n");

    const char* json = R"({"name":"小明","age":18,"tags":["a","b"],"vip":true})";

    lenjson_status status = LENJSON_OK;
    lenjson_value* root = lenjson_parse(json, LENJSON_NUL_TERMINATED, &status);
    if (root == nullptr) {
        std::printf("解析失败: %s\n", lenjson_last_error_message());
        return;
    }

    std::printf("name = %s\n", get_string(root, "name"));

    lenjson_value* age = lenjson_value_get_member(root, "age", &status);
    int64_t age_value = 0;
    if (age != nullptr && lenjson_value_get_int(age, &age_value) == LENJSON_OK) {
        std::printf("age  = %lld\n", static_cast<long long>(age_value));
    }
    lenjson_value_free(age);

    lenjson_value* vip = lenjson_value_get_member(root, "vip", &status);
    int vip_value = 0;
    if (vip != nullptr && lenjson_value_get_bool(vip, &vip_value) == LENJSON_OK) {
        std::printf("vip  = %s\n", vip_value ? "true" : "false");
    }
    lenjson_value_free(vip);

    lenjson_value* tags = lenjson_value_get_member(root, "tags", &status);
    if (tags != nullptr) {
        std::printf("tags = [");
        for (size_t i = 0; i < lenjson_value_size(tags); ++i) {
            lenjson_value* item = lenjson_value_get_item(tags, i, &status);
            const char* text = (item != nullptr) ? lenjson_value_get_string(item, nullptr) : nullptr;
            std::printf("%s%s", (i > 0 ? ", " : ""), (text != nullptr ? text : "?"));
            lenjson_value_free(item);
        }
        std::printf("]\n");
    }
    lenjson_value_free(tags);

    lenjson_value_free(root);
}

void demo_build_and_dump() {
    std::printf("\n== 2. 构建并序列化 ==\n");

    lenjson_value* config = lenjson_value_new_object();

    lenjson_value* server = lenjson_value_new_string("127.0.0.1", LENJSON_NUL_TERMINATED);
    lenjson_value_set_member(config, "server", server);
    lenjson_value_free(server);   // 内容已经搬进 config，句柄本身仍要释放

    lenjson_value* port = lenjson_value_new_int(8080);
    lenjson_value_set_member(config, "port", port);
    lenjson_value_free(port);

    lenjson_value* tags = lenjson_value_new_array();
    lenjson_value* tag = lenjson_value_new_string("web", LENJSON_NUL_TERMINATED);
    lenjson_value_push_back(tags, tag);
    lenjson_value_free(tag);
    lenjson_value_set_member(config, "tags", tags);
    lenjson_value_free(tags);

    // 紧凑输出
    size_t length = 0;
    char* text = lenjson_dump(config, nullptr, &length);
    if (text != nullptr) {
        std::printf("紧凑: %s\n", text);
        lenjson_free_string(text);
    }

    // 美化输出（2 空格缩进）
    lenjson_dump_options options = lenjson_dump_options_default();
    options.indent = 2;
    text = lenjson_dump(config, &options, &length);
    if (text != nullptr) {
        std::printf("美化:\n%s\n", text);
        lenjson_free_string(text);
    }

    lenjson_value_free(config);
}

void demo_error_handling() {
    std::printf("\n== 3. 错误处理 ==\n");

    lenjson_status status = LENJSON_OK;
    lenjson_value* bad = lenjson_parse("{\"a\": }", LENJSON_NUL_TERMINATED, &status);
    if (bad == nullptr) {
        lenjson_error_info info = lenjson_last_error_info();
        std::printf("状态码 = %d\n", static_cast<int>(status));
        std::printf("错误信息: %s\n", lenjson_last_error_message());
        std::printf("位置: 第 %zu 行, 第 %zu 列\n", info.line, info.column);
    }
}

} // namespace

int main() {
    demo_parse_and_read();
    demo_build_and_dump();
    demo_error_handling();
    return 0;
}
