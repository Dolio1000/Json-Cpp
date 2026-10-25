# lenjson —— 从零实现的 C 接口 JSON 库

一个**不依赖任何第三方库**、只用 C++ 标准库实现的 JSON 序列化 / 反序列化库，
**对外只暴露一套纯 C 接口**。

它的首要目标不是“功能大而全”，而是**把一个库的接口设计与内部实现思路讲清楚**：
既解释“为什么 C++ 库不能随便跨编译器调用”，也解释“一个实用的 JSON 库内部到底怎么写”。

---

## 一、为什么对外是 C 接口

这是本项目最核心的设计决策，值得放在最前面。

C++ 的 ABI 没有统一标准。两个不同的编译器（MSVC / MinGW-g++ / clang）对同一个类的
**内存布局、名字修饰、异常处理、STL 容器实现**都可能不同。只要有一个 C++ 对象
（哪怕只是一个 `std::string`）跨过 DLL 边界，就会出现“这边分配、那边释放”的堆损坏——
症状是运行时的随机崩溃，而不是编译期报错。

C 接口没有这个问题：

| 边界上允许出现的东西 | 说明 |
| --- | --- |
| 函数名 | C 没有 name mangling，两个工具链产出的名字完全一致 |
| `int64_t` / `double` / `int` | 固定宽度，布局由标准保证 |
| 指针 | 只传指针，不解引用 |
| 定长 POD 结构体 | 选项与错误信息都用这种 |
| `const char *` | 只读，且缓冲区由本库分配、本库释放 |

再加上四条硬规矩，本库就做到了“g++ 编译的 DLL 能被 MSVC 程序安全调用”：

1. **导出表里只有 C 符号。** 内部那套 C++ 实现（`lenjson::Value` 等）一律不导出。
   Windows 上靠 `__declspec(dllexport)` 逐个标注，并关掉 CMake 的
   `WINDOWS_EXPORT_ALL_SYMBOLS`；Linux/macOS 上靠 `-fvisibility=hidden` 把内部符号藏起来。
2. **字符串缓冲区由本库分配、由本库释放。** 调用方拿到 `char *`，用
   `lenjson_free_string()` 还回去，绝不自己 `free()`。
3. **异常绝不跨边界。** 库内部抛出的 C++ 异常在本地被捕获，翻译成状态码返回。
4. **句柄是不透明指针。** 调用方不解引用它，只用 API 操作。

实测（MinGW 16.2.0 / MSVC 19.51.36231，均为 x64）：g++ 编出的 `lenjson.dll` 只导出
44 个 C 符号、内部 C++ 符号一个不漏，被 MSVC 编出的程序通过 `lib /def:` 生成的导入库
正常调用，解析、取值、构造、序列化、错误定位全部正确；静态库则**不能**这样用，
原因见第八节。

---

## 二、特性一览

- 对外接口是**纯 C**，C 与 C++ 工程都能直接用；可作为静态库或动态库链接
- 支持 JSON 全部 6 种类型：`null` / 布尔 / 数字 / 字符串 / 数组 / 对象
- 数字区分**整数（`int64_t`）与浮点（`double`）**，整数保持精确、浮点用“最短往返”输出
- 字符串统一以 **UTF-8** 存储；正确处理 `\uXXXX` 与 **UTF-16 代理对**（BMP 之外的字符）
- 反序列化：手写**递归下降解析器**，错误信息带**行号 / 列号**，可设置**最大嵌套深度**防爆栈
- 序列化：支持**紧凑**与**美化（缩进）**输出，可选 **`ensure_ascii`**（把中文转成 `\uXXXX`）
- 对象**保持插入顺序**（序列化结果与原始文本顺序一致）
- 值语义：拷贝是**深拷贝**，移动是**窃取指针**（Rule of Five）
- 错误处理：返回**状态码** + 线程局部的错误文本与出错位置
- 提供 **CMake install / find_package** 支持，也提供给 MSVC 用的 `.def` 导入库定义

---

## 三、目录结构

```
lenjson/
├── CMakeLists.txt
├── README.md
├── cmake/
│   ├── lenjsonConfig.cmake.in   # find_package(lenjson) 用的 package 配置模板
├── include/lenjson/
│   └── lenjson.h                 # 唯一的公开头文件（纯 C 接口）
├── src/
│   ├── internal/                 # 内部 C++ 实现，不对外发布、不导出符号
│   │   ├── json_error.h          #   异常体系（只在库内部使用）
│   │   ├── json_value.h          #   Value 核心类型
│   │   ├── json_parser.h         #   递归下降解析器
│   │   ├── json_writer.h         #   序列化器
│   │   └── json_utils.h          #   底层工具（数字 / UTF-8 / 转义）
│   ├── json_c_api.cpp            # ★ C 接口实现（唯一的对外导出面）
│   ├── json_value.cpp
│   ├── json_value_access.cpp
│   ├── json_parser.cpp
│   ├── json_writer.cpp
│   ├── json_utils_number.cpp
│   └── json_utils_unicode.cpp
├── examples/
│   └── demo.cpp                  # 用 C 接口走一遍完整流程
└── tests/
    ├── test_lenjson_c.c          # 公开 C 接口的测试（纯 C 编译单元）
    └── test_lenjson.cpp          # 内部 C++ 实现的测试
```

依赖关系是**单向、无环**的：

```
utils  <-  value  <-  writer
              ^
              |
           parser
```

`utils` 不依赖任何其它模块，所以能最先编写、也最容易被测试。
`json_c_api.cpp` 依赖全部内部模块，但不导出它们的符号。

---

## 四、构建与运行

### 方式 1：CMake（推荐）

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure   # 运行单元测试
./build/lenjson_demo                                    # 运行示例
```

（多配置生成器如 Visual Studio 会把产物放进 `build/Release/`，那时示例是
`build/Release/lenjson_demo.exe`；单配置生成器如 Ninja 直接放在 `build/` 下。）

CMake 会为 MSVC 自动加上 `/utf-8`（保证中文源码正确），为 GCC/Clang 加上 `-Wall -Wextra`。

默认**同时构建动态库与静态库**，两者由同一份源码编译，区别只在链接方式：

| 产物 | MSVC | MinGW / GCC |
| --- | --- | --- |
| 动态库 | `lenjson.dll` | `lenjson.dll` / `liblenjson.so` |
| 导入库（链接 DLL 时用） | `lenjson.lib` | `liblenjson.dll.a` |
| 静态库 | `lenjson_static.lib` | `liblenjson_static.a` |

> MinGW 上去掉了默认的 `lib` 前缀，让各工具链产出的动态库都叫 `lenjson.dll`——
> 跨编译器使用时，MSVC 侧生成的导入库与运行时要找的 DLL 名字才对得上。

可用的开关：`LENJSON_BUILD_SHARED`、`LENJSON_BUILD_STATIC`、`LENJSON_BUILD_EXAMPLES`、
`LENJSON_BUILD_TESTS`、`LENJSON_INSTALL`（默认 OFF）、`LENJSON_STATIC_RUNTIME`（默认 ON）。

`LENJSON_STATIC_RUNTIME` 只对 GCC/MinGW 的动态库生效：打开时加 `-static`，把
libstdc++ / libgcc / libwinpthread 一起链进 `lenjson.dll`，产物只剩系统 DLL 依赖，
拷到没有装 MinGW 的机器（比如只有 MSVC 的机器）上也能跑。

### 方式 2：直接用 GCC / MinGW

```sh
g++ -std=c++17 -Wall -Wextra -Iinclude \
    src/*.cpp examples/demo.cpp -o demo
./demo

# 内部实现的测试要直接 include src/internal/ 下的头文件，因此多一个 -Isrc
g++ -std=c++17 -Wall -Wextra -Iinclude -Isrc \
    src/*.cpp tests/test_lenjson.cpp -o test_lenjson
./test_lenjson
```

### 方式 3：直接用 MSVC

> MSVC 务必加上 `/utf-8`，否则源码中的中文在当前代码页下会被误读。

```bat
cl /nologo /std:c++17 /EHsc /W4 /utf-8 /Iinclude ^
   src\json_c_api.cpp src\json_value.cpp src\json_value_access.cpp ^
   src\json_parser.cpp src\json_writer.cpp src\json_utils_number.cpp ^
   src\json_utils_unicode.cpp examples\demo.cpp /Fe:demo.exe
demo.exe
```

### 在自己的工程里使用

用 CMake 的话，安装规则要靠 `LENJSON_INSTALL` 打开（默认关闭，不打开时
`cmake --install` 什么也不做，而且不会生成 package 配置文件）：

```sh
cmake -S . -B build -DLENJSON_INSTALL=ON
cmake --build build --config Release
cmake --install build --prefix /opt/lenjson
```

装好的目录里是：`include/lenjson/lenjson.h`、`bin/lenjson.dll`、
`lib/liblenjson.dll.a`（或 `lenjson.lib`）、`lib/liblenjson_static.a`，
以及 `lib/cmake/lenjson/` 下的 package 配置和一份 `lenjson.def`。

```cmake
find_package(lenjson REQUIRED)
target_link_libraries(app PRIVATE lenjson::lenjson)          # 动态库
# 或 target_link_libraries(app PRIVATE lenjson::lenjson_static) # 静态库
```

头文件目录、库文件、`LENJSON_SHARED` 宏都会随目标自动传递，使用方不需要手动加任何东西。

手动链接的话：

- **用动态库**：编译时定义 `LENJSON_SHARED`，链接导入库，运行时把 DLL 放到 exe 同目录；
- **用静态库**：直接链接静态库，不需要定义任何宏。

---

## 五、快速上手

```c
#include "lenjson/lenjson.h"
#include <stdio.h>

int main(void) {
    /* 1. 解析 */
    lenjson_status status = LENJSON_OK;
    lenjson_value* root = lenjson_parse(
        "{\"name\":\"小明\",\"age\":18,\"tags\":[\"a\",\"b\"]}",
        LENJSON_NUL_TERMINATED, &status);
    if (root == NULL) {
        fprintf(stderr, "解析失败: %s\n", lenjson_last_error_message());
        return 1;
    }

    /* 2. 读取 */
    lenjson_value* name = lenjson_value_get_member(root, "name", &status);
    printf("name = %s\n", lenjson_value_get_string(name, NULL));
    lenjson_value_free(name);

    lenjson_value* age = lenjson_value_get_member(root, "age", &status);
    int64_t age_value = 0;
    lenjson_value_get_int(age, &age_value);
    printf("age  = %lld\n", (long long)age_value);
    lenjson_value_free(age);

    /* 3. 构建 */
    lenjson_value* config = lenjson_value_new_object();
    lenjson_value* port = lenjson_value_new_int(8080);
    lenjson_value_set_member(config, "port", port);
    lenjson_value_free(port);           /* 内容已搬进 config，句柄仍要释放 */

    /* 4. 序列化 */
    lenjson_dump_options options = lenjson_dump_options_default();
    options.indent = 2;                 /* 美化输出；-1 是紧凑 */
    char* text = lenjson_dump(config, &options, NULL);
    printf("%s\n", text);
    lenjson_free_string(text);          /* 必须用库自己的函数释放 */

    /* 5. 收尾 */
    lenjson_value_free(config);
    lenjson_value_free(root);
    return 0;
}
```

完整示例见 `examples/demo.cpp`，可运行的测试见 `tests/test_lenjson_c.c`。

---

## 六、内存与所有权规则

整套 API 只有三条规则，记住就不会用错：

**规则一：凡是 API 返回的句柄，都要恰好释放一次。**

```c
lenjson_value_free(handle);
```

根句柄（`lenjson_parse` / `lenjson_value_new_*` / `lenjson_value_clone` 的返回值）
释放时整棵 JSON 树一起销毁；子句柄（`lenjson_value_get_item` /
`lenjson_value_get_member` 的返回值）只是指向父节点里的一个元素，
释放它不会影响父节点里的数据。

**规则二：凡是 API 返回的字符串，都要用 `lenjson_free_string` 释放。**

```c
char* text = lenjson_dump(value, NULL, NULL);
lenjson_free_string(text);
```

不要自己 `free()`——库内部用的可能是与你的程序不同的 CRT / 分配器。

**规则三：写入函数搬走子值的内容，但不接管子值的句柄。**

```c
lenjson_value* child = lenjson_value_new_int(8080);
lenjson_value_set_member(config, "port", child);
lenjson_value_free(child);   /* 照样要释放：此时它已经是个空值 */
```

搬走之后 `child` 变成一个空值（相当于 C++ 的移动后状态），但它的内存还在，
所以**仍然要释放一次**，否则会漏掉那点内存。

另外：`lenjson_value_set_item` / `set_member` / `push_back` 只接受**自己创建的句柄**。
借用来的子句柄属于父节点，把它搬走会把父节点的子树掏空，所以库会直接拒绝；
需要时请先 `lenjson_value_clone()` 复制一份。

---

## 七、错误处理

C 里没有异常，本库用“**状态码 + 线程局部错误信息**”：

```c
lenjson_status status = LENJSON_OK;
lenjson_value* v = lenjson_parse(text, length, &status);
if (v == NULL) {
    lenjson_error_info info = lenjson_last_error_info();
    fprintf(stderr, "%s (第 %zu 行, 第 %zu 列)\n",
            lenjson_last_error_message(), info.line, info.column);
}
```

| 状态码 | 含义 |
| --- | --- |
| `LENJSON_OK` | 成功 |
| `LENJSON_ERROR_INVALID_ARGUMENT` | 传入了空指针之类的非法参数 |
| `LENJSON_ERROR_PARSE` | JSON 文本格式错误（带行列位置） |
| `LENJSON_ERROR_TYPE` | 类型不匹配 |
| `LENJSON_ERROR_OUT_OF_BOUNDS` | 下标越界或键不存在 |
| `LENJSON_ERROR_MEMORY` | 内存分配失败 |
| `LENJSON_ERROR_INTERNAL` | 其它内部错误 |

错误信息按线程保存，因此多线程下互不干扰。代价是：`lenjson_last_error_message()`
返回的指针在**本线程下一次调用 API 后就失效**，要保留必须自己拷一份。

`status` 出参一律可以是 `NULL`（表示不关心）。

---

## 八、跨编译器使用（MinGW 构建 → MSVC 调用）

这是本库对外用 C 接口的主要动机，下面是完整可复现的步骤。

### 第 1 步：用 g++ 编译出 DLL

```sh
g++ -std=c++17 -O2 -m64 -shared -static \
    -DLENJSON_SHARED -DLENJSON_EXPORTS -fvisibility=hidden \
    -Iinclude src/*.cpp -o lenjson.dll
```

三个关键参数：

- `-m64`：MSVC 侧也必须用 x64。32 位的 DLL 和 64 位的 exe 不能互相加载。
- `-fvisibility=hidden`：把内部 C++ 符号藏起来，只留头文件里 `LENJSON_API` 标注的 C 符号。
- `-static`：把 GCC 运行时静态链接进去。只加 `-static-libgcc -static-libstdc++`
  还不够——`thread_local` 错误状态会再牵出 `libwinpthread-1.dll`；用 `-static` 后
  `lenjson.dll` 只剩 `KERNEL32.dll` 和系统 UCRT 依赖，拷到只有 MSVC 的机器上照样能跑。

用 CMake 构建的话，可见性由 `CXX_VISIBILITY_PRESET hidden` 处理，运行时静态链接由
`LENJSON_STATIC_RUNTIME`（默认 ON，加 `-static`）处理，不用自己拼参数。

### 第 2 步：生成 MSVC 的导入库

推荐做法是用 `lib.exe` 从 `.def` 文件生成一份 MSVC 自己的导入库。
（实测 MSVC 的 `link.exe` 其实也能直接吃 MinGW 产出的 `lenjson.dll.a`——它就是个
COFF 导入库——但那是生成器的实现细节，换 MinGW 版本/目标平台就不保证，
`.def` 这条路才是可控的。）

```bat
lib /def:lenjson.def /machine:x64 /out:lenjson.lib
```

`lenjson.def` 由 CMake 在构建时**从公开头文件自动生成**（放在 `build/lenjson.def`，
安装时也会一并安装到 `lib/cmake/lenjson/`），所以它不会和头文件脱节：

```
LIBRARY lenjson.dll
EXPORTS
    lenjson_parse
    lenjson_parse_ex
    lenjson_dump
    ...
```

`LIBRARY` 那一行写明运行时该去找哪个 DLL；导入库叫 `lenjson.lib`，
运行时就会去找 `lenjson.dll`。

### 第 3 步：MSVC 侧编译与调用

```bat
cl /nologo /std:c++17 /EHsc /W4 /utf-8 /Iinclude ^
   my_app.cpp /Fe:my_app.exe /link lenjson.lib
```

`/utf-8` **不能省**：公开头文件的注释是中文、字节是 UTF-8，MSVC 默认按当前代码页
（简体中文系统上是 GBK）解码，会把注释末尾的字节当成行继续符 `\`，
最后报一个莫名其妙的 `fatal error C1022: 应输入 #endif`。

调用方只需要 `#include "lenjson/lenjson.h"` 并定义 `LENJSON_SHARED`：

```bat
cl /nologo /std:c++17 /EHsc /W4 /utf-8 /DLENJSON_SHARED /Iinclude ^
   my_app.cpp /Fe:my_app.exe /link lenjson.lib
```

（用 CMake 的话，`target_link_libraries(app PRIVATE lenjson::lenjson)` 会自动带上这个宏。）

运行时把 `lenjson.dll` 放到 `my_app.exe` 同目录即可。

### 不想生成导入库？用运行时动态加载

如果不想在构建期依赖 DLL，也可以完全用 `LoadLibrary` + `GetProcAddress`：

```c
HMODULE dll = LoadLibraryA("lenjson.dll");
lenjson_value* (*parse)(const char*, size_t, lenjson_status*) =
    (lenjson_value* (*)(const char*, size_t, lenjson_status*))
    GetProcAddress(dll, "lenjson_parse");
```

这条路的好处是完全不需要导入库，缺点是每个函数都要自己转一次类型。

### 那静态库呢？——不行

**g++ 编出来的静态库不能交给 MSVC 链接。** 把 `liblenjson_static.a` 改名 / 过一遍
`lib.exe` 再交给 `link.exe`，会当场报错（实测）：

```
mingw_static.lib(json_c_api.cpp.obj) : fatal error LNK1143:
    无效或损坏的文件: 没有 COMDAT 0x5 的符号
```

就算绕过这一关，后面还欠一堆符号，因为：

1. GCC 的目标文件里有 MSVC 链接器不认识的段与 COMDAT 分组（`.eh_frame` 等）；
2. 静态库**不会**把 libstdc++ / libgcc 打包进去，库里引用的 `__cxa_*`、`_Unwind_*`、
   `__gxx_personality_seh0`、`std::__cxx11::*` 都得由链接方提供，而 MSVC 的 CRT/STL
   里没有这些名字；
3. 即使强行拼上两套运行时，GCC 和 MSVC 也是两个堆、两套异常机制，
   跨边界传对象迟早出事——本库做成 C 接口正是为了绕开这一整类问题。

所以：**跨编译器只能走 DLL；想要静态链接，就用 MSVC 自己把源码编一遍**
（源码是标准 C++17，不需要任何改动）。

对照表：

| 产物 | MSVC 能直接用吗 | 说明 |
| --- | --- | --- |
| `lenjson.dll` + 由 `.def` 生成的 `lenjson.lib` | ✅ 推荐 | 本 README 的路线，实测可用 |
| `lenjson.dll` + MinGW 的 `liblenjson.dll.a` | ⚠️ 实测能连，但不保证 | 依赖 `link.exe` 对 COFF 导入库的容忍度 |
| `liblenjson_static.a` | ❌ 不能 | `LNK1143` + 缺 libstdc++ 符号 |
| MinGW 安装包给 MSVC 工程 `find_package` | ⚠️ 实测可以，但不保证 | 需要同架构，且 `.dll.a` 能连上 |

---

## 九、C API 速查

| 目的 | 写法 |
| --- | --- |
| 解析 | `lenjson_parse(text, length, &status)` |
| 带选项解析 | `lenjson_parse_ex(text, length, &options, &status)` |
| 默认选项 | `lenjson_parse_options_default()` / `lenjson_dump_options_default()` |
| 序列化 | `lenjson_dump(value, &options, &out_length)` |
| 释放字符串 | `lenjson_free_string(text)` |
| 释放句柄 | `lenjson_value_free(value)` |
| 深拷贝 | `lenjson_value_clone(value)` |
| 构造 | `lenjson_value_new_null/bool/int/double/string/array/object` |
| 类型判断 | `lenjson_value_type` / `lenjson_value_is_null/bool/integer/real/number/string/array/object` |
| 类型名 | `lenjson_type_name(type)` |
| 取值 | `lenjson_value_get_bool/int/double` |
| 取字符串 | `lenjson_value_get_string`（借用）/ `lenjson_value_get_string_copy`（副本） |
| 大小 / 判空 | `lenjson_value_size` / `lenjson_value_empty` |
| 数组元素 | `lenjson_value_get_item(array, index, &status)` |
| 对象成员 | `lenjson_value_get_member(object, key, &status)` |
| 判断成员 | `lenjson_value_has_member(object, key)` |
| 写入 | `lenjson_value_set_item` / `set_member` / `push_back` |
| 删除 | `lenjson_value_erase_item` / `erase_member` |
| 清空 | `lenjson_value_clear` |
| 比较 | `lenjson_value_equal(a, b)` |
| 错误信息 | `lenjson_last_error_message()` / `lenjson_last_error_info()` |

长度参数统一传 `LENJSON_NUL_TERMINATED` 表示“按 C 字符串处理”（内部用 `strlen`）。
JSON 字符串里可能有 `\u0000`，所以文本长度要用 `lenjson_dump` 的 `out_length` 取，
不要用 `strlen`。

---

## 十、内部实现的设计讲解

C 接口是外壳，里面的 C++ 实现才是这个库的主体。代码配了大量中文注释，
按功能模块切分，单个源文件都不超过 600 行。

### 1. 用 `std::variant` 表示“动态类型”

JSON 的值在运行期才有确定类型，C++ 里需要一个“**带标签的联合体（tagged union）**”。
朴素做法是手写 `union { bool; long long; double; std::string; Array*; Object*; }`，
但它不会自动调用非平凡成员的构造/析构，需要手动 `placement new`、手动析构、手动维护标签，
非常容易出错。`std::variant` 正是把这套样板封装好了：

```cpp
using Storage = std::variant<std::nullptr_t, bool, long long, double,
                             std::string, std::unique_ptr<Array>, std::unique_ptr<Object>>;
```

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

C 接口的 `lenjson_value_clone` 与 `set_member` 的“搬走内容”语义，正是建立在这套规则之上。

### 4. 递归下降解析器

JSON 的文法本身就是递归的，直接把文法翻译成一组互相调用的函数即可：

```
value  := object | array | string | number | 'true' | 'false' | 'null'
object := '{' (string ':' value (',' string ':' value)*)? '}'
array  := '[' (value (',' value)*)? ']'
```

解析器是一个“**状态机对象**”，内部记录当前读取位置 `pos_`；出错时扫描一遍输入即可算出
行列位置。我们还用 `max_depth` 限制嵌套深度，把“恶意超深 JSON 导致的栈溢出崩溃”
变成一个可捕获的 `LENJSON_ERROR_PARSE`。

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

### 8. C 接口这一层怎么写

`src/json_c_api.cpp` 的骨架只有三块：

1. **句柄**：公开头文件里 `lenjson_value` 是不透明声明，这里给出完整定义
   （一个 `Value *` 加一个 `owned` 标志）。子句柄包一层而不是直接返回节点指针，
   是为了让 `lenjson_value_free` 对根和子都安全——调用方不必记忆“借用 / 拥有”的差别。
2. **异常翻译**：一个 `guard()` 模板把 `ParseError` / `TypeException` / `OutOfRange` /
   `bad_alloc` 翻译成状态码，并把文本与行列位置写进线程局部变量。
3. **句柄工厂**：`make_owned()` 建根句柄（拥有数据），`make_borrowed()` 建子句柄（借用数据）。

---

## 十一、已知取舍与可继续练习的方向

- 对象按键查找是 O(n)：可增加可选的哈希索引（同时保留顺序）。
- 解析时未做浮点的“区域无关”处理（依赖 C 区域设置）：可用 `std::from_chars` 替换。
- 未实现 JSON Pointer / JSON Patch、注释保留等高级特性。
- 可增加对自定义结构体的映射支持（设计一套 C 侧的“序列化 traits”）。
- 可把对象键类型、分配器做成模板参数，进一步体会“库的可配置性”设计。
- `lenjson.def` 由 CMake 从头文件自动生成，但如果手工改了导出列表，记得同步头文件。

愿你在这份代码里看到的，不只是“一个 JSON 库”，而是**一种把复杂需求拆解成清晰模块的方法**。
