# 模块 9.1：有界图片任务队列与背压

> 状态：2026-10-05 已完成。学生已运行成功、回答集中检查，并明确确认理解等锁与内存预算的补充。接续 [09-2 工作线程、完成通知与 epoll 接入](09-2-worker-pool-epoll.md)；本页保留本课教材与历史反馈。

## 1. 接续位置与本课目标

你已说明：了解线程相关概念，写过一个简单线程池，使用的是无界任务队列。本课直接从无界队列的缺口开始，不重讲线程创建入门，也不重复询问熟悉程度。

模块 8 的 epoll 服务端和两连接观察已完成。当前 `apps/vision_epoll_server.cpp` 在 read_step 收齐图片后仍同步调用：

```cpp
connection.output = make_response(connection.header, connection.body);
```

只读核对时，这段仍在事件循环线程里执行。为了随后把它交给工作线程，先完成一个独立、可复用的知识点：**带容量限制、拒绝提交和关闭语义的图片任务队列**。

本课完成：

- 固定排队容量，提交区分 accepted、full、closed。
- 队列满时不等待空位，返回给调用者决定下一步。
- 提交失败时，图片字节仍留在调用者手中。
- 消费者在空队列上等待，有任务或关闭时醒来。
- 关闭后拒绝新任务，已有任务继续被取出，排空后消费者退出。
- 用集中观察程序验证容量、顺序、拒绝后的所有权和关闭行为。

本课完整代码在讲义中，由你亲手写入工程。老师仅准备本讲义、CMake 配套和进度记录，不配置、构建、运行或测试课程。

模块 9 分为连续的完整知识点：09-1 先实现有界任务队列；确认理解后，09-2 再把工作线程、结果通知和连接状态接入 epoll。当前还没有把图像计算移出事件循环，也没有实现服务器 busy 响应。

## 2. 无界队列把什么问题推迟了

用一组假设数字演算：每秒收到 100 个任务，两条工作线程各用 100 毫秒处理一个任务。理想情况下，每条线程每秒处理 10 个，两条合计 20 个。

```text
每秒进入：100
每秒完成：20
每秒新增积压：80
持续10秒后：约800个排队任务
```

假设每个任务拥有 1 MiB 压缩图片，仅这些排队图片就约占 800 MiB，尚未计算解码后的像素、连接缓冲区和其他对象。

这是解释无界队列的假设，不是当前 8 连接服务端的实测结果。增加队列长度可以吸收短时突发，不能让长期处理能力从 20 个/秒变成 100 个/秒。

有界队列规定：例如最多让 8 个任务排队。满了之后，系统必须把“暂时接不下”反映给上游，这就是本课讨论的背压或过载反馈。

**返回 full 本身只是队列接口的反馈。** 调用方还需要采取行动，才能形成完整的服务端过载策略。

## 3. 队列满了，为什么本课选择拒绝

| 策略 | 提交者会怎样 | 在本项目中的考虑 |
| --- | --- | --- |
| 等待空位 | 一直等某个消费者取任务 | 若提交者是 epoll 线程，期间无法处理其他网络事件 |
| 返回 full | 调用者得到失败状态并继续执行 | 本课采用；后续可构造忙碌响应 |
| 丢弃旧任务 | 旧请求被新请求替换 | 必须另行约定旧请求结果，不能悄悄丢掉 |
| 调用者自己执行 | 提交线程直接做图像计算 | 又把耗时计算放回事件循环 |

本课不等待“队列有空位”，但内部仍会获取 mutex；锁竞争和容器分配可能耗时。**try_push 不是无锁接口，也不承诺严格实时或绝不阻塞。** 它保证的是满队列不会让生产者等消费者腾位置。

所有锁内工作应尽量短，图像解码和处理放到取出任务之后。互斥锁的 lock 可能等待持有者释放，见 [C++ 标准草案的 mutex 规定](https://eel.is/c++draft/thread.mutex.requirements.mutex)。

## 4. 先明确队列的契约

### 4.1 提交结果不是“任务执行结果”

```cpp
enum class SubmitResult {
    accepted,
    full,
    closed
};
```

| 返回值 | 意义 | 本次是否移动走传入任务 |
| --- | --- | --- |
| accepted | 成功进入待处理队列 | 是 |
| full | 当前排队数达到容量 | 否 |
| closed | 队列不再接收新任务 | 否 |

accepted 不保证任务已经开始或已经成功完成。full 是正常的过载分支，不需要拿异常表达。内存分配、系统锁操作等异常仍可能抛出，不会假装成 accepted。

### 4.2 容量数的是“还在排队”的任务

容量 K=2 时：

```text
[A, B] → 满了，C提交失败
取出A → [B]
C再提交 → [B, C]
```

A 一被取走，就不再占队列位置，即使它还在计算。

若一个线程池有 W 个工作线程、每条线程至多持有一个任务，且没有其他预取缓存，那么它可以同时有最多 K 个排队任务加 W 个线程正在持有的任务。K=8、W=2 时是最多 10 个，不能把队列容量误认为“全系统最多 8 个请求”。

本课只限制任务数量，没有按字节设置队列预算。假设上游限定每份压缩图片最多 P 字节，队列中的图片内容至多约 K×P；连接接收缓冲区、工作线程的 Mat、已完成结果还需要单独考虑。后续接入时继续讨论这些边界。

### 4.3 关闭后仍排空已有任务

| 队列状态 | try_push | wait_pop |
| --- | --- | --- |
| 开放、非空 | 有空位则接受，否则 full | 取出队首，返回 true |
| 开放、为空 | 接受 | 等待任务或关闭 |
| 关闭、非空 | closed | 继续取出已有任务，返回 true |
| 关闭、为空 | closed | 返回 false，让消费者退出 |

close 的含义是“停止接收新任务”；它不清空已有数据、不取消正在计算的任务，也不等待线程退出。调用方还要 join 工作线程后，才能销毁队列。

这属于队列自身的生命周期约定；操作系统信号和完整进程优雅停止仍在模块 10 讲解。

## 5. 文件与动手位置

全部文件都在 `/home/rikanshin/VisionFlow/VisionFlow_learn`。

| 文件 | 本课内容 | 谁写入 |
| --- | --- | --- |
| include/image_task_queue.hpp | ImageTask、SubmitResult、队列类声明 | 你 |
| src/image_task_queue.cpp | 加锁入队、等待出队和关闭 | 你 |
| apps/task_queue_demo.cpp | 集中观察程序 | 你 |
| CMakeLists.txt | 条件式 task_queue_demo 目标及线程依赖 | 老师 |
| lessons/lesson9/09-1-bounded-task-queue.md | 当前完整讲义 | 老师 |

这些是三个新增源码文件，不是在已有 epoll 入口里追加。现有服务端继续保留，09-2 才改接任务提交和结果回传。

## 6. include/image_task_queue.hpp 完整内容

由你新建该文件，写入下面内容：

```cpp
#pragma once

#include "protocol.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace visionflow::jobs {

struct ImageTask {
    std::uint64_t connection_token = 0;
    protocol::Header header{};
    std::vector<std::uint8_t> image_bytes;
};

enum class SubmitResult {
    accepted,
    full,
    closed
};

class ImageTaskQueue {
public:
    explicit ImageTaskQueue(std::size_t capacity);

    ImageTaskQueue(const ImageTaskQueue&) = delete;
    ImageTaskQueue& operator=(const ImageTaskQueue&) = delete;

    SubmitResult try_push(ImageTask&& task);
    bool wait_pop(ImageTask& task);
    void close();

private:
    const std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable not_empty_;
    std::deque<ImageTask> tasks_;
    bool closed_ = false;
};

} // namespace visionflow::jobs
```

### 6.1 ImageTask 只携带计算需要的数据和标识

| 字段 | 类型 | 用途 |
| --- | --- | --- |
| connection_token | uint64_t | 以后将计算结果关联回原连接 |
| header | protocol::Header | 已解释的消息类型、请求编号、体长 |
| image_bytes | vector<uint8_t> | 这份任务拥有的压缩图片字节 |

ImageTask 位于 visionflow::jobs 命名空间，protocol::Header 对应已存在的 visionflow::protocol::Header。

本课没有把 Connection& 或 socket fd 交给工作线程。连接可能在计算期间关闭；后续会按 token 查找仍有效的连接，网络操作仍由事件循环负责。这是下一知识点要完成的连接方式，本课先保留所需字段。

队列只负责存取任务，不校验 VFLW，也不确认字节是不是 JPEG。之后由网络入口收齐并校验请求，再创建任务；观察程序则使用几字节样本，仅观察所有权和顺序。

### 6.2 三个方法的参数与返回值

```cpp
SubmitResult try_push(ImageTask&& task);
bool wait_pop(ImageTask& task);
void close();
```

- try_push 接收待转移的任务，返回提交状态。右值引用让它能接收 std::move(task)，但只有真正执行移动时才取走内容。
- wait_pop 通过可修改引用把队首任务交给调用方。true 表示拿到一个任务；false 表示队列已关闭且排空。false 时不改调用方的 task。
- close 没有参数和返回值，修改队列状态并唤醒消费者。多次调用仍保持关闭状态。

构造函数的 capacity 是排队容量，要求大于 0；传入 0 会抛 invalid_argument，避免出现“永远没有空位”的模糊配置。

### 6.3 成员各自保护什么

```cpp
const std::size_t capacity_;
std::mutex mutex_;
std::condition_variable not_empty_;
std::deque<ImageTask> tasks_;
bool closed_ = false;
```

capacity_ 在构造后不变。tasks_ 和 closed_ 的所有并发访问都放在同一把 mutex_ 下。

deque 允许从尾部加入、从头部移除。用 push_back 配合 front/pop_front 实现先进先出；多个工作线程将来仍可能以不同顺序完成计算。

not_empty_ 用来让消费者等待“有任务或已关闭”。本课生产者满了立即得到 full，所以没有额外的 not_full 条件变量。

类禁止复制，避免把同步对象及其保护的数据当成独立副本来使用。

## 7. src/image_task_queue.cpp 完整内容

由你新建该文件，写入下面内容：

```cpp
#include "image_task_queue.hpp"

#include <stdexcept>
#include <utility>

namespace visionflow::jobs {

ImageTaskQueue::ImageTaskQueue(std::size_t capacity)
    : capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("任务队列容量必须大于 0");
    }
}

SubmitResult ImageTaskQueue::try_push(ImageTask&& task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return SubmitResult::closed;
        }
        if (tasks_.size() >= capacity_) {
            return SubmitResult::full;
        }
        tasks_.push_back(std::move(task));
    }
    not_empty_.notify_one();
    return SubmitResult::accepted;
}

bool ImageTaskQueue::wait_pop(ImageTask& task) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this] {
        return closed_ || !tasks_.empty();
    });
    if (tasks_.empty()) {
        return false;
    }
    task = std::move(tasks_.front());
    tasks_.pop_front();
    return true;
}

void ImageTaskQueue::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    not_empty_.notify_all();
}

} // namespace visionflow::jobs
```


## 8. try_push：检查容量与入队是同一个受保护操作

### 8.1 先上锁，再决定提交结果

```cpp
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return SubmitResult::closed;
    }
    if (tasks_.size() >= capacity_) {
        return SubmitResult::full;
    }
    tasks_.push_back(std::move(task));
}
```

lock_guard 构造时取得 mutex_，离开这个大括号作用域时释放。两处提前 return 也会销毁 lock，不会因为提前退出而忘记解锁。

closed_ 优先检查，所以“已经关闭且原来正好满了”返回 closed。它告诉调用者这个队列以后也不再接收任务；full 则仅表示当前没有位置。

检查 size 和 push 必须放在同一把锁内。假设容量 2、当前只有 1 项，如果两个生产者各自先在锁外看到“还有一个位置”，随后都追加，就可能变成 3 项；正确做法是第一个检查并追加之后，第二个再在锁内重新检查。

这也是为什么不提供“先调用 full()、再调用 push()”让调用者拼接容量判断：两个调用之间，状态可能已经改变。

### 8.2 std::move 并不会提前清空任务

调用写法：

```cpp
const auto result = queue.try_push(std::move(task));
```

std::move 使 task 能绑定到 ImageTask&& 参数。参数仍然引用调用者的那份对象，没有在进入函数时构造另一个 ImageTask。

真正的资源转移发生在：

```cpp
tasks_.push_back(std::move(task));
```

当代码在前两个 if 返回时，这句还没有执行，所以图片字节保持原样。成功进入队列后，后续由队列内任务持有图片容器，调用者不要再依赖原 task 的具体内容。

如果把函数参数改成按值接收的 `try_push(ImageTask task)`，调用 std::move(original) 时可能在函数检查容量之前，就把 original 移进参数对象。此时返回 full 也不能声称原对象没变。这就是本课选择右值引用并推迟移动的原因。

这里承诺的是正常返回 full/closed 时不移动任务；系统异常和内存分配异常仍通过异常机制报告，不把所有失败都压成一种状态。

### 8.3 解锁后通知消费者

```cpp
not_empty_.notify_one();
return SubmitResult::accepted;
```

notify_one 至多唤醒一个正在等待的消费者。我们刚放入一个任务，只需通知一个线程来取。

队列里的真实任务才是状态；通知不是一份持久保存的“任务凭证”。如果此时没有消费者等待，未来消费者会在拿锁后看到队列非空，直接取走，不需要补发通知。

把通知放在锁外，能让醒来的消费者有机会直接取得锁。并不是把通知移到锁外就能忽略同步：任务的检查和修改仍必须在锁内。

## 9. wait_pop：消费者等待的是一个条件

### 9.1 为什么使用 unique_lock

```cpp
std::unique_lock<std::mutex> lock(mutex_);
not_empty_.wait(lock, [this] {
    return closed_ || !tasks_.empty();
});
```

unique_lock 管理同一把 mutex_，同时允许 condition_variable 在等待时释放锁，醒来后重新取得锁。lock_guard 不提供这里需要的等待配合接口。

谓词 `[this] { ... }` 是捕获当前对象指针的 lambda，使它能读取本队列的 closed_ 和 tasks_。这些读取发生在持锁时。

它在以下任一条件成立时允许继续：

- 队列有任务，可以取一个。
- 队列已关闭，需要决定排空或退出。

### 9.2 wait 期间会释放 mutex

若“未关闭且队列为空”，wait 会释放锁并等待。生产者才能取得 mutex、放入任务、解锁、发通知。消费者返回 wait 前，会重新持有锁。

谓词版本会反复检查条件；即使没有符合业务条件的唤醒，或别的消费者先取走了任务，也不会直接操作空队列。可把它理解成“条件不成立就继续 wait”。这些等待与重锁语义见 [C++ condition_variable 规定](https://eel.is/c++draft/thread.condition.condvar)。

程序依赖的是受同一把锁保护的状态和谓词，而不是“这次肯定收到了一个对应自己的通知”。因此关闭或入队先于消费者开始等待，也能正确工作。

### 9.3 为什么醒来后还检查 empty

```cpp
if (tasks_.empty()) {
    return false;
}
task = std::move(tasks_.front());
tasks_.pop_front();
return true;
```

wait 已保证 closed_ 或非空成立。此时若仍为空，就只能是关闭状态，所以返回 false；开放空队列不会从谓词等待中正常继续到这里。

如果 closed_=true 但还有任务，empty 为假，仍然正常出队。这实现“先拒绝新任务，再排空旧任务”。

front 取得队首引用；移动赋值把队首任务的内容交给调用者；pop_front 删除容器中的原条目。两步都在锁内，所以其他消费者不会同时取得同一个队首。

返回 true 之前，局部 lock 会析构并解锁；工作线程接下来做图像处理时已经离开队列临界区。

### 9.4 不要在持锁期间执行任务

未来工作线程的结构应是：

```cpp
for (;;) {
    ImageTask task;
    if (!queue.wait_pop(task)) {
        break;
    }
    // 此处才解码和处理图片，队列锁已经释放。
}
```

这个片段表达调用顺序，当前新文件的完整代码以下文为准。

若在 queue 的锁内执行耗时算法，所有生产者和其他消费者都会等这把锁，线程池就很难发挥作用。每轮新建局部 task，还能在本轮结束时释放这份任务持有的输入字节。

## 10. close：关闭状态与线程退出是两件事

```cpp
void ImageTaskQueue::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    not_empty_.notify_all();
}
```

修改 closed_ 和提交／取出共用 mutex，保证并发顺序清楚：若某次提交先在锁内完成入队，它可被接受并随后排空；若关闭先发生，后续提交得到 closed。不能仅按两个函数在终端上打印的先后猜测实际入队顺序。

关闭要 notify_all，因为可能有多条工作线程在等任务。只唤醒一个可能让其他线程仍然等下去，导致调用方 join 无法结束。

关闭后的队列不能重新开放；多次 close 只是保持 true，再次通知不会新增任务。

拥有队列和工作线程的调用方应按顺序：

```text
不再接收新任务
    ↓
queue.close()
    ↓
工作线程取完已有任务，wait_pop 返回 false
    ↓
join 所有工作线程
    ↓
销毁队列
```

close 不会强制中止已经执行的计算。如果某个任务本身一直不结束，join 仍可能一直等；那属于任务执行与完整停止策略的问题，不能把 close 当成强制杀线程。

本类没有一个“析构时自动通知就一定安全”的快捷实现。通知之后工作线程还可能访问队列，所以仍必须在销毁前完成线程回收。

## 11. 用容量 2 完整演算一遍

下面是集中观察程序采用的顺序。前半段在启动消费者之前执行，使队列满的分支不依赖线程调度速度，也不需要 sleep 猜时间。

| 操作 | 排队任务编号 | 返回值或结果 |
| --- | --- | --- |
| 提交 101 | [101] | accepted |
| 提交 102 | [101, 102] | accepted |
| 提交 103 | [101, 102] | full，103 字节仍在调用者 |
| 取出队首 | [102] | 得到完整任务 101 |
| 再提交 103 | [102, 103] | accepted |
| 启动消费者并关闭入口 | 逐步排空 | 消费者取出 102、103 后退出 |
| 提交 104 | []，已关闭 | closed，104 内容仍在调用者 |
| 关闭且排空后再取 | [] | false |

这个演算里的 connection_token 分别是 11～14，request_id 是 101～104；它们只是观察数据，没有对应的真实网络连接。

观察程序用几字节 vector 检查内容，没有 JPEG 解码、没有图片算法，也没有网络请求。不要把“队列观察完成”当成“多线程网络服务已完成”。

## 12. apps/task_queue_demo.cpp 完整内容

由你新建该文件，写入下面内容。require 是本观察程序自己定义的检查函数，失败会显示具体原因并返回 1。

```cpp
#include "image_task_queue.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace jobs = visionflow::jobs;
namespace protocol = visionflow::protocol;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 仅用于本课观察：启动一个消费者，关闭入口，排空队列并回收线程。
std::vector<std::uint64_t> drain_with_worker(jobs::ImageTaskQueue& queue) {
    std::vector<std::uint64_t> processed;
    std::exception_ptr worker_error;
    std::thread worker([&] {
        try {
            for (;;) {
                jobs::ImageTask task;
                if (!queue.wait_pop(task)) {
                    break;
                }
                // 在队列锁之外使用取出的任务。本课只记录编号，不解码图片。
                processed.push_back(task.header.request_id);
            }
        } catch (...) {
            worker_error = std::current_exception();
        }
    });

    try {
        queue.close();
        worker.join();
    } catch (...) {
        queue.close();
        if (worker.joinable()) {
            worker.join();
        }
        throw;
    }
    if (worker_error) {
        std::rethrow_exception(worker_error);
    }
    return processed;
}

} // namespace

int main() {
    try {
        jobs::ImageTaskQueue queue(2);
        jobs::ImageTask first{
            11, {protocol::Type::image, 101, 4}, {10, 20, 30, 40}
        };
        jobs::ImageTask second{
            12, {protocol::Type::image, 102, 3}, {50, 60, 70}
        };
        jobs::ImageTask third{
            13, {protocol::Type::image, 103, 2}, {80, 90}
        };

        require(queue.try_push(std::move(first)) == jobs::SubmitResult::accepted,
                "第一个任务应被接受");
        require(queue.try_push(std::move(second)) == jobs::SubmitResult::accepted,
                "第二个任务应被接受");
        std::cout << "容量为 2，已接受任务 101、102\n";

        require(queue.try_push(std::move(third)) == jobs::SubmitResult::full,
                "队列已满时应拒绝第三个任务");
        require(third.image_bytes == std::vector<std::uint8_t>({80, 90}),
                "拒绝提交后，调用者的图片字节应保持原样");
        std::cout << "任务 103 被拒绝：队列已满，原来的 2 字节仍在\n";

        jobs::ImageTask taken;
        require(queue.wait_pop(taken), "队列中应能取出第一个任务");
        require(taken.connection_token == 11 &&
                    taken.header.request_id == 101 &&
                    taken.image_bytes ==
                        std::vector<std::uint8_t>({10, 20, 30, 40}),
                "取出的任务内容不正确");
        std::cout << "取出任务 101，释放一个排队位置\n";

        require(queue.try_push(std::move(third)) == jobs::SubmitResult::accepted,
                "释放位置后，第三个任务应被接受");
        std::cout << "重新提交任务 103：已接受\n";

        const auto processed = drain_with_worker(queue);
        require(processed == std::vector<std::uint64_t>({102, 103}),
                "关闭后应按入队顺序排空剩余任务");
        std::cout << "关闭入口后，工作线程依次取出任务 102、103 并退出\n";

        jobs::ImageTask late{
            14, {protocol::Type::image, 104, 1}, {100}
        };
        require(queue.try_push(std::move(late)) == jobs::SubmitResult::closed,
                "关闭后必须拒绝新任务");
        require(late.image_bytes == std::vector<std::uint8_t>({100}),
                "关闭后拒绝提交不应取走任务内容");
        require(!queue.wait_pop(taken), "关闭且排空后应返回 false");
        queue.close();
        std::cout << "关闭后拒绝新任务；队列排空后取任务返回 false\n";

        jobs::ImageTaskQueue empty_queue(1);
        require(drain_with_worker(empty_queue).empty(),
                "空队列关闭不应产生任务");
        std::cout << "空队列关闭后，工作线程也能退出\n";
        std::cout << "有界任务队列观察完成\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "队列观察失败：" << error.what() << '\n';
        return 1;
    }
}
```



## 13. 观察程序中值得对照的细节

### 13.1 为什么先填满，再启动消费者

若一开始就启动工作线程，生产者提交第三个任务之前，消费者可能已经取走前两个；你就不能稳定观察到 full。

本程序先在主线程提交 101、102，再尝试 103，确定触及容量边界。取出 101 后再重试 103，确定释放的队列位置可被重新使用。真实消费者只负责后半段的排空和退出。

这不是用 sleep 模拟“应该足够慢”，而是用明确执行顺序建立所需的队列状态。

### 13.2 为什么用向量比较检查拒绝后内容

```cpp
require(third.image_bytes == std::vector<std::uint8_t>({80, 90}),
        "拒绝提交后，调用者的图片字节应保持原样");
```

比较的是完整字节内容。仅比较 size=2 不能发现两个字节被错误修改；仅看到 full 也不能证明任务没有提前移动走。

使用 enum class 后，代码明确比较 SubmitResult::full 等枚举值，不会把某个整数的真假误当作是否接受。

### 13.3 消费者取得任务后，队列锁已经释放

```cpp
jobs::ImageTask task;
if (!queue.wait_pop(task)) {
    break;
}
processed.push_back(task.header.request_id);
```

wait_pop 正常返回时，函数内的 unique_lock 已销毁。本行的记录动作发生在锁外，未来在同一个位置调用图像处理也应在锁外。

每轮 task 是消费者自己的对象。消费者不用回到队列中查找 task，也不借用一个可能被其他线程删除的队首引用。

### 13.4 为什么 processed 和 worker_error 没有再加锁

在本观察程序中，这两个变量只由一个工作线程写；主线程在 worker.join() 成功返回后才读取。它们不存在同时读写。

join 等待该线程结束，并提供相应的线程完成同步关系，见 [C++ thread::join 规定](https://eel.is/c++draft/thread.thread.member)。

这不能推广成“多个工作线程同时 push_back 同一个 vector 也安全”。之后的多个工作线程回传结果需要自己的同步队列或其他受保护机制。本课没有省略那部分锁后仍声称多线程网络接入完成。

### 13.5 线程里的异常如何交给主线程

如果异常直接逃出 std::thread 的入口函数，主线程外层的 try/catch 接不到它。

本程序在线程里 catch(...)，通过 std::current_exception() 保存异常到 exception_ptr；主线程 join 后检查这个指针，再用 std::rethrow_exception 重新抛出，让 main 的 catch 输出中文失败上下文。

这样消费者记录编号时若出现异常，不会静默报成功。主线程关闭队列和回收线程的异常分支也保留必要的清理。

### 13.6 空队列关闭观察证明什么

drain_with_worker 先启动消费者，再由主线程关闭入口。具体调度下，消费者可能已经等待，也可能尚未进入 wait。

两种次序都应结束：前者靠关闭通知重新检查，后者直接看到关闭谓词。观察完成可以确认本次配合能退出，但不能单凭这一输出宣称真实睡眠、虚假唤醒和所有线程交错都被覆盖。

## 14. CMake 配套

老师已在 CMakeLists.txt 末尾加入下面的条件式目标，你不需要再追加。三个源码文件由你写齐之后，再自行重新配置。


```cmake
# 模块 9.1：学生写齐队列与观察程序后，自行重新配置生成目标。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/image_task_queue.hpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/image_task_queue.cpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/task_queue_demo.cpp")
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)
    add_executable(task_queue_demo
        apps/task_queue_demo.cpp
        src/image_task_queue.cpp
    )
    target_include_directories(task_queue_demo PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(task_queue_demo PRIVATE Threads::Threads)
endif()
```


find_package(Threads REQUIRED) 查找当前平台的线程支持，Threads::Threads 把需要的编译／链接选项带给目标。THREADS_PREFER_PTHREAD_FLAG 表示在适用的平台优先采用 pthread 编译选项；不手写某个固定库路径。说明见 [CMake FindThreads 文档](https://cmake.org/cmake/help/latest/module/FindThreads.html)。

task_queue_demo 仅需要自己的入口和队列实现。头文件引用 protocol.hpp 中的数据类型，观察程序没有调用 encode/decode/packet，因此不必把 protocol.cpp 或 OpenCV 算法源文件加进该目标。

EXISTS 只检查文件存在，不代表内容正确或构建已通过。老师仅维护配套，不执行配置或编译。

## 15. 由你执行的集中实践

先写齐以下三个文件：

```text
include/image_task_queue.hpp
src/image_task_queue.cpp
apps/task_queue_demo.cpp
```

然后在同一个 WSL 学习工程中执行：

```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
cmake -S . -B build
cmake --build build --target task_queue_demo -j2
./build/task_queue_demo
echo $?
```

本课不需要启动网络服务端。按代码推导的预期输出为：

```text
容量为 2，已接受任务 101、102
任务 103 被拒绝：队列已满，原来的 2 字节仍在
取出任务 101，释放一个排队位置
重新提交任务 103：已接受
关闭入口后，工作线程依次取出任务 102、103 并退出
关闭后拒绝新任务；队列排空后取任务返回 false
空队列关闭后，工作线程也能退出
有界任务队列观察完成
0
```

这些是预期，不是老师运行得到的记录。若某一步失败，请贴对应命令和完整输出；不要求为了本课重复旧模块的构建与观察。

| 现象 | 对照位置 |
| --- | --- |
| 找不到 task_queue_demo 目标 | 三份源码是否存在，之后是否重新执行配置 |
| 找不到 ImageTaskQueue 方法定义 | src/image_task_queue.cpp 是否写齐，命名空间和签名是否一致 |
| 第三次提交没有得到 full | 容量检查是否在 push 之前；是否提前启动消费者改变了演示顺序 |
| 返回 full 后，third 的字节丢失 | 参数是否误改为按值传入；是否在检查之前移动任务 |
| 关闭队列时丢失 102、103 | wait_pop 是否一见 closed 就退出，忽略尚未排空的任务 |
| 程序一直等，不能结束 | close 是否通知所有消费者，wait 谓词是否包含 closed_ |
| 使用线程时程序直接终止 | 是否有异常逃出线程入口，或 joinable 的线程未经回收就被销毁 |

这些只是定位入口，不替代你的实际报错证据。

## 16. 本知识点的集中理解检查

阅读并完成实践后，集中说明下面四点即可，不逐参数停下来答题。

1. 为什么 epoll 线程提交任务时，队列满了应返回 full，而不能在这里等空位？本接口是否完全不会因 mutex 而等待？
2. 为什么传入 std::move(task) 后，返回 full 时 task 的图片字节仍在？真正的移动发生在哪一句？
3. wait 的谓词为什么是 closed_ || !tasks_.empty()？关闭后队列里还有两项时，消费者应怎样做？
4. 若队列容量为 8、工作线程有 2 条，且每条只持有一个任务，排队数量与该线程池持有的任务总量分别怎样计算？为什么仅靠任务数量上限仍不是全系统内存预算？

## 17. 下一知识点的接续范围

09-1 理解确认后，再准备 09-2：将本队列接入现有 epoll 服务端，并连接工作线程、完成结果通知和连接生命周期。

后续需要处理：任务拒绝时的应用响应、结果返回原连接、计算期间断连时的结果丢弃，以及计算期间的超时状态。现在只列路线，不提前实现。

2026-10-05 实践记录：三份主要源码已由学生写入，老师只读核对与本课示例一致。学生自行运行 task_queue_demo，输出与第 15 节预期一致，echo $? 为 0。实现和本次集中运行观察已完成，集中回答与补充解释见下方记录；确认理解后再进入 09-2。未展示配置／构建输出，也不将本次退出观察记成所有等待和并发交错均已验证。老师未配置、构建、运行或测试。


2026-10-05 理解检查反馈：学生正确解释事件循环不能等队列空位、std::move 不直接移动且 full 在 push_back 前返回、关闭后按 FIFO 排空，以及最多 8 项排队、10 项由线程池持有。老师补充：短临界区不提供 mutex 等待时间上界，调度、竞争和锁内分配都会影响等待；每项最多 16 MiB 时，160 MiB 只是 10 项压缩载荷的内容上界，不是全部内存预算。现有服务端限制 8 条活动连接，每条载荷最多 16 MiB，连接 body 内容合计最多 128 MiB，并非无上限；容器实际分配、解码像素和中间 Mat 另计，移动前后同一缓冲不可重复计数。核心回答正确，两处补充尚待学生确认理解；不记录为学生已独立答对全部边界。09-2 尚未开始。


2026-10-05 最终确认：学生回复“已消化化”。据已有运行输出、集中回答和补充后的理解确认，09-1 完成，不再要求重跑或重答原题。09-2 讲义和 CMake 配套已准备，主要源码与实践仍由学生完成。
