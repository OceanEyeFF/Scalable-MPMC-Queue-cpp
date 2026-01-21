# 使用指南

本页从“如何选择队列”与“如何正确使用 API”两方面给出示例与最佳实践。可直接参考：

- `examples/simple_queue.cpp`
- `examples/benchmark_custom.cpp`

## 选择合适的队列

- 需要 **有界** 且希望吞吐更好：优先考虑 `SCQ<T>`
- 需要 **有界** 且存储的是 `T*`：使用 `SCQP<T>`
- 需要 **无界** 且存储的是 `T*`：使用 `LSCQ<T>`（内部链式扩容）

## 基本用法

### SCQ（索引队列）

`SCQ<T>` 中的 `T` 是“索引值”（通常映射到外部数组/对象池），需要满足：

- `T` 是无符号整型（如 `std::uint64_t`）
- 入队值必须 `< (SCQSIZE - 1)` 且不等于 `SCQ<T>::kEmpty`
- `dequeue()` 返回 `kEmpty` 表示空队列

建议做法：

- 让 `SCQSIZE` 为 2 的幂（构造函数会自动 round up）
- 把对象存到外部 `std::vector`/池中，入队的是索引

### SCQP / LSCQ（指针队列）

`SCQP<T>` 与 `LSCQ<T>` 存储的是 `T*`：

- `enqueue(nullptr)` 会失败（返回 `false`）
- `dequeue()` 返回 `nullptr` 表示空队列

指针生命周期是调用方责任：

- 被入队的对象必须在出队前保持有效
- 若对象需要释放，建议出队方统一释放，或结合业务内存池策略

## 最佳实践

1. **优先复用内存**：生产者从对象池获取对象/槽位，消费者归还，减少 new/delete 压力。
2. **关注 CAS2 能力与回退路径**：
   - `lscq::has_cas2_support()` 可在运行时判断 CPU 是否支持 128-bit CAS
   - `SCQP<T>::is_using_fallback()` 可判断是否进入 index+side-array 回退模式
3. **容量选择（有界队列）**：
   - `SCQ/SCQP` 的 `scqsize` 是 ring 大小（2n），有效容量约为一半（n）
4. **避免把“特殊值”当作正常值**：
   - `SCQ<T>` 不能 enqueue `kEmpty`，且值不能达到 `SCQSIZE-1`（bottom 标记）

## 构建与运行（开发者）

```bash
# 构建示例
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target examples

# 生成 Doxygen 文档
doxygen Doxyfile
```

