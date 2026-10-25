# lenjson —— 从零实现的 C++17 JSON 库（学习向）

一个**不依赖任何第三方库**、只用 C++ 标准库实现的 JSON 序列化 / 反序列化库。
它的首要目标不是“功能大而全”，而是**把 JSON 库背后的语言设计与核心实现思路讲清楚**，
适合已经会写基本 C++、想进一步理解“一个实用的库到底是怎么设计出来的”的读者。

代码配了大量中文注释，并且**按功能模块切分文件，单个源文件都不超过 600 行**。

---

## 一、特性一览

- 支持 JSON 全部 6 种类型：`null` / 布尔 / 数字 / 字符串 / 数组 / 对象
- 数字区分**整数（`long long`）与浮点（`double`）**，整数保持精确、浮点用“最短往返”输出
- 字符串统一以 **UTF-8** 存储；正确处理 `\uXXXX` 与 **UTF-16 代理对**（BMP 之外的字符）
- 反序列化：手写**递归下降解析器**，错误信息带**行号 / 列号**，可设置**最大嵌套深度**防爆栈
- 序列化：支持**紧凑**与**美化（缩进）**输出，可选 **`ensure_ascii`**（把中文转成 `\uXXXX`）
- 对象**保持插入顺序**（序列化结果与原始文本顺序一致）
- 值语义：拷贝是**深拷贝**，移动是**窃取指针**（Rule of Five）
- 类型安全的取值 `get<T>()`，以及带默认值的 `get_or<T>()`
- 异常分层：`ParseError` / `TypeException` / `OutOfRange`，统一继承自 `JsonException`

---

## 二、目录结构与模块划分

```
lenjson/
├── CMakeLists.txt
├── README.md
├── include/lenjson/
│   ├── lenjson.h            # 聚合头：一行包含全部公开接口
│   ├── json_export.h        # 动态库符号导入/导出宏（LENJSON_API）
│   ├── json_error.h         # 异常体系
│   ├── json_value.h         # Value 核心类型（动态类型 + 递归结构 + 值语义）
│   ├── json_parser.h        # 递归下降解析器
│   ├── json_writer.h        # 序列化器
│   └── json_utils.h         # 底层工具（数字 / UTF-8 / 转义）
├── src/
│   ├── json_value.cpp         # Value：构造 / 生命周期 / 类型 / 取值
│   ├── json_value_access.cpp  # Value：元素访问 / 增删改 / 比较 / 序列化转发
│   ├── json_parser.cpp        # 解析器实现
│   ├── json_writer.cpp        # 序列化实现
│   ├── json_utils_number.cpp  # 数字 <-> 字符串
│   └── json_utils_unicode.cpp # UTF-8 编解码 + 字符串转义
├── examples/
│   └── demo.cpp
└── tests/
    └── test_lenjson.cpp
```

依赖关系是**单向、无环**的：

```
utils  <-  value  <-  writer
              ^
              |
           parser
```

`utils` 不依赖任何其它模块，所以能最先编写、也最容易被测试。

---

## 三、构建与运行

### 方式 1：CMake（推荐）

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure   # 运行单元测试
./build/Release/lenjson_demo                            # 运行示例
```

CMake 会为 MSVC 自动加上 `/utf-8`（保证中文源码正确），为 GCC/Clang 加上 `-Wall -Wextra`。

默认**同时构建动态库与静态库**，两者由同一份源码编译，区别只在链接方式：

| 产物 | MSVC | MinGW / GCC |
| --- | --- | --- |
| 动态库 | `lenjson.dll` | `liblenjson.dll` |
| 导入库（链接 DLL 时用） | `lenjson.lib` | `liblenjson.dll.a` |
| 静态库 | `lenjson_static.lib` | `liblenjson_static.a` |

想只要其中一种，可以加 `-DLENJSON_BUILD_SHARED=OFF` 或 `-DLENJSON_BUILD_STATIC=OFF`。

Windows 下 DLL 的符号必须显式导出才能被外部链接，这件事由 `json_export.h` 里的
`LENJSON_API` 宏负责：编译 DLL 时展开为 `dllexport`，使用 DLL 时展开为 `dllimport`，
而静态库不使用这个宏（代码直接被“搬进”最终程序）。

**在自己的工程里使用：**

- 用动态库：编译时定义 `LENJSON_SHARED`，链接导入库，运行时把 `lenjson.dll` 放到 exe 同目录；
- 用静态库：直接链接静态库即可，不需要定义任何宏。

### 方式 2：直接用 GCC / MinGW

```sh
g++ -std=c++17 -Wall -Wextra -Iinclude \
    src/*.cpp examples/demo.cpp -o demo
./demo

g++ -std=c++17 -Wall -Wextra -Iinclude \
    src/*.cpp tests/test_lenjson.cpp -o test_lenjson
./test_lenjson
```

### 方式 3：直接用 MSVC

> MSVC 务必加上 `/utf-8`，否则源码中的中文在当前代码页下会被误读。

```bat
cl /nologo /std:c++17 /EHsc /W4 /utf-8 /Iinclude ^
   src\json_value.cpp src\json_value_access.cpp src\json_parser.cpp ^
   src\json_writer.cpp src\json_utils_number.cpp src\json_utils_unicode.cpp ^
   examples\demo.cpp /Fe:demo.exe
demo.exe
```

---

## 四、快速上手

```cpp
#include "lenjson/lenjson.h"
#include <iostream>

int main() {
    // 1. 解析
    lenjson::Value root = lenjson::parse(R"({"name":"小明","age":18,"tags":["a","b"]})");

    // 2. 读取（类型安全，失败抛异常；get_or 提供默认值）
    std::string name = root["name"].get<std::string>();
    int         age  = root["age"].get<int>();
    bool        vip  = root["vip"].get_or<bool>(false);

    // 3. 构建
    lenjson::Value config;
    config["server"] = "127.0.0.1";
    config["port"]   = 8080;
    config["tags"]   = lenjson::Value::Array{"web", "api"};

    // 4. 序列化（紧凑 / 美化 2 空格 / 纯 ASCII）
    std::cout << config.dump() << "\n";
    std::cout << config.dump(2) << "\n";
    std::cout << root["name"].dump(-1, /*ensure_ascii=*/true) << "\n";  // "\u5c0f\u660e"

    // 5. 错误处理
    try {
        lenjson::parse("{bad}");
    } catch (const lenjson::ParseError& e) {
        std::cerr << e.what() << "\n";   // 解析错误 (第 1 行, 第 2 列): ...
    }
}
```

---

## 五、核心设计讲解（重点）

### 1. 用 `std::variant` 表示“动态类型”

JSON 的值在运行期才有确定类型，C++ 里需要一个“**带标签的联合体（tagged union）**”。
朴素做法是手写 `union { bool; long long; double; std::string; Array*; Object*; }`，
但它不会自动调用非平凡成员的构造/析构，需要手动 `placement new`、手动析构、手动维护标签，
非常容易出错。`std::variant` 正是把这套样板封装好了：

```cpp
using Storage = std::variant<std::nullptr_t, bool, long long, double,
                             std::string, std::unique_ptr<Array>, std::unique_ptr<Object>>;
```

> 练习建议：试着用裸 `union` 手写一份等价的 `Storage`，体会 `variant` 帮你省掉了什么。

### 2. 递归类型必须加“间接层（指针）”

`Value` 内部要装数组，而数组的元素又是 `Value` —— 这是**递归定义**。
如果在 `Value` 里直接放 `std::vector<Value>`，就会在 `Value` 尚未定义完整时实例化它，
属于“**不完整类型递归**”，标准并不保证可移植。用 `std::unique_ptr<Array>` 包一层后，
`unique_ptr` 的大小与 `Array` 是否完整无关，`Value` 的定义才得以自洽
（`nlohmann/json` 等主流库也是同样的思路）。

### 3. 值语义与 Rule of Five

JSON 值应当像 `int` 一样可以自由复制。因为 `unique_ptr` 不可拷贝，编译器默认生成的拷贝会被删除，
我们必须**手写深拷贝**：

- 拷贝构造：递归复制整棵子树（`clone`）——两个值完全独立；
- 移动构造：窃取内部指针（O(1)），并把源对象重置为 `null`；
- 拷贝赋值：采用 **copy-and-swap**，天然自赋值安全、异常安全。

这份代码是理解 C++“值语义 vs 引用语义”“拷贝控制”的绝佳素材。

### 4. 递归下降解析器

JSON 的文法本身就是递归的，直接把文法翻译成一组互相调用的函数即可：

```
value  := object | array | string | number | 'true' | 'false' | 'null'
object := '{' (string ':' value (',' string ':' value)*)? '}'
array  := '[' (value (',' value)*)? ']'
```

解析器是一个“**状态机对象**”，内部记录当前读取位置 `pos_`；出错时扫描一遍输入即可算出
行列位置。我们还用 `max_depth` 限制嵌套深度，把“恶意超深 JSON 导致的栈溢出崩溃”
变成一个可捕获的 `ParseError`。

### 5. 数字处理：整数保精度 + 浮点最短往返

- 没有小数点/指数时按整数解析，用 `strtoll` 并检查 `ERANGE`，**保持精度**；
  真的超出 `long long` 才退化为 `double`。
- 浮点序列化用“**最短往返**”算法：从 1 位有效数字逐步加到 17 位，一旦
  `strtod(输出) == 原值` 就停下，得到既最短又无损的文本。
- 整数值的浮点（如 `1.0`）会补上 `.0`，避免往返后从浮点变成整数。

### 6. UTF-8 与代理对

`\uXXXX` 表示的是一个 UTF-16 码元。BMP 之外的字符（如 𝄞 U+1D11E）需要**一对代理项**
`\uD834\uDD1E` 合成。解析时我们把它们还原成码点、再编码成 UTF-8 存入内存；
序列化时若开启 `ensure_ascii`，则反向操作，把 UTF-8 解码回码点再拆成代理对输出。

### 7. 对象为什么用“有序键值对向量”而不是 `std::map`

`std::map` 会按字典序排列键，破坏原始顺序。为了让**序列化结果与解析顺序一致**，
对象采用 `std::vector<std::pair<std::string, Value>>`：代价是按键查找为 O(n)，
但换来顺序保真，对日常体量的 JSON 完全够用。

---

## 六、API 速查

| 目的 | 写法 |
| --- | --- |
| 解析 | `lenjson::parse(text)` / `Value::parse(text)` |
| 带选项解析 | `lenjson::ParseOptions opt; opt.allow_comments = true; lenjson::Parser::parse(text, opt)` |
| 紧凑序列化 | `v.dump()` |
| 美化序列化 | `v.dump(2)` |
| 纯 ASCII 输出 | `v.dump(-1, true)` |
| 类型判断 | `is_null/is_bool/is_integer/is_real/is_number/is_string/is_array/is_object` |
| 取值 | `as_bool / as_integer / as_real / as_string / as_number` |
| 模板取值 | `get<T>()`、`get_or<T>(默认值)` |
| 下标访问 | `v[i]`（数组，越界自动扩容）、`v["key"]`（对象，不存在则插入） |
| 带检查访问 | `v.at(i)`、`v.at("key")`（越界抛 `OutOfRange`） |
| 查找 / 判断 / 删除 | `find` / `contains` / `erase` |
| 数组追加 | `v.push_back(x)` |
| 容器视图 | `v.as_array()`、`v.as_object()`（可直接范围 `for`） |
| 大小 / 判空 / 清空 | `size()` / `empty()` / `clear()` |
| 比较 / 交换 | `operator==`、`operator!=`、`swap` |

---

## 七、已知取舍与可继续练习的方向

- 对象按键查找是 O(n)：可增加可选的哈希索引（同时保留顺序）。
- 解析时未做浮点的“区域无关”处理（依赖 C 区域设置）：可用 `std::from_chars` 替换。
- 未实现 `std::ostream` 流式输出、JSON Pointer / JSON Patch、注释保留等高级特性。
- 可增加对 `std::optional<T>`、自定义结构体（序列化 traits）的映射支持。
- 可把对象键类型、分配器做成模板参数，进一步体会“库的可配置性”设计。

愿你在这份代码里看到的，不只是“一个 JSON 库”，而是**一种把复杂需求拆解成清晰模块的方法**。
