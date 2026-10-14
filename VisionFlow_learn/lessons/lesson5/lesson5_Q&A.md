# 第五课问答记录：相对路径、JSON 字段顺序、默认参数与异常类型

> 配套讲义：[05-1-command-line.md](05-1-command-line.md)、[05-2-json-results.md](05-2-json-results.md)、[05-3-config-validation.md](05-3-config-validation.md)
> 代码：[apps/vision_cli.cpp](../../apps/vision_cli.cpp)、[include/processor.hpp](../../include/processor.hpp)、[src/processor.cpp](../../src/processor.cpp)
> 涉及数据流：命令行参数 → `Config` → `create_processor` → `LaneLinesProcessor` → `process_lane_lines` → `rank_and_limit`

本文件按提问顺序整理第五课的四次问答。

---

## 问题 1：为什么 `"samples/frame.jpg"` 不用写成 `"./samples/frame.jpg"`？

### 结论：两者是**同一个路径**

`./` 不是语法、不是前缀操作符，它就是**当前目录这个目录项本身**。用 inode 验证（同一个 inode = 同一个东西）：

```text
$ ls -di . ./
111235 .
111235 ./
```

`.` 是内核里每个目录天生带的一个硬链接，指向自己。所以 `./samples/frame.jpg` 的意思是"当前目录 → samples → frame.jpg"，而 `samples/frame.jpg` 的意思是"（从当前目录出发）samples → frame.jpg"。**起点本来就是当前目录**，写 `./` 只是把这个默认行为显式地说了一遍。

### 内核怎么解析

`cv::imread` 最终调到 `open(path, O_RDONLY)`。内核对路径只有两条规则（POSIX）：

- 以 `/` 开头 → **绝对路径**，从根目录开始
- 其他任何情况 → **相对路径**，从进程的 **CWD**（当前工作目录）开始

[vision_cli.cpp:107](../../apps/vision_cli.cpp#L107) 那个字符串不满足第一条，走第二条，起点就是 CWD。加不加 `./` 走的是同一条分支、同一段解析逻辑。CWD 存在进程的 `fs_struct` 里，可以用 `readlink /proc/<pid>/cwd` 看到。

### 那为什么 `./a.out` 必须写 `./`

因为那是 **shell 的规则，不是路径的规则**：

```bash
./build/vision_cli     # 必须写 ./
```

敲命令时 shell 拿到 `build/vision_cli`，会先去**搜 `PATH`** 里那串目录（`/usr/bin`、`/bin`…），找不到就报 `command not found`。`.` 一般**故意不在 PATH 里**（安全考虑：否则当前目录里放一个叫 `ls` 的恶意程序就能冒充系统命令）。所以要运行当前目录的程序，必须用 `./` 明确告诉 shell"别搜 PATH，就在这儿"。

而 `cv::imread("samples/frame.jpg")` **根本不经过 shell**，只是把字符串交给 `open()`。没有 PATH 搜索，没有程序查找，`.` 不参与任何决策。

> 一句话：`./` 在**命令行的可执行文件查找**场景里是必须的；在**程序内部打开数据文件**的场景里是纯装饰。

### 真正的坑：CWD 依赖，不是 `./`

`./` 冗余无害，但相对路径有必须清楚的语义：**它是相对于"运行程序时你站在哪个目录"，而不是相对于 [vision_cli.cpp](../../apps/vision_cli.cpp) 所在的 `apps/` 目录。**

```bash
# 在项目根目录 ── 能跑通
$ ./build/vision_cli
# CWD = VisionFlow_learn/ → 找到 samples/frame.jpg ✓

# 进到 build 目录再跑 ── 立刻失败
$ cd build && ./vision_cli
# CWD = VisionFlow_learn/build/ → 去找 build/samples/frame.jpg ✗
# 输出: Cannot read image: samples/frame.jpg
```

同样的道理，`cv::imwrite("build/edges.png", ...)` 也是 CWD 相对路径——只在项目根目录运行时才写得进去。

**顺带**：路径不存在时 `cv::imread` **不抛异常，而是返回空 `cv::Mat`**，所以必须检查 `image.empty()`。对比：`cv::imwrite` 失败返回的是 `bool`。

### 更稳健的做法

- **当前代码已经给出了最好的一手**：`--input` 参数让调用者自己决定路径，默认值只是"方便"
- 若确实要写死默认值又想摆脱 CWD 依赖，可用 CMake 把绝对路径编进去：

  ```cmake
  target_compile_definitions(vision_cli PRIVATE
      SAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples")
  ```

- IDE 侧也有对应开关（VSCode `launch.json` 的 `"cwd"`、CLion 的 Working directory），本质都在控制同一个 CWD

---

## 问题 2：`05-2` 讲义说"字段排列可能不同"，为什么 JSON 对象的字段顺序不固定？

### 两层原因

**第一层：JSON 规范规定对象本来就是无序的。**

RFC 8259 对对象的定义原文是 "An object is an **unordered** collection of zero or more name/value pairs"。所以"字段谁在前"根本不是 JSON 文档语义的一部分——任何顺序都是合法 JSON。

这正好解释项目里两类数据的差别：

| | 顺序 | 原因 |
| --- | --- | --- |
| 对象 `{"status": ..., "width": ...}` | 无意义 | 靠**键名**取值 |
| 数组 `[x1, y1, x2, y2]` | 是语义 | 靠**位置**取值 |

`[82, 358, 287, 203]` 调换一下就是另一条线段；而 `status` 和 `width` 谁先谁后不影响任何东西。这正是讲义里"JSON 本身不知道它是线段，这是我们规定的含义"那句话的分量所在。

**第二层（更具体）：nlohmann/json 默认把对象存在一个按字典序排序的容器里。**

本机安装的 3.7.3 头文件证据：

| 位置 | 内容 |
| --- | --- |
| `nlohmann/json.hpp:86` | 注释写明 `std::map` by default |
| `nlohmann/json.hpp:479` | `using object_t = ObjectType<StringType, ...>` |
| `nlohmann/detail/output/serializer.hpp:119` | dump 时 `auto i = val.m_value.object->cbegin();`，从容器头迭代到尾 |

也就是说 **`nlohmann::json` 的对象容器是 `std::map<std::string, json>`**。`std::map` 是一棵红黑树，遍历顺序 = **键的字典序**，和插入先后完全无关。

`document["width"] = ...` 底层是 `m_value.object->operator[](key)`——往 map 里插一个键，插进去那一刻顺序就定了。

**所以实际输出大概率是：**

```json
{
  "channels": 3,
  "edge_pixels": 2095,
  "height": 360,
  "lines": [],
  "processor": "edges",
  "status": "ok",
  "width": 640
}
```

注意这跟 `05-2` 讲义里的示例**不一样**——讲义那份是按代码书写顺序列的。所以讲义才要加那句"字段排列可能不同，键和值应一致"来兜底。以实际 `cat build/result.json` 的结果为准。

### "可能"在可能什么

写"可能"而不是"一定"是更保守也更正确的说法——顺序取决于**哪套实现**：

- **换容器**：`std::map` → `std::unordered_map`，顺序变成哈希序，看起来完全随机
- **换版本**：nlohmann 从 **3.9.0** 才引入 `nlohmann::ordered_json`（保留插入顺序）。本机 3.7.3 里没有这个类型（头文件中检索命中 0 次）
- **换语言/库**：Python 的 `json.dumps` 保留 dict 插入序；Go 的 `encoding/json` 对 map 按键排序；Java Jackson 默认插入序
- **中间过一手**：解析别人的 JSON 再 dump，顺序会按解析器自己的容器规则重排

### 工程上的结论

1. **对象靠键名取，不靠位置**：C++ 侧 `document["width"]`，将来 Python 侧 `data["width"]`
2. **数组靠位置取，位置是契约**：`[x1, y1, x2, y2]` 和 `lines` 里第几条线段，顺序必须保留
3. **不要拿文本逐字比对做测试**：以后写自动化校验，应该先解析成对象再按键比较，否则一个字典序差异就会误报
4. **文档写"预期输出"时**，要么按实际顺序写，要么加一句"排列可能不同，键和值一致"

---

## 问题 3：`Config` 里已有 `max_lines = 3`，`processor.hpp` 里那三个 `= 3` 默认值是不是浪费？

### 结论

**性能上完全不浪费，但语义上埋了个隐患。** 而且有个更关键的事实：

> 在当前程序里，[processor.hpp](../../include/processor.hpp) 里那三个 `= 3` **一次都没生效过**。

### 先追一遍调用链

| 位置 | 调用点 | 实际传参 | 默认值生效了吗 |
| --- | --- | --- | --- |
| [vision_cli.cpp:113](../../apps/vision_cli.cpp#L113) | `create_processor(processor_name, config.max_lines)` | 显式传两个 | ❌ |
| [processor.cpp:116](../../src/processor.cpp#L116) | `make_unique<LaneLinesProcessor>(max_lines)` | 显式传 | ❌ |
| [processor.cpp:19](../../src/processor.cpp#L19) | `process_lane_lines(image, max_lines_)` | 显式传 | ❌ |

三条链路上每一环都显式把值传了下去，**三个默认值全是死代码**（就当前程序而言）。真正决定"默认 3"的只有 [Config](../../apps/vision_cli.cpp#L13) 那一处。

### 性能上：默认实参零运行时开销

默认实参**不是**运行时判断"他没传我就填 3"，而是**编译器在调用点直接把实参补进生成的代码里**：

```cpp
create_processor("edges")   // 编译器眼里等价于 create_processor("edges", 3)
```

不占运行时分支、不占额外内存、不影响速度——纯粹的编译期语法糖，连一个字节的机器码都不多花。

### 真正的隐患：默认值有了两个"权威来源"

"默认保留 3 条线段"这件事现在写了 **4 遍**：

| 位置 | 写法 | 语义 |
| --- | --- | --- |
| [vision_cli.cpp:13](../../apps/vision_cli.cpp#L13) | `Config::max_lines = 3` | 程序级默认（**权威**） |
| [processor.hpp:35](../../include/processor.hpp#L35) | `LaneLinesProcessor(int = 3)` | 对象构造默认 |
| [processor.hpp:50](../../include/processor.hpp#L50) | `process_lane_lines(..., = 3)` | 函数接口默认 |
| [processor.hpp:51](../../include/processor.hpp#L51) | `create_processor(..., = 3)` | 工厂接口默认 |

现在它们**恰好都是 3**，所以看不出问题。但设想把默认值改成 5：

- 改 `Config` ✅ 正确，那是权威
- 但若以后有人写了 `create_processor("lane_lines")` 忘传第二个参数 → 拿到的是 **3**，不是 5 ❌

**同一个"程序默认值"两个地方各说各话**，是典型的 bug 温床。`05-3` 讲义写明"不传 `--config` | 使用默认 `max_lines=3`"，也就是说 **`Config` 才是权威**；`processor.hpp` 那三处是"接口自带的方便默认"，语义层级不一样。

### 但它们也不是纯粹的冗余

保留它们有实际价值：

- **接口可以独立使用**：以后写单元测试想单独测 `process_lane_lines`，有默认值就能直接调用
- **它们在文档化"这个参数的合理默认是 3"**，函数签名本身就是文档
- **工厂的默认值让 `create_processor("edges")` 合法**——edges 处理器根本不用 `max_lines`，强迫调用者传个对它无意义的参数是噪音

> **顺带确认做对的地方**：这三个默认值**只在声明处出现一次**，[processor.cpp](../../src/processor.cpp) 里的定义都没重复写 `= 3`。这是对的——C++ 规定同一个函数在同一作用域内默认实参只能给一次，写两遍会直接编译错误。

### 更值得修的重复：那个 `1..256`

范围检查写了四遍：

| 位置 | 代码 |
| --- | --- |
| [vision_cli.cpp:42](../../apps/vision_cli.cpp#L42) | `if (number < 1 \|\| number > 256)` |
| [processor.cpp:11](../../src/processor.cpp#L11) | `if (max_lines_ < 1 \|\| max_lines_ > 256)` |
| [processor.cpp:42](../../src/processor.cpp#L42) | `if (max_lines < 1 \|\| max_lines > 256)` |
| [processor.cpp:109](../../src/processor.cpp#L109) | `if (max_lines < 1 \|\| max_lines > 256)` |

这比默认值重复更麻烦——它是**逻辑重复**：四处判断条件必须完全一致，将来改范围漏改一处，就会出现"配置检查通过、对象构造拒绝"这种自相矛盾的行为。

不过也要说句公道话：这四处**各自都有防御的理由**（每个入口都能被独立调用，谁都不该假设调用者已经检查过）。所以正确做法不是删掉三处，而是**把数字提取成常量**。

### 建议

**方案 A（推荐，改动最小收益最大）**：在 [processor.hpp](../../include/processor.hpp) 顶部定义常量，四处引用：

```cpp
inline constexpr int kDefaultMaxLines = 3;
inline constexpr int kMinMaxLines    = 1;
inline constexpr int kMaxMaxLines    = 256;

explicit LaneLinesProcessor(int max_lines = kDefaultMaxLines);
ProcessResult process_lane_lines(const cv::Mat& image, int max_lines = kDefaultMaxLines);
std::unique_ptr<IProcessor> create_processor(const std::string& name,
                                             int max_lines = kDefaultMaxLines);
```

`inline constexpr` 是 C++17 的写法——头文件里定义变量要加 `inline`，否则多个 `.cpp` 包含它会重复定义。

**方案 B（更严格的架构分层）**：只删工厂那个 `= 3`，强制调用者明确给值。理由是 main 本来就传 `config.max_lines`，工厂的默认值在这个程序里没有用武之地，却制造了"第二权威"；保留 `process_lane_lines` 和 `LaneLinesProcessor` 的默认值作为"库的便利"。

**不建议**三个全删——接口会变啰嗦，而且没解决真正的重复（数字还是会散落）。

### 相关陷阱：默认实参 + 虚函数

项目里有 `IProcessor` 基类，提前记一个坑：**默认实参不参与动态绑定**。

```cpp
struct Base    { virtual void f(int x = 1); };
struct Derived : Base { void f(int x = 2) override; };

Base* p = new Derived;
p->f();   // 调用的是 Derived::f，但传入的是 1 —— Base 的默认值！
```

原因：默认实参是**编译期按指针/引用的静态类型**补齐的；实际执行哪个函数体是运行期的虚函数分发。**两者依据的信息不同。** 所以业界惯例是：**虚函数不要给默认实参**。现在 `IProcessor::process` 无参，安全。

---

## 问题 4：`std::runtime_error` 是什么函数？

### 直接纠正：它不是函数，是一个类

`std::runtime_error` 是**类型**（class）。代码里的 `std::runtime_error("...")` 看起来像函数调用，其实是在**构造一个对象**——调用的是它的构造函数。

| | 函数 | 构造函数 |
| --- | --- | --- |
| 返回类型 | 有（`int`、`void`…） | **没有**，也不写 |
| 名字 | 随便起 | **必须和类名一样** |
| 作用 | 算出一个值 | 造出一个对象 |
| 调用样子 | `f(x)` | `类名(x)` ← 看起来一样 |

### 本机头文件里的真身

```text
class runtime_error : public exception        // /usr/include/c++/9/stdexcept:219
{
    __cow_string _M_msg;                      // 内部存了一份错误消息

public:
    explicit runtime_error(const string& __arg);   // :226
    explicit runtime_error(const char*);           // :230

    virtual ~runtime_error() noexcept;             // :244
    const char* what() const noexcept;             // :249
};
```

三点值得注意：

1. **派生自 `std::exception`** —— 这是它能被通用 `catch` 接住的原因
2. **内部存了一个字符串**（`_M_msg`），你传进去的那句话被**拷贝**进去了，所以它自己活多久不依赖你传的临时对象
3. **构造函数是 `explicit`** —— 阻止 `std::runtime_error e = "msg";` 这种隐式转换。和 [processor.hpp:35](../../include/processor.hpp#L35) 的 `explicit LaneLinesProcessor(...)` 是同一个机制

> [vision_cli.cpp:19](../../apps/vision_cli.cpp#L19) 传的是 `"cannot open config: " + path`，`const char*` 加 `std::string` 得到 `std::string`，所以命中的是 `const string&` 那个重载。

### 标准库异常体系

```text
std::exception                        ← 所有标准异常的根
│
├── std::logic_error                  (:113)   「程序员的错」
│   ├── std::invalid_argument         (:168)   ← 已在用
│   ├── std::domain_error             (:153)
│   ├── std::length_error             (:184)
│   └── std::out_of_range             (:200)
│
└── std::runtime_error                (:219)   ← 本次提问
    ├── std::range_error              (:258)
    ├── std::overflow_error           (:273)
    └── std::underflow_error          (:288)
```

（另有 `std::bad_alloc`、`std::system_error` 等直接从 `exception` 派生，不在 `<stdexcept>` 里。）

**两大家族的语义分工：**

- **`logic_error`** —— **调用者的错**。理论上运行前就能查出来：参数不合法、下标越界、字符串长度非法。属于"程序写错了"
- **`runtime_error`** —— **外部状况**。运行前无法预知：文件不存在、磁盘满、网络断了、权限不对。属于"环境出问题了"

**项目里两族都用对了：**

| 代码位置 | 抛出类型 | 归类 |
| --- | --- | --- |
| [vision_cli.cpp:19](../../apps/vision_cli.cpp#L19) 打不开配置文件 | `runtime_error` | 外部状况 ✅ |
| [vision_cli.cpp:184](../../apps/vision_cli.cpp#L184) 打不开输出文件 | `runtime_error` | 外部状况 ✅ |
| [vision_cli.cpp:29](../../apps/vision_cli.cpp#L29) 未知配置字段 | `invalid_argument` | 调用者笔误 ✅ |
| [vision_cli.cpp:38](../../apps/vision_cli.cpp#L38) max_lines 类型不对 | `invalid_argument` | 调用者给错 ✅ |
| [processor.cpp:12](../../src/processor.cpp#L12) max_lines 超范围 | `invalid_argument` | 调用者给错 ✅ |

这个分类不是形式主义——调用方据此决定怎么处理：`invalid_argument` 说明**喂数据的那段代码有 bug**，不该重试；`runtime_error` 说明**环境不对**，换个路径重试可能就好了。

### `throw` 也不是函数，是关键字

```cpp
throw std::runtime_error("cannot open config: " + path);
```

`throw` 是**语句关键字**，后面跟的是一个**表达式**。整件事分三步：

1. **构造**出 `runtime_error` 对象（就在这一行求值）
2. 把它交给**异常机制**（放到一块特殊内存，不是普通栈上）
3. **栈展开**（stack unwinding）：从 throw 点开始一层层退出函数作用域，沿途**每个局部对象都被析构**

第 3 步是项目里一个重要的隐性保障。若异常从 `load_config` 里抛出，函数开头创建的 `std::ifstream file(path)` 会在栈展开时**自动析构 → 自动关闭文件**。没写 `file.close()` 也不会泄漏。

> 这就是 RAII（Resource Acquisition Is Initialization）：资源生命周期绑在对象上，对象销毁 → 资源释放。异常路径和正常路径走的是同一套清理逻辑，所以不会"忘了关"。

### 和 catch 连起来看

[vision_cli.cpp:196-202](../../apps/vision_cli.cpp#L196-L202)：

```cpp
} catch (const cv::Exception& error) {
    std::cerr << "OpenCV error: " << error.what() << "\n";
    return 1;
} catch (const std::exception& error) {
    std::cerr << "vision_cli: " << error.what() << "\n";
    return 1;
}
```

第二个 `catch` 能接住 `runtime_error` 和 `invalid_argument`，**因为它们都继承自 `std::exception`**。

`error.what()` 能打印出你写的那句话，是因为 **`what()` 是虚函数**（头文件 :249）。按 `const std::exception&` 引用调用，实际执行的是 `runtime_error::what()`，返回存下来的 `_M_msg`。这就是多态。

**两个必须知道的细节：**

**① catch 顺序不能反——具体在前，通用在后**

`cv::Exception` 也派生自 `std::exception`。`catch` 的规则是**自上而下、第一个匹配的就进、进完跳过剩下的**。所以把 `std::exception` 写在前面：

```cpp
} catch (const std::exception& error) {   // 所有异常都被这里吃掉
} catch (const cv::Exception& error) {    // 永远到不了 —— 死代码
```

编译能过，但第二个分支永远不执行。**这是 C++ 里非常经典的静默 bug。**

**② catch 必须用引用，不能按值**

```cpp
} catch (std::exception error) {   // ❌ 漏掉 &
```

按值捕获会发生**对象切片**（object slicing）：`runtime_error` 对象被"切"成只剩基类 `std::exception` 那部分，消息 `_M_msg` 连同派生类的虚函数表一起丢了，`what()` 只会返回一句无意义的通用文本。

用 `const` 是因为你只读它，不改。

### 和其他语言类比

| 语言 | 写法 | 差异 |
| --- | --- | --- |
| C++ | `throw std::runtime_error("msg")` | **没有 `new`** |
| Java | `throw new RuntimeException("msg")` | 要 `new` |
| Python | `raise ValueError("msg")` | 也是类 |

C++ 这里没有 `new`，直接就地构造一个对象交给异常机制。这也是为什么**不推荐** `throw new std::runtime_error(...)`：那样抛出的是**指针**，谁来 `delete` 它就成了问题；直接抛对象由异常机制统一管理生命周期。

所以 C++ 的惯例是**抛值、按引用接**（`throw T(...)` + `catch (const T&)`）。这和问题 3 末尾那条"默认实参不参与动态绑定"是一对：**类型系统在编译期决定的事（补默认值、按值拷贝）都不会随多态走，只有通过引用/指针的虚函数调用才会。**

### 小结

- `std::runtime_error` **是类，不是函数**；`std::runtime_error("...")` 是**构造函数调用**
- 它派生自 `std::exception`，内部存一份错误消息，`what()` 是虚函数负责取出来
- 它属于"**外部状况出错**"那一族；"调用者传错参数"那一族用 `std::invalid_argument`（派生自 `logic_error`）
- `throw` 是关键字，抛的是**值**；`catch` 要**按 const 引用接**，且**从具体到通用排序**
- 使用它只需要 `#include <stdexcept>` —— [vision_cli.cpp](../../apps/vision_cli.cpp) 已经包含

---

## 附：本课关键代码与术语速查

| 位置 | 代码 | 作用 |
| --- | --- | --- |
| [vision_cli.cpp:107](../../apps/vision_cli.cpp#L107) | `cv::imread(input_path, cv::IMREAD_COLOR)` | CWD 相对路径；失败返回空 Mat，不抛异常 |
| [vision_cli.cpp:13](../../apps/vision_cli.cpp#L13) | `int max_lines = 3;` | 程序级默认值的**唯一权威** |
| [processor.hpp:35,50,51](../../include/processor.hpp#L35) | `= 3` 默认实参 | 接口便利默认，当前程序从不走这条路径 |
| [vision_cli.cpp:42](../../apps/vision_cli.cpp#L42) + [processor.cpp:11,42,109](../../src/processor.cpp#L11) | `if (... < 1 \|\| ... > 256)` | 四处逻辑重复，应提取为常量 |
| [vision_cli.cpp:19](../../apps/vision_cli.cpp#L19) | `throw std::runtime_error("cannot open config: " + path)` | 外部状况（文件打不开） |
| [vision_cli.cpp:38](../../apps/vision_cli.cpp#L38) | `throw std::invalid_argument("max_lines must be an integer")` | 调用者给错（类型/范围） |
| [vision_cli.cpp:196-202](../../apps/vision_cli.cpp#L196-L202) | 两个 `catch`，`cv::Exception` 在前 | 具体在前、通用在后 |

| 术语 | 含义 |
| --- | --- |
| CWD（当前工作目录） | 相对路径的解析起点；属于**进程**，不属于可执行文件 |
| PATH 搜索 | shell 查找命令的机制；`.` 通常不在其中，故需 `./a.out` |
| 字典序 | `std::map<std::string, ...>` 的遍历顺序，`nlohmann::json` 默认如此 |
| 无序对象 | RFC 8259 规定 JSON 对象不保证字段顺序，取字段应靠键名 |
| 默认实参 | 编译期在调用点补齐，零运行时开销；不参与虚函数动态绑定 |
| 权威来源 | 同一个默认值只应有一处定义，其余引用它 |
| `std::exception` | 所有标准异常的基类；按引用捕获它能接住全部派生异常 |
| 对象切片 | 按值捕获派生异常导致派生部分丢失，`what()` 失效 |
| 栈展开 | 异常传播时逐层析构局部对象，RAII 因此对异常路径同样有效 |
