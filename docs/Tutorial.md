# 教程：LSCQ / SCQ 系列队列使用指南（中文）

本文档面向第一次使用本仓库队列的读者，包含队列选型、核心 API 语义、示例代码和最佳实践。

如果你想先快速上手，请从 `docs/usage.md` 开始；如果想了解实现原理，请看 `docs/architecture.md`。

---

## 1. 队列选型（先决定用哪个）

本项目提供的队列可以粗略分为两类：**有界**（容量固定）与 **无界**（自动扩容）。

| 需求 | 推荐类型 | 备注 |
|---|---|---|
| 有界、追求更高吞吐 | `lscq::SCQ<T>` | 存“索引值”，有效容量约为 ring 的一半 |
| 有界、存指针 | `lscq::SCQP<T>` | `T*` API，CAS2 可用时更快 |
| 无界、存指针 | `lscq::LSCQ<T>` | 链式扩容 + EBR 回收空节点 |
| 需要对照基线（吞吐很重要的场景不建议） | `lscq::MSQueue<T>` / `lscq::MutexQueue<T>` | 用于 benchmark 对比或快速验证 |

---

## 2. API 语义概览（一定要看）

### 2.1 `SCQ<T>`（索引队列）

`SCQ<T>` 存储的是“索引值”（通常映射到外部数组/对象池），典型约束：

- `T` 建议使用无符号整型（如 `std::uint64_t`）
- 入队值必须 `< (SCQSIZE - 1)` 且不等于 `SCQ<T>::kEmpty`
- `dequeue()` 返回 `SCQ<T>::kEmpty` 表示空队列

容量说明：

- 构造参数是 ring size（内部会 round up 到 2 的幂）
- **有效容量约为 ring size 的一半**（算法特性）

### 2.2 `SCQP<T>`（指针队列，有界）

`SCQP<T>` 存 `T*`：

- `enqueue(nullptr)` 会失败（返回 `false`）
- `dequeue()` 返回 `nullptr` 表示空队列
- 可通过 `is_using_fallback()` 观察是否进入回退模式（CAS2 不可用时）

### 2.3 `LSCQ<T>`（指针队列，无界）

`LSCQ<T>` 是无界队列，内部链接多个 `SCQP` 节点。注意点：

- `enqueue(nullptr)` 失败（返回 `false`）
- `dequeue()` 返回 `nullptr` 表示空队列
- 需要一个 `lscq::EBRManager` 实例，并且它必须 **比队列活得更久**（`LSCQ` 持有引用）
- `LSCQ` 的 `enqueue/dequeue` 内部会进入 EBR 临界区（你不需要手动 `EpochGuard`）

### 2.4 `MSQueue<T>`（Michael-Scott 基线）

这是一个简化版 M&S 队列，API 是“值 + 输出参数”风格：

- `bool enqueue(const T&)`
- `bool dequeue(T& out)`：成功返回 `true`，空队列返回 `false`

注意：为了避免引入 hazard pointer/epoch，出队后节点不会立即释放，而是延迟到析构时统一回收。**销毁队列时必须确保没有其他线程再访问该队列。**

### 2.5 `MutexQueue<T>`（互斥基线）

这是一个最小的 mutex + `std::queue` 基线实现，API 与 `MSQueue` 类似：

- `bool enqueue(const T&)` / `bool enqueue(T&&)`
- `bool dequeue(T& out)`

---

## 3. 示例代码（可直接复制）

### 3.1 基本示例：`SCQ` 入队/出队（索引）

```cpp
#include <lscq/scq.hpp>
#include <cstdint>
#include <iostream>

int main() {
    lscq::SCQ<std::uint64_t> q(1024);

    for (std::uint64_t v = 1; v <= 5; ++v) {
        q.enqueue(v);
    }

    while (true) {
        auto v = q.dequeue();
        if (v == lscq::SCQ<std::uint64_t>::kEmpty) break;
        std::cout << v << "\n";
    }
}
```

### 3.2 基本示例：`SCQP` 入队/出队（指针）

```cpp
#include <lscq/scqp.hpp>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    std::vector<std::uint64_t> values = {10, 20, 30, 40, 50};
    lscq::SCQP<std::uint64_t> q(1024);

    std::cout << "fallback: " << (q.is_using_fallback() ? "yes" : "no") << "\n";

    for (auto& v : values) {
        q.enqueue(&v);
    }

    while (auto* p = q.dequeue()) {
        std::cout << *p << "\n";
    }
}
```

### 3.3 基本示例：`LSCQ`（无界指针队列 + EBR）

```cpp
#include <lscq/ebr.hpp>
#include <lscq/lscq.hpp>
#include <cstdint>
#include <iostream>

int main() {
    lscq::EBRManager ebr;
    lscq::LSCQ<std::uint64_t> q(ebr);

    std::uint64_t a = 123;
    q.enqueue(&a);

    if (auto* p = q.dequeue()) {
        std::cout << *p << "\n";
    }
}
```

生命周期提示：

- `LSCQ` 存的是 `T*`，对象必须在出队之前保持有效。
- 如果对象需要释放，建议由消费者统一释放（或使用对象池/内存池）。

### 3.4 基线示例：`MSQueue` / `MutexQueue`（值队列）

```cpp
#include <lscq/msqueue.hpp>
#include <lscq/mutex_queue.hpp>
#include <cstdint>

int main() {
    lscq::MSQueue<std::uint64_t> msq;
    lscq::MutexQueue<std::uint64_t> muq;

    msq.enqueue(1);
    muq.enqueue(2);

    std::uint64_t out = 0;
    msq.dequeue(out);
    muq.dequeue(out);
}
```

---

## 4. 最佳实践（性能与正确性）

1. **确认 CAS2 能力与回退路径**
   - `lscq::has_cas2_support()`：运行时检测 CPU 是否支持 128-bit CAS
   - `SCQP<T>::is_using_fallback()`：指针队列是否处于回退模式
2. **有界队列容量要留余量**
   - `SCQ/SCQP` 的“有效容量约为一半”，建议按峰值并发量留足空间
3. **尽量避免频繁 new/delete**
   - `SCQ` 适合“索引 + 外部数组/对象池”
   - 指针队列适合结合业务对象池，减少内存分配与缓存抖动
4. **不要把特殊值当作正常数据**
   - `SCQ<T>` 不能 enqueue `kEmpty`，也不能入队达到 `SCQSIZE - 1` 的值（bottom 标记相关）
   - 指针队列不能 enqueue `nullptr`
5. **性能测试用 Release-Perf**
   - Windows 推荐使用 `CMakePresets.json` 里的 `windows-clang-release-perf`
   - 参考：`docs/benchmark-testing-guide.md`

---

## 5. 构建与运行（快速提示）

```bash
# 示例
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target examples
./build/release/examples/simple_queue

# 单元测试
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

如果你的重点是 benchmark，请直接按 `docs/benchmark-testing-guide.md` 的步骤操作（含 Windows/Linux/macOS 说明与 Python 结果分析）。

