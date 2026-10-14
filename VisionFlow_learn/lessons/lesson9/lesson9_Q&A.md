# 模块 9 集中理解检查：Q&A 总结

> 来源：[09-2 讲义第 17 节「集中理解检查」](09-2-worker-pool-epoll.md)五题。
> 解答日期：2026-10-07。
> 相关代码：[apps/vision_pool_server.cpp](../../apps/vision_pool_server.cpp)、[include/image_worker_pool.hpp](../../include/image_worker_pool.hpp)、[src/image_worker_pool.cpp](../../src/image_worker_pool.cpp)、[src/image_task_queue.cpp](../../src/image_task_queue.cpp)。

---

## 1. 一张图片从 `Connection::body` 到工作线程，再到 `Connection::output`，经历哪些所有权转移？哪条线程负责 send，为什么不让 worker 直接发送？

### 所有权转移链

字节**只跨线程边界换手两次**，其余环节全是 move（偷指针，不复制）：

```
Connection::body
    │ std::move —— 第 1 次跨线程换手
    ▼
ImageTask ──push──> tasks_ deque ──wait_pop──> worker 局部 task
                                                    │
                                                    │ handler_(header, bytes)
                                                    │ 传的是 const&，不转移所有权
                                                    ▼
                                              CompletedTask::output
                                                    │ std::move
                                                    ▼
   Connection::output <── std::move ── start_sending <── completed_ deque
        ▲
        └── 第 2 次跨线程换手
```

| 环节 | 位置 |
|---|---|
| ① 收齐后构造 `ImageTask`，`connection.body` 被掏空 | [vision_pool_server.cpp:269-271](../../apps/vision_pool_server.cpp#L269-L271) |
| ② `try_submit` → `try_push`，进入 deque | [image_worker_pool.cpp:50](../../src/image_worker_pool.cpp#L50) |
| ③ worker `wait_pop`，移入局部变量 | [image_worker_pool.cpp:87-88](../../src/image_worker_pool.cpp#L87-L88) |
| ④ `handler_` 以 `const&` 借用，产出新 vector | [image_worker_pool.cpp:97](../../src/image_worker_pool.cpp#L97) |
| ⑤ 结果存入 `completed_` | [image_worker_pool.cpp:103-104](../../src/image_worker_pool.cpp#L103-L104) |
| ⑥ `take_completed` 用 `swap` 整体取走 | [image_worker_pool.cpp:142](../../src/image_worker_pool.cpp#L142) |
| ⑦ `start_sending(..., std::move(result.output))` | [vision_pool_server.cpp:328](../../apps/vision_pool_server.cpp#L328) |
| ⑧ `write_step` 用 `send()` 写出 | [vision_pool_server.cpp:291](../../apps/vision_pool_server.cpp#L291) |

### send 由谁负责

**事件循环线程**（main loop 调用 `write_step`）。worker 只产出字节，从不碰 socket。

### 为什么不让 worker 直接发送

1. **分片状态无法共享**：非阻塞 socket 上单次 `send` 不保证发完，必须维护 `output + sent`。这个状态存在 `Connection` 里，让多个 worker 并发写同一连接，就得给每个连接加锁。
2. **字节交错**：两个 worker 同时 `send` 同一个 fd，几十 KB 的 JSON 响应会在内核里交错成乱码。
3. **等待机制不匹配**：EAGAIN 后要等 `EPOLLOUT`，而 epoll 只被事件循环持有；worker 拿到 EAGAIN 只能阻塞或自旋，破坏非阻塞模型。
4. **写回本身就是状态机的一部分**：`Phase::sending` 与 `EPOLLOUT` 联动（[vision_pool_server.cpp:473-481](../../apps/vision_pool_server.cpp#L473-L481)），拆出去就要把状态机也拆开。

---

## 2. 为什么要先放入 `completed_`，再写 eventfd？读出的计数为 2 时，能否机械地只取两条结果？

### 顺序不能反

eventfd 是**纯唤醒信号**。若先写信号再放数据：

```
worker:  写 eventfd ──────────►  事件循环被唤醒
         （数据还没放）           take_completed() → 看到空队列
                                  ↓
                                 什么都不取，回到 epoll_wait
                                 而此后没有新任务 → 永远不会再被唤醒
                                 ↓
                                 结果永久卡死
```

所以必须**先发布数据，再发信号**：[push_back](../../src/image_worker_pool.cpp#L104) 在前，[notify_loop](../../src/image_worker_pool.cpp#L106) 在后。

### 不能按计数取两条

[image_worker_pool.cpp:120](../../src/image_worker_pool.cpp#L120) 的注释就是答案：**「数值不用于决定取多少条结果」**。三个原因：

1. **多生产者并发**：两个 worker 各自写 eventfd，计数器与 push 次数没有可靠对应关系。
2. **计数会溢出**：eventfd 是 64 位计数器，[notify_loop 的 EAGAIN 分支](../../src/image_worker_pool.cpp#L74-L76)正是在处理溢出——溢出即丢失，计数不再可信。
3. **本来就不需要**：`take_completed()` 用 `swap` **整体取走**（[第 142 行](../../src/image_worker_pool.cpp#L142)），有多少取多少。

**结论**：eventfd 只回答「**有没有**」，不回答「**有多少**」。

---

## 3. 连接已经因超时被关闭，但 worker 稍后交回结果，如何避免误发给 fd 被复用的新连接？为什么关闭连接时不能立即减少 `outstanding_`？

### 防误发：靠 token，不靠 fd

`next_token++` **单调递增、永不复用**（[vision_pool_server.cpp:198-201](../../apps/vision_pool_server.cpp#L198-L201)）。连接关闭时 `connections.erase(token)`，token 即失效。结果里携带的是 token 而非 fd：

```cpp
// vision_pool_server.cpp:317-323
const auto token = result.connection_token;
const auto it = connections.find(token);
if (it == connections.end() ||
    it->second.phase != Phase::waiting_result) {
    std::cout << "丢弃连接 " << token << " 的过期结果" << std::endl;
    continue;
}
```

即使 fd 号被新连接复用，**token 也匹配不上** → 丢弃。第二重保险是 `phase != Phase::waiting_result` 检查：万一 token 尚存但连接已转入其他阶段。

### 关闭时不能立即减 `outstanding_`

`outstanding_` 数的是「**已提交、尚未交回**」的任务。**连接关闭不改变 worker 还在算这个事实**——任务仍在队列中或 worker 手里，图片字节仍在内存里。

提前递减会让 `max_outstanding` 低估真实在飞量，从而**允许超发**，破坏资源预算。

递减只能发生在 [take_completed:147](../../src/image_worker_pool.cpp#L147)——交回多少减多少：

```cpp
outstanding_ -= ready.size();
```

[第 144-146 行](../../src/image_worker_pool.cpp#L144-L146)的 `ready.size() > outstanding_` 检查就是这条不变量的运行时自检。

---

## 4. 任务队列容量为 2、工作线程为 2，为什么还要有 `outstanding <= 4`？完成队列用了 deque，为何正常运行不会无限增加？这是否就是整个进程的内存上限？

### 为什么需要 `outstanding <= 4`

**因为 `queue_capacity` 只约束队列，管不了 worker 手里那个。**

worker `wait_pop` 把任务移出队列后，队列立刻空出一个位置——但那个任务的图片**还在 worker 手里**。所以真实在飞量是：

```
2（队列中排队）+ 2（worker 正在算）= 4
```

`max_outstanding = worker_count + queue_capacity`（[image_worker_pool.hpp:33-34](../../include/image_worker_pool.hpp#L33-L34)）。没有它的话，那额外的 2 个**根本没被计数**。

`outstanding_` 从 `try_submit` 成功那一刻起计（[image_worker_pool.cpp:52](../../src/image_worker_pool.cpp#L52)），覆盖「排队中」和「正在算」两个阶段。

### deque 为何不会无限增长

三个条件共同封顶：

1. `take_completed()` 用 `swap` 整体清空（[第 142 行](../../src/image_worker_pool.cpp#L142)），每轮事件循环都会取走。
2. `outstanding_ <= 4` 保证任何时刻未交回的任务不超过 4 个。
3. 每次 `push_back` 后都写 eventfd 唤醒事件循环，触发 `apply_completed`。

所以 `completed_.size()` 天然 **≤ 4**。

### 不是全进程内存上限

`outstanding_` 管的是**任务个数**，不是**字节数**。至少漏了四块：

| 来源 | 上界 | 说明 |
|---|---|---|
| 4 个任务 × 16 MiB | 64 MiB | `outstanding_` 管得着的部分 |
| **8 个连接的 `body`** | 128 MiB | `resize` 一次性全额分配（[第 261 行](../../apps/vision_pool_server.cpp#L261)），且发生在**接收阶段**，此时 `outstanding_ = 0` |
| **worker 解码后的 `cv::Mat`** | 单个可达数百 MB | `imdecode` + `process_lane_lines` 六七张同尺寸 raster；2 worker 可达 GB 级 |
| 内核 socket 缓冲、json 中间对象、分配器开销 | — | 不在你的堆里但真实存在 |

`max_connections = 8`（[第 35 行](../../apps/vision_pool_server.cpp#L35)）堵住了「连接数无上限」的洞，但**内存的真正大头是解码后的 `cv::Mat`，它不受任何计数约束**。

---

## 5. 请求已收齐后收到 `EPOLLRDHUP`，为什么不能直接把它等同于「不能发送响应」？30 秒处理期限到期时，是否意味着计算已被取消？

### RDHUP ≠ 不能发送

`EPOLLRDHUP` 表示**对端关闭了写方向**（`shutdown(SHUT_WR)` 或 close），这是 **half-close**。TCP 是全双工的，一方关掉写方向**不代表**它关掉了读方向。

典型场景：客户端发完请求 → `shutdown(fd, SHUT_WR)` → 表示「我说完了」→ **但仍然在等着读响应**。这是合法且推荐的用法。若服务端把 RDHUP 当作「连接死了」直接关闭，客户端将永远拿不到响应。

代码的处理（[vision_pool_server.cpp:466-471](../../apps/vision_pool_server.cpp#L466-L471)）：

```cpp
if (flags & EPOLLRDHUP) {
    // 已收齐请求；对端可能只是关闭发送方向。
    // 关闭这一持续通知，仍会收到 EPOLLERR/EPOLLHUP。
    set_interest(poller.get(), EPOLL_CTL_MOD,
                 connection.socket.get(), token, 0);
}
```

**关键是清空兴趣而不是关闭连接**。因为 RDHUP 在水平触发模式下**只要对端保持半关闭状态就会持续上报**，不清掉会使事件循环每次 `epoll_wait` 都看到它，变成忙等自旋。清成 `0` 之后仍会收到 `EPOLLERR` / `EPOLLHUP`——**那两个才是真的连接断了**。

对比 `Phase::waiting_result` 下的 `EPOLLHUP`：[第 463-465 行](../../apps/vision_pool_server.cpp#L463-L465)直接抛异常关闭连接。

### 30 秒到期 ≠ 取消计算

[expire_connections](../../apps/vision_pool_server.cpp#L341-L359) 只做一件事——把连接从 `connections` 中删除：

```cpp
const bool processing = connection.phase == Phase::waiting_result;
const bool expired = processing
    ? now - connection.submitted_at >= processing_timeout
    : now - connection.last_progress >= idle_timeout;
```

**worker 完全不知情**，继续执行 `handler_`，跑完照样入队、照样写 eventfd。随后事件循环在 `apply_completed` 中查 token 查不到，[打印「丢弃过期结果」](../../apps/vision_pool_server.cpp#L321)并丢弃。

**所以 30 秒是「放弃等待的期限」，不是「取消计算的期限」**：超时后计算继续占用 worker 和内存，直到自然结束。一个跑 60 秒的任务会白白占着 worker 30 秒，成果被丢。

---

## 附：当前设计的已知局限

| 局限 | 说明 | 可能的改进方向 |
|---|---|---|
| **无取消机制** | 超时只放弃等待，计算继续跑到自然结束 | 协作式取消点（worker 定期检查 `atomic<bool>`），或把任务切成可中断的段 |
| **无字节级内存预算** | `outstanding_` 只计任务个数，`cv::Mat` 完全不受约束 | 队列增加累计字节计数；限制解码尺寸（`cv::IMREAD_REDUCED_*`） |
| **`body` 全额预分配** | `resize(payload_length)` 在收到消息头时即分配，慢速客户端可造成内存放大 | 改为按需增长，或对单连接设更小上限 |

---

## 一句话总结

五道题共同指向一条主线：**事件循环独占连接状态，worker 只做纯计算，两者通过有界队列 + eventfd + token 单向交接所有权**。这个模型用「计数」而非「字节」来约束资源，用「token」而非「fd」来保证身份，用「丢弃」而非「取消」来处理过期——三处简化各自划定了当前设计的边界。
