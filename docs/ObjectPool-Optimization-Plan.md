# ObjectPool 优化计划

> 创建日期: 2026-01-25
> 状态: 规划中

## 背景

当前 ObjectPool 实现采用分片 + mutex 的设计，每次 Get/Put 都需要加锁。
参考 Golang sync.Pool 的设计理念，计划通过渐进式迭代优化性能。

## 当前实现分析

```cpp
// 当前架构
template <class T>
class ObjectPool {
    struct Shard {
        mutable std::mutex mutex;
        std::vector<std::unique_ptr<T>> objects;
    };

    std::vector<Shard> shards_;  // 分片，减少锁竞争
    Factory factory_;
};

// Get/Put 流程
// 1. hash(thread_id) → 选择分片
// 2. 加锁 → 操作 → 解锁
// 3. 工作窃取（try_lock 其他分片）
```

**性能瓶颈**: 每次操作都需要 mutex 加锁，热路径开销 ~50-100ns

---

## 渐进式优化计划

### Phase 1: 单对象本地缓存

**目标**: 为每个线程添加单个私有对象槽位，热路径无锁

**设计**:
```cpp
struct LocalCache {
    pointer private_obj = nullptr;  // 单个私有对象
};

// Per-Pool 缓存映射（避免全局 TLS 问题）
std::shared_mutex cache_mutex_;
std::unordered_map<std::thread::id, LocalCache> caches_;
```

**Get 流程**:
```
1. [热路径] 检查 private_obj → 非空则直接返回（无锁）
2. [温路径] 从共享分片获取
3. [冷路径] 工作窃取 / 新建
```

**Put 流程**:
```
1. [热路径] private_obj 为空 → 直接存入（无锁）
2. [温路径] private_obj 已满 → 放入共享分片
```

**预期收益**:
- 热路径: ~5-10ns（vs 当前 ~50-100ns）
- 命中率: ~50%（Get 后 Put，或 Put 后 Get 的场景）

**风险**:
- Per-Pool Map 查找有开销（shared_mutex 读锁 + hash 查找）
- 需要验证 LSCQ 使用场景的命中率

---

### Phase 2: 多对象本地缓存

**目标**: 扩展本地缓存容量，提高命中率

**设计**:
```cpp
struct alignas(64) LocalCache {  // cache line 对齐
    static constexpr std::size_t kBatchSize = 4;

    pointer private_obj = nullptr;           // 最快路径
    pointer local_batch[kBatchSize] = {};    // 本地批量
    std::size_t local_count = 0;
};
```

**Get 流程**:
```
1. [最热] private_obj 非空 → 返回
2. [次热] local_batch 有对象 → pop 并返回
3. [温] 从共享分片批量获取到 local_batch
4. [冷] 工作窃取 / 新建
```

**Put 流程**:
```
1. [最热] private_obj 为空 → 存入
2. [次热] local_batch 未满 → push
3. [温] local_batch 满 → 批量转移到共享分片
```

**预期收益**:
- 热路径命中率: ~80-90%
- 减少共享分片访问频率

**注意事项**:
- kBatchSize 需要根据 LSCQ 使用模式调优
- 过大的批量可能导致内存浪费

---

### Phase 3: TLS 生命周期改进

**目标**: 解决 TLS 生命周期问题，确保安全性

**当前问题**:
```
┌─────────────────────────────────────────────────────────────┐
│ 核心矛盾                                                     │
├─────────────────────────────────────────────────────────────┤
│ TLS 生命周期 = 线程生命周期                                  │
│ Pool 生命周期 = Pool 对象生命周期                            │
│                                                             │
│ 可能出现：                                                   │
│ 1. Pool 先销毁 → 线程的 TLS 指向已释放内存 (UAF)            │
│ 2. 线程先退出 → Pool 持有悬空指针（可接受）                 │
└─────────────────────────────────────────────────────────────┘
```

**EBR 踩过的坑**:
- 全局 TLS 被多个 Pool 实例共享
- 析构时只能清理当前线程的 TLS
- 其他线程的 TLS 仍指向已释放内存

**解决方案: 闭锁 + Per-Pool Map**:
```cpp
template <class T>
class ObjectPool {
    // 闭锁机制（保护析构）
    std::atomic<bool> closing_{false};
    std::atomic<int> active_ops_{0};

    // Per-Pool 缓存（避免全局 TLS）
    std::shared_mutex cache_mutex_;
    std::unordered_map<std::thread::id, LocalCache> caches_;

    pointer Get() {
        // 检查关闭状态
        if (closing_.load(std::memory_order_acquire)) {
            return factory_();
        }

        // 操作计数保护
        active_ops_.fetch_add(1, std::memory_order_acquire);
        auto guard = finally([this] {
            active_ops_.fetch_sub(1, std::memory_order_release);
        });

        // ... 正常 Get 逻辑 ...
    }

    ~ObjectPool() {
        // 1. 标记关闭
        closing_.store(true, std::memory_order_release);

        // 2. 等待所有操作完成
        while (active_ops_.load(std::memory_order_acquire) > 0) {
            std::this_thread::yield();
        }

        // 3. 安全清理本地缓存
        std::unique_lock lock(cache_mutex_);
        for (auto& [tid, cache] : caches_) {
            if (cache.private_obj) {
                delete cache.private_obj;
            }
            for (std::size_t i = 0; i < cache.local_count; ++i) {
                delete cache.local_batch[i];
            }
        }
        caches_.clear();

        // 4. 共享分片自动清理（RAII）
    }
};
```

**优点**:
- Pool 完全控制缓存生命周期
- 无全局 TLS，无跨 Pool 干扰
- 析构时可安全遍历所有缓存

**缺点**:
- 每次 Get/Put 需要 shared_mutex 读锁
- 有原子操作开销（active_ops_）

**优化方向**:
- 考虑用 thread_local + Pool 指针检查替代 map 查找
- 或使用 lock-free 的并发 map

---

## 实施时间线

| Phase | 预计工作量 | 优先级 |
|-------|-----------|--------|
| Phase 1 | 1-2 天 | 高 |
| Phase 2 | 1 天 | 中 |
| Phase 3 | 1-2 天 | 高（安全性） |

## 测试验证

每个 Phase 完成后需要验证：

1. **功能正确性**
   - 单线程 Get/Put 测试
   - 多线程并发测试
   - LSCQ 集成测试

2. **内存安全**
   - AddressSanitizer 验证无泄漏
   - ThreadSanitizer 验证无数据竞争

3. **性能对比**
   - benchmark_pair 测试吞吐量
   - 对比优化前后的 ns/op

4. **析构安全**
   - Pool 先于线程销毁
   - 线程先于 Pool 退出
   - 并发析构场景

---

## 参考资料

- Golang sync.Pool 实现: https://github.com/golang/go/blob/master/src/sync/pool.go
- C++ TLS 最佳实践
- 项目 EBR 实现的教训: `include/lscq/ebr.hpp`
