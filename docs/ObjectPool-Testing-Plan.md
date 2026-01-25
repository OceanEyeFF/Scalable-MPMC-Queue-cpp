# ObjectPool 优化测试验证计划 v2.0

> 创建日期: 2026-01-25
> 更新日期: 2026-01-25
> 状态: **已重构**
> 环境: Windows + MSVC
>
> ⚠️ **重要变更**: 本版本基于优化计划Review的发现进行了重大重构

---

## 变更摘要

相比v1.0的主要变更：

| 变更项 | v1.0 | v2.0 |
|--------|------|------|
| Phase划分 | Phase 1-3 | **Phase 0** (新增) + Phase 1-3 |
| 性能预期 | 5-10ns | **30-50ns** (修正) 或 5-10ns (thread_local方案) |
| 测试场景 | 对称Get/Put | **LSCQ真实场景模拟** |
| 命中率测量 | 仅提及 | **具体实现方案** |
| 方案验证 | 无 | **thread_local vs shared_mutex对比** |

---

## 测试目标 (更新)

确保 ObjectPool 优化在以下方面完全可靠：

1. 📊 **Phase 0: 基准数据收集** ← 🆕 最重要！
2. ✅ **功能正确性** - 单线程和多线程场景
3. 🎯 **LSCQ真实场景验证** ← 🆕 贴近实际使用
4. 🔥 **压力测试** - 高并发、长时间运行
5. 💾 **内存安全** - 无泄漏、无UAF、无数据竞争
6. 🛡️ **析构安全** - Pool和线程生命周期交叉场景
7. ⚡ **性能提升验证** - 量化优化效果

---

## 测试策略架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                    测试金字塔 v2.0                                   │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  🏆 验收测试                                                         │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ LSCQ真实负载性能验证                                          │   │
│  │ 端到端性能对比                                                 │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  🔥 压力测试                                                         │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ 高并发压力 | 长时间运行 | 析构安全 | 生命周期交叉            │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  🎯 场景测试 (🆕)                                                    │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ LSCQ enqueue扩容模拟 | LSCQ dequeue推进模拟 | 竞争失败模拟  │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  📊 基准测试 (Phase 0) (🆕)                                          │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ Map查找开销 | shared_mutex开销 | thread_local开销           │   │
│  │ LSCQ Get/Put比例分析 | 命中率测量                            │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  ✅ 单元测试                                                         │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ Get/Put基础 | 对象复用 | 分片选择 | 工作窃取                 │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Phase 0: 基准数据收集 🆕

> ⚠️ **这是最关键的Phase！** 必须在开始任何优化前完成。

### 0.1 组件开销微基准测试

**目标**: 精确测量各个组件的开销，验证优化假设

**测试文件**: `benchmarks/benchmark_components.cpp`

```cpp
#include <benchmark/benchmark.h>
#include <unordered_map>
#include <shared_mutex>
#include <thread>
#include <atomic>

//=============================================================================
// 0.1.1 Map查找开销测量
//=============================================================================

// 测量 unordered_map + shared_mutex 的读取开销
static void BM_MapLookup_SharedMutex(benchmark::State& state) {
    std::shared_mutex mutex;
    std::unordered_map<std::thread::id, int> map;
    auto tid = std::this_thread::get_id();
    map[tid] = 42;

    for (auto _ : state) {
        std::shared_lock lock(mutex);
        auto it = map.find(tid);
        benchmark::DoNotOptimize(it->second);
    }
}
BENCHMARK(BM_MapLookup_SharedMutex)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// 测量纯 unordered_map 查找开销（无锁）
static void BM_MapLookup_NoLock(benchmark::State& state) {
    std::unordered_map<std::thread::id, int> map;
    auto tid = std::this_thread::get_id();
    map[tid] = 42;

    for (auto _ : state) {
        auto it = map.find(tid);
        benchmark::DoNotOptimize(it->second);
    }
}
BENCHMARK(BM_MapLookup_NoLock);

// 测量 std::thread::id 获取开销
static void BM_GetThreadId(benchmark::State& state) {
    for (auto _ : state) {
        auto tid = std::this_thread::get_id();
        benchmark::DoNotOptimize(tid);
    }
}
BENCHMARK(BM_GetThreadId);

// 测量 std::hash<std::thread::id> 开销
static void BM_HashThreadId(benchmark::State& state) {
    auto tid = std::this_thread::get_id();
    std::hash<std::thread::id> hasher;

    for (auto _ : state) {
        auto h = hasher(tid);
        benchmark::DoNotOptimize(h);
    }
}
BENCHMARK(BM_HashThreadId);

//=============================================================================
// 0.1.2 锁开销测量
//=============================================================================

// shared_mutex 读锁开销
static void BM_SharedMutex_ReadLock(benchmark::State& state) {
    std::shared_mutex mutex;

    for (auto _ : state) {
        std::shared_lock lock(mutex);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_SharedMutex_ReadLock)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// shared_mutex 写锁开销
static void BM_SharedMutex_WriteLock(benchmark::State& state) {
    std::shared_mutex mutex;

    for (auto _ : state) {
        std::unique_lock lock(mutex);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_SharedMutex_WriteLock)->Threads(1);

// mutex 开销（对比基准）
static void BM_Mutex_Lock(benchmark::State& state) {
    std::mutex mutex;

    for (auto _ : state) {
        std::lock_guard lock(mutex);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_Mutex_Lock)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

//=============================================================================
// 0.1.3 thread_local 开销测量
//=============================================================================

struct LocalCache {
    int* private_obj = nullptr;
    void* owner = nullptr;
};

static thread_local LocalCache tls_cache;

// thread_local 访问开销
static void BM_ThreadLocal_Access(benchmark::State& state) {
    tls_cache.private_obj = new int(42);
    tls_cache.owner = reinterpret_cast<void*>(0x12345678);

    for (auto _ : state) {
        if (tls_cache.owner == reinterpret_cast<void*>(0x12345678)
            && tls_cache.private_obj) {
            benchmark::DoNotOptimize(*tls_cache.private_obj);
        }
    }

    delete tls_cache.private_obj;
}
BENCHMARK(BM_ThreadLocal_Access);

// thread_local + 指针检查开销
static void BM_ThreadLocal_OwnerCheck(benchmark::State& state) {
    void* pool_addr = reinterpret_cast<void*>(0x12345678);
    tls_cache.owner = pool_addr;

    for (auto _ : state) {
        bool match = (tls_cache.owner == pool_addr);
        benchmark::DoNotOptimize(match);
    }
}
BENCHMARK(BM_ThreadLocal_OwnerCheck);

//=============================================================================
// 0.1.4 原子操作开销测量
//=============================================================================

// atomic<bool> load (acquire)
static void BM_AtomicBool_Load(benchmark::State& state) {
    std::atomic<bool> flag{false};

    for (auto _ : state) {
        bool v = flag.load(std::memory_order_acquire);
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_AtomicBool_Load);

// atomic<int> fetch_add (acquire)
static void BM_AtomicInt_FetchAdd(benchmark::State& state) {
    std::atomic<int> counter{0};

    for (auto _ : state) {
        counter.fetch_add(1, std::memory_order_acquire);
        counter.fetch_sub(1, std::memory_order_release);
    }
}
BENCHMARK(BM_AtomicInt_FetchAdd)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

//=============================================================================
// 0.1.5 完整热路径模拟
//=============================================================================

// 模拟 Per-Pool Map 方案的热路径
static void BM_FullPath_MapBased(benchmark::State& state) {
    std::shared_mutex mutex;
    std::unordered_map<std::thread::id, LocalCache> map;
    std::atomic<bool> closing{false};
    std::atomic<int> active_ops{0};

    auto tid = std::this_thread::get_id();
    map[tid] = LocalCache{new int(42), reinterpret_cast<void*>(0x12345678)};

    for (auto _ : state) {
        // 闭锁检查
        if (closing.load(std::memory_order_acquire)) continue;
        active_ops.fetch_add(1, std::memory_order_acquire);

        // Map查找
        {
            std::shared_lock lock(mutex);
            auto it = map.find(tid);
            if (it != map.end() && it->second.private_obj) {
                benchmark::DoNotOptimize(*it->second.private_obj);
            }
        }

        active_ops.fetch_sub(1, std::memory_order_release);
    }

    delete map[tid].private_obj;
}
BENCHMARK(BM_FullPath_MapBased)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// 模拟 thread_local 方案的热路径
static void BM_FullPath_ThreadLocal(benchmark::State& state) {
    static thread_local LocalCache fast_cache;
    std::atomic<bool> closing{false};
    std::atomic<int> active_ops{0};

    void* pool_addr = reinterpret_cast<void*>(0x12345678);
    fast_cache.owner = pool_addr;
    fast_cache.private_obj = new int(42);

    for (auto _ : state) {
        // 闭锁检查
        if (closing.load(std::memory_order_acquire)) continue;
        active_ops.fetch_add(1, std::memory_order_acquire);

        // thread_local 快路径
        if (fast_cache.owner == pool_addr && fast_cache.private_obj) {
            benchmark::DoNotOptimize(*fast_cache.private_obj);
        }

        active_ops.fetch_sub(1, std::memory_order_release);
    }

    delete fast_cache.private_obj;
    fast_cache.private_obj = nullptr;
}
BENCHMARK(BM_FullPath_ThreadLocal)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

BENCHMARK_MAIN();
```

**预期输出格式**:
```
---------------------------------------------------------------------------
Benchmark                                 Time             CPU   Iterations
---------------------------------------------------------------------------
BM_MapLookup_SharedMutex/threads:1       45 ns         45 ns     15000000
BM_MapLookup_SharedMutex/threads:4       89 ns         85 ns      8000000
BM_MapLookup_SharedMutex/threads:8      156 ns        148 ns      4500000
BM_ThreadLocal_Access                     3 ns          3 ns    220000000
BM_FullPath_MapBased/threads:1           62 ns         62 ns     11000000
BM_FullPath_ThreadLocal/threads:1        18 ns         18 ns     39000000
```

**决策阈值**:
```
如果 BM_FullPath_MapBased < 25ns:
    → 继续使用 Per-Pool Map 方案
如果 BM_FullPath_MapBased > 40ns:
    → 必须切换到 thread_local 方案
如果 25ns <= BM_FullPath_MapBased <= 40ns:
    → 需要权衡：安全性 vs 性能
```

---

### 0.2 LSCQ 使用模式分析

**目标**: 分析LSCQ真实使用场景中的Get/Put比例和调用模式

**测试文件**: `tests/analysis/test_lscq_usage_pattern.cpp`

```cpp
#include <gtest/gtest.h>
#include <lscq/lscq.hpp>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
#include <iomanip>

// 统计版ObjectPool包装器
template<typename T>
class InstrumentedObjectPool {
public:
    // 统计计数器
    std::atomic<long> total_get_calls{0};
    std::atomic<long> total_put_calls{0};
    std::atomic<long> get_from_cache_hits{0};      // 命中本地缓存
    std::atomic<long> get_from_shard_hits{0};      // 命中共享分片
    std::atomic<long> get_from_factory{0};         // 调用工厂创建
    std::atomic<long> put_to_cache{0};             // 放入本地缓存
    std::atomic<long> put_to_shard{0};             // 放入共享分片
    std::atomic<long> get_after_put_same_thread{0}; // 同线程Put后Get（命中候选）

    // 每线程上下文追踪
    struct ThreadContext {
        bool last_op_was_put = false;
        std::chrono::steady_clock::time_point last_put_time;
    };
    std::unordered_map<std::thread::id, ThreadContext> thread_contexts_;
    std::mutex ctx_mutex_;

    lscq::ObjectPool<T>& underlying_pool_;

    explicit InstrumentedObjectPool(lscq::ObjectPool<T>& pool)
        : underlying_pool_(pool) {}

    T* Get() {
        total_get_calls.fetch_add(1);

        // 检查是否是Put后的Get（潜在命中）
        {
            std::lock_guard lock(ctx_mutex_);
            auto tid = std::this_thread::get_id();
            auto& ctx = thread_contexts_[tid];
            if (ctx.last_op_was_put) {
                auto elapsed = std::chrono::steady_clock::now() - ctx.last_put_time;
                if (elapsed < std::chrono::microseconds(100)) {
                    get_after_put_same_thread.fetch_add(1);
                }
            }
            ctx.last_op_was_put = false;
        }

        return underlying_pool_.Get();
    }

    void Put(T* obj) {
        total_put_calls.fetch_add(1);

        {
            std::lock_guard lock(ctx_mutex_);
            auto tid = std::this_thread::get_id();
            auto& ctx = thread_contexts_[tid];
            ctx.last_op_was_put = true;
            ctx.last_put_time = std::chrono::steady_clock::now();
        }

        underlying_pool_.Put(obj);
    }

    void PrintStats() {
        long gets = total_get_calls.load();
        long puts = total_put_calls.load();
        long pot_hits = get_after_put_same_thread.load();

        std::cout << "\n========== LSCQ ObjectPool Usage Analysis ==========\n";
        std::cout << "Total Get() calls:     " << gets << "\n";
        std::cout << "Total Put() calls:     " << puts << "\n";
        std::cout << "Get/Put ratio:         " << std::fixed << std::setprecision(2)
                  << (puts > 0 ? (double)gets / puts : 0) << "\n";
        std::cout << "Potential cache hits:  " << pot_hits
                  << " (" << (gets > 0 ? pot_hits * 100.0 / gets : 0) << "%)\n";
        std::cout << "====================================================\n";
    }
};

class LSCQUsagePatternTest : public ::testing::Test {
protected:
    static constexpr std::size_t kSCQSize = 256;
    static constexpr int kProducers = 4;
    static constexpr int kConsumers = 4;
    static constexpr int kDurationSeconds = 10;
    static constexpr int kItemsPerProducer = 100000;
};

// 测试场景1: 对称生产消费
TEST_F(LSCQUsagePatternTest, SymmetricProducerConsumer) {
    lscq::LSCQ<uint64_t> queue(kSCQSize);

    std::atomic<bool> stop{false};
    std::atomic<long> enqueue_count{0};
    std::atomic<long> dequeue_count{0};

    // 生产者
    std::vector<std::thread> producers;
    for (int i = 0; i < kProducers; ++i) {
        producers.emplace_back([&, i]() {
            for (int j = 0; j < kItemsPerProducer && !stop.load(); ++j) {
                auto* item = new uint64_t(i * kItemsPerProducer + j);
                if (queue.enqueue(item)) {
                    enqueue_count.fetch_add(1);
                } else {
                    delete item;
                }
            }
        });
    }

    // 消费者
    std::vector<std::thread> consumers;
    for (int i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&]() {
            while (!stop.load() || dequeue_count.load() < enqueue_count.load()) {
                if (auto* item = queue.dequeue()) {
                    dequeue_count.fetch_add(1);
                    delete item;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    // 等待生产者完成
    for (auto& t : producers) t.join();

    // 等待消费完成
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (dequeue_count.load() < enqueue_count.load()
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    stop.store(true);

    for (auto& t : consumers) t.join();

    std::cout << "\n========== Symmetric Producer/Consumer ==========\n";
    std::cout << "Enqueue count: " << enqueue_count.load() << "\n";
    std::cout << "Dequeue count: " << dequeue_count.load() << "\n";
    std::cout << "=================================================\n";

    EXPECT_EQ(enqueue_count.load(), dequeue_count.load());
}

// 测试场景2: 高竞争enqueue（触发扩容）
TEST_F(LSCQUsagePatternTest, HighContentionEnqueue) {
    lscq::LSCQ<uint64_t> queue(64);  // 小容量，强制扩容

    std::atomic<long> total_enqueues{0};
    std::atomic<long> enqueue_retries{0};  // 需要重试的次数（间接反映扩容）

    std::vector<std::thread> producers;
    for (int i = 0; i < 16; ++i) {  // 16个生产者，高竞争
        producers.emplace_back([&]() {
            for (int j = 0; j < 10000; ++j) {
                auto* item = new uint64_t(j);
                int attempts = 0;
                while (!queue.enqueue(item)) {
                    attempts++;
                    if (attempts > 100) {
                        delete item;
                        break;
                    }
                    std::this_thread::yield();
                }
                if (attempts <= 100) {
                    total_enqueues.fetch_add(1);
                    if (attempts > 0) {
                        enqueue_retries.fetch_add(attempts);
                    }
                }
            }
        });
    }

    for (auto& t : producers) t.join();

    std::cout << "\n========== High Contention Enqueue ==========\n";
    std::cout << "Total enqueues:  " << total_enqueues.load() << "\n";
    std::cout << "Total retries:   " << enqueue_retries.load() << "\n";
    std::cout << "Retry rate:      " << std::fixed << std::setprecision(2)
              << (total_enqueues.load() > 0
                  ? enqueue_retries.load() * 100.0 / total_enqueues.load()
                  : 0) << "%\n";
    std::cout << "=============================================\n";

    // 清理队列
    while (auto* item = queue.dequeue()) {
        delete item;
    }
}

// 测试场景3: 分析命中率潜力
TEST_F(LSCQUsagePatternTest, CacheHitPotentialAnalysis) {
    // 模拟ObjectPool的Get/Put调用序列
    // 分析：如果有本地缓存，命中率会是多少？

    struct CallRecord {
        enum Type { GET, PUT };
        Type type;
        std::thread::id tid;
        std::chrono::steady_clock::time_point time;
    };

    std::vector<CallRecord> records;
    std::mutex records_mutex;

    lscq::LSCQ<uint64_t> queue(kSCQSize);
    std::atomic<bool> stop{false};

    // 模拟LSCQ负载，记录Get/Put序列
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([&]() {
            auto tid = std::this_thread::get_id();
            for (int j = 0; j < 1000 && !stop.load(); ++j) {
                // Enqueue (可能触发Get)
                auto* item = new uint64_t(j);
                {
                    std::lock_guard lock(records_mutex);
                    records.push_back({CallRecord::GET, tid,
                                       std::chrono::steady_clock::now()});
                }
                queue.enqueue(item);

                // Dequeue (触发Put)
                if (auto* dequeued = queue.dequeue()) {
                    {
                        std::lock_guard lock(records_mutex);
                        records.push_back({CallRecord::PUT, tid,
                                           std::chrono::steady_clock::now()});
                    }
                    delete dequeued;
                }
            }
        });
    }

    for (auto& t : workers) t.join();

    // 分析命中率潜力
    long total_gets = 0;
    long potential_hits = 0;
    std::unordered_map<std::thread::id, std::chrono::steady_clock::time_point> last_put;

    for (const auto& rec : records) {
        if (rec.type == CallRecord::PUT) {
            last_put[rec.tid] = rec.time;
        } else {  // GET
            total_gets++;
            auto it = last_put.find(rec.tid);
            if (it != last_put.end()) {
                auto elapsed = rec.time - it->second;
                // 如果在100us内有Put，则可能命中
                if (elapsed < std::chrono::microseconds(100)) {
                    potential_hits++;
                }
            }
        }
    }

    double hit_rate = total_gets > 0 ? potential_hits * 100.0 / total_gets : 0;

    std::cout << "\n========== Cache Hit Potential Analysis ==========\n";
    std::cout << "Total Get calls:      " << total_gets << "\n";
    std::cout << "Potential cache hits: " << potential_hits << "\n";
    std::cout << "Estimated hit rate:   " << std::fixed << std::setprecision(1)
              << hit_rate << "%\n";
    std::cout << "==================================================\n";

    // 验证假设
    if (hit_rate < 30) {
        std::cout << "⚠️ WARNING: Low hit rate! May need larger batch size.\n";
    } else if (hit_rate > 60) {
        std::cout << "✅ Good hit rate potential!\n";
    }

    // 清理
    while (auto* item = queue.dequeue()) delete item;
}
```

---

### 0.3 Phase 0 决策流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                       Phase 0 决策流程                               │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  1. 运行 benchmark_components                                       │
│     ↓                                                               │
│  2. 收集关键数据:                                                    │
│     - BM_FullPath_MapBased (单线程)                                 │
│     - BM_FullPath_ThreadLocal (单线程)                              │
│     - 多线程竞争下的性能变化                                         │
│     ↓                                                               │
│  3. 运行 test_lscq_usage_pattern                                    │
│     ↓                                                               │
│  4. 收集关键数据:                                                    │
│     - Get/Put 比例                                                  │
│     - 潜在命中率                                                     │
│     ↓                                                               │
│  5. 决策:                                                            │
│     ┌─────────────────────────────────────────────────────────┐    │
│     │ MapBased热路径 < 25ns AND 命中率 > 40%                   │    │
│     │   → 使用 Per-Pool Map 方案 (简单安全)                    │    │
│     ├─────────────────────────────────────────────────────────┤    │
│     │ MapBased热路径 > 40ns OR 命中率 < 30%                    │    │
│     │   → 使用 thread_local 混合方案 (高性能)                  │    │
│     ├─────────────────────────────────────────────────────────┤    │
│     │ 其他情况                                                 │    │
│     │   → 评估风险后选择，倾向于 thread_local 方案             │    │
│     └─────────────────────────────────────────────────────────┘    │
│     ↓                                                               │
│  6. 更新优化计划文档，记录决策依据                                   │
│     ↓                                                               │
│  7. 进入 Phase 1 开发                                               │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Phase 1-3: 功能和性能测试

### 1. 单元测试

**测试文件**: `tests/unit/test_object_pool.cpp`

```cpp
#include <gtest/gtest.h>
#include <lscq/object_pool.hpp>
#include <thread>
#include <vector>

using namespace lscq;

class ObjectPoolTest : public ::testing::Test {
protected:
    struct TestNode {
        int value = 0;
        explicit TestNode(int v = 0) : value(v) {}
    };

    ObjectPool<TestNode> pool_{[] { return new TestNode(42); }};
};

// 基础功能测试
TEST_F(ObjectPoolTest, BasicGetPut) {
    auto* obj = pool_.Get();
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->value, 42);
    pool_.Put(obj);
}

TEST_F(ObjectPoolTest, ReuseReturnedObject) {
    auto* obj1 = pool_.Get();
    obj1->value = 100;
    pool_.Put(obj1);

    auto* obj2 = pool_.Get();
    // 应该复用同一个对象
    EXPECT_EQ(obj2->value, 100);
    pool_.Put(obj2);
}

TEST_F(ObjectPoolTest, PutNullptr) {
    // Put nullptr 不应该崩溃
    pool_.Put(nullptr);
    SUCCEED();
}

TEST_F(ObjectPoolTest, MultipleGetPut) {
    std::vector<TestNode*> objects;
    for (int i = 0; i < 100; ++i) {
        objects.push_back(pool_.Get());
    }
    for (auto* obj : objects) {
        pool_.Put(obj);
    }
    EXPECT_GE(pool_.Size(), 0);
}

TEST_F(ObjectPoolTest, ClearRemovesAllObjects) {
    for (int i = 0; i < 10; ++i) {
        pool_.Put(pool_.Get());
    }
    pool_.Clear();
    EXPECT_EQ(pool_.Size(), 0);
}

// 多线程测试
TEST_F(ObjectPoolTest, MultiThreadedGetPut) {
    constexpr int kThreads = 8;
    constexpr int kIterations = 10000;
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < kIterations; ++j) {
                auto* obj = pool_.Get();
                if (!obj) {
                    errors.fetch_add(1);
                    continue;
                }
                pool_.Put(obj);
            }
        });
    }

    for (auto& t : threads) t.join();
    EXPECT_EQ(errors.load(), 0);
}

// Phase 1 特有测试：本地缓存命中率
TEST_F(ObjectPoolTest, LocalCacheHitRate) {
    // 这个测试需要 ObjectPool 提供命中率统计接口
    // 如果没有，可以通过测量延迟来间接验证

    constexpr int kIterations = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < kIterations; ++i) {
        auto* obj = pool_.Get();
        pool_.Put(obj);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = elapsed.count() / static_cast<double>(kIterations * 2);

    std::cout << "Average ns/op (Get+Put): " << ns_per_op << "\n";

    // 如果命中率高，延迟应该很低
    // 具体阈值需要根据 Phase 0 数据确定
}

// Phase 2 特有测试：批量缓存行为
TEST_F(ObjectPoolTest, BatchCacheBehavior) {
    // 连续Put多个对象，验证批量转移行为
    constexpr int kBatchSize = 10;
    std::vector<TestNode*> objects;

    for (int i = 0; i < kBatchSize; ++i) {
        objects.push_back(pool_.Get());
    }

    std::size_t size_before = pool_.Size();

    for (auto* obj : objects) {
        pool_.Put(obj);
    }

    std::size_t size_after = pool_.Size();

    // Put后 pool 大小应该增加
    EXPECT_GE(size_after, size_before);
}
```

---

### 2. LSCQ 真实场景模拟测试 🆕

**测试文件**: `tests/integration/test_lscq_realistic.cpp`

```cpp
#include <gtest/gtest.h>
#include <lscq/lscq.hpp>
#include <thread>
#include <vector>
#include <chrono>

using namespace lscq;

class LSCQRealisticTest : public ::testing::Test {
protected:
    static constexpr std::size_t kSCQSize = 256;
};

// 场景1: 模拟 enqueue 扩容场景
// Pool: Get() → 使用节点 → 可能被其他线程 Put()
TEST_F(LSCQRealisticTest, EnqueueExpansionScenario) {
    LSCQ<uint64_t> queue(64);  // 小容量强制扩容
    std::atomic<bool> stop{false};
    std::atomic<long> enqueue_success{0};
    std::atomic<long> enqueue_fail{0};

    // 16个生产者高并发enqueue
    std::vector<std::thread> producers;
    for (int i = 0; i < 16; ++i) {
        producers.emplace_back([&]() {
            for (int j = 0; j < 5000 && !stop.load(); ++j) {
                auto* item = new uint64_t(j);
                if (queue.enqueue(item)) {
                    enqueue_success.fetch_add(1);
                } else {
                    enqueue_fail.fetch_add(1);
                    delete item;
                }
            }
        });
    }

    // 慢速消费者（让队列膨胀）
    std::thread consumer([&]() {
        while (!stop.load()) {
            if (auto* item = queue.dequeue()) {
                delete item;
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }
    });

    for (auto& t : producers) t.join();
    stop.store(true);
    consumer.join();

    std::cout << "Enqueue success: " << enqueue_success.load() << "\n";
    std::cout << "Enqueue fail:    " << enqueue_fail.load() << "\n";

    // 清理
    while (auto* item = queue.dequeue()) delete item;
}

// 场景2: 模拟 dequeue 推进场景
// Pool: 只有 Put()，没有配对的 Get()
TEST_F(LSCQRealisticTest, DequeueAdvanceScenario) {
    LSCQ<uint64_t> queue(kSCQSize);

    // 先填充队列
    constexpr int kItems = 10000;
    for (int i = 0; i < kItems; ++i) {
        queue.enqueue(new uint64_t(i));
    }

    std::atomic<long> dequeue_count{0};

    // 多个消费者快速dequeue
    std::vector<std::thread> consumers;
    for (int i = 0; i < 8; ++i) {
        consumers.emplace_back([&]() {
            while (auto* item = queue.dequeue()) {
                dequeue_count.fetch_add(1);
                delete item;
            }
        });
    }

    for (auto& t : consumers) t.join();

    EXPECT_EQ(dequeue_count.load(), kItems);
}

// 场景3: 模拟 enqueue 竞争失败场景
// Pool: Get() → 立即 Put()（CAS失败）
TEST_F(LSCQRealisticTest, EnqueueCompetitionScenario) {
    // 这个场景很难直接模拟，通过高并发来间接触发
    LSCQ<uint64_t> queue(32);  // 极小容量
    std::atomic<bool> stop{false};
    std::atomic<long> total_ops{0};

    // 大量线程竞争
    std::vector<std::thread> workers;
    for (int i = 0; i < 32; ++i) {
        workers.emplace_back([&]() {
            for (int j = 0; j < 1000 && !stop.load(); ++j) {
                auto* item = new uint64_t(j);
                queue.enqueue(item);
                if (auto* dequeued = queue.dequeue()) {
                    delete dequeued;
                }
                total_ops.fetch_add(1);
            }
        });
    }

    for (auto& t : workers) t.join();

    std::cout << "Total operations: " << total_ops.load() << "\n";

    // 清理
    while (auto* item = queue.dequeue()) delete item;
}

// 场景4: 不对称负载 - 更多的Put（模拟LSCQ的真实情况）
TEST_F(LSCQRealisticTest, AsymmetricPutHeavyLoad) {
    LSCQ<uint64_t> queue(kSCQSize);
    std::atomic<bool> stop{false};

    // 生产者：正常速率
    std::thread producer([&]() {
        for (int i = 0; i < 10000 && !stop.load(); ++i) {
            queue.enqueue(new uint64_t(i));
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });

    // 消费者：快速消费（产生大量Put）
    std::vector<std::thread> consumers;
    for (int i = 0; i < 4; ++i) {
        consumers.emplace_back([&]() {
            while (!stop.load()) {
                if (auto* item = queue.dequeue()) {
                    delete item;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    producer.join();

    // 等待消费完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);

    for (auto& t : consumers) t.join();

    // 清理
    while (auto* item = queue.dequeue()) delete item;
}
```

---

### 3. 压力测试 (更新)

**测试文件**: `tests/stress/test_object_pool_stress.cpp`

```cpp
#include <gtest/gtest.h>
#include <lscq/object_pool.hpp>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <chrono>

using namespace lscq;

class ObjectPoolStressTest : public ::testing::Test {
protected:
    struct Node {
        char data[64];  // 模拟LSCQ节点大小
        explicit Node(int v = 0) { data[0] = static_cast<char>(v); }
    };
};

// 压力测试1: 高频Get/Put（基础）
TEST_F(ObjectPoolStressTest, HighFrequencyGetPut) {
    const int kThreads = std::thread::hardware_concurrency() * 2;
    const int kIterations = 500000;

    ObjectPool<Node> pool([] { return new Node(42); });
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < kIterations; ++j) {
                Node* obj = pool.Get();
                if (!obj) {
                    errors.fetch_add(1);
                    continue;
                }
                // 模拟使用
                benchmark::DoNotOptimize(obj->data[0]);
                pool.Put(obj);
            }
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(errors.load(), 0) << "Get failures detected";
    std::cout << "Final pool size: " << pool.Size() << "\n";
}

// 压力测试2: 随机延迟（模拟真实场景的不确定性）
TEST_F(ObjectPoolStressTest, RandomDelayPattern) {
    const int kThreads = 8;
    const int kIterations = 100000;

    ObjectPool<Node> pool([] { return new Node(42); });
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            std::mt19937 rng(i * 12345);
            std::uniform_int_distribution<int> delay_dist(0, 100);

            for (int j = 0; j < kIterations; ++j) {
                Node* obj = pool.Get();
                if (!obj) {
                    errors.fetch_add(1);
                    continue;
                }

                // 随机延迟
                if (delay_dist(rng) < 10) {  // 10%概率延迟
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(delay_dist(rng)));
                }

                pool.Put(obj);
            }
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(errors.load(), 0);
}

// 压力测试3: 不对称Get/Put比例（模拟LSCQ）🆕
TEST_F(ObjectPoolStressTest, AsymmetricGetPutRatio) {
    const int kGetThreads = 2;   // 少量Get（enqueue扩容）
    const int kPutThreads = 6;   // 大量Put（dequeue推进）
    const int kDuration = 5;     // 秒

    ObjectPool<Node> pool([] { return new Node(42); });
    std::atomic<bool> stop{false};
    std::atomic<long> get_count{0};
    std::atomic<long> put_count{0};

    // 存储Get获取的对象，供Put线程使用
    std::vector<std::atomic<Node*>> shared_objects(100);
    for (auto& obj : shared_objects) obj.store(nullptr);
    std::atomic<int> put_index{0};

    // Get线程
    std::vector<std::thread> getters;
    for (int i = 0; i < kGetThreads; ++i) {
        getters.emplace_back([&]() {
            while (!stop.load()) {
                Node* obj = pool.Get();
                if (obj) {
                    get_count.fetch_add(1);
                    // 随机放入共享数组供Put线程使用
                    int idx = put_index.fetch_add(1) % shared_objects.size();
                    Node* expected = nullptr;
                    if (!shared_objects[idx].compare_exchange_strong(expected, obj)) {
                        pool.Put(obj);  // 槽位被占用，直接Put回去
                    }
                }
            }
        });
    }

    // Put线程（模拟dequeue后的Put）
    std::vector<std::thread> putters;
    for (int i = 0; i < kPutThreads; ++i) {
        putters.emplace_back([&, i]() {
            while (!stop.load()) {
                int idx = (i * 13 + put_count.load()) % shared_objects.size();
                Node* obj = shared_objects[idx].exchange(nullptr);
                if (obj) {
                    put_count.fetch_add(1);
                    pool.Put(obj);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(kDuration));
    stop.store(true);

    for (auto& t : getters) t.join();
    for (auto& t : putters) t.join();

    // 清理剩余对象
    for (auto& obj : shared_objects) {
        if (Node* p = obj.load()) pool.Put(p);
    }

    std::cout << "Get count: " << get_count.load() << "\n";
    std::cout << "Put count: " << put_count.load() << "\n";
    std::cout << "Get/Put ratio: " << (double)get_count.load() / put_count.load() << "\n";
}

// 压力测试4: 突发流量
TEST_F(ObjectPoolStressTest, BurstyWorkload) {
    ObjectPool<Node> pool([] { return new Node(42); });

    const int kNumBursts = 10;
    const int kThreadsPerBurst = 16;
    const int kOpsPerThread = 50000;

    for (int burst = 0; burst < kNumBursts; ++burst) {
        std::vector<std::thread> threads;
        std::atomic<long> ops{0};

        for (int i = 0; i < kThreadsPerBurst; ++i) {
            threads.emplace_back([&]() {
                for (int j = 0; j < kOpsPerThread; ++j) {
                    Node* obj = pool.Get();
                    ASSERT_NE(obj, nullptr);
                    pool.Put(obj);
                    ops.fetch_add(1);
                }
            });
        }

        for (auto& t : threads) t.join();

        std::cout << "Burst " << burst + 1 << ": " << ops.load() << " ops\n";

        // 突发之间休息
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
```

---

### 4. 析构安全测试 (更新)

**测试文件**: `tests/lifecycle/test_object_pool_lifecycle.cpp`

```cpp
#include <gtest/gtest.h>
#include <lscq/object_pool.hpp>
#include <thread>
#include <atomic>
#include <memory>

using namespace lscq;

class ObjectPoolLifecycleTest : public ::testing::Test {
protected:
    struct Node {
        int value;
        explicit Node(int v = 0) : value(v) {}
    };
};

// 场景1: Pool先于线程销毁 (危险场景)
TEST_F(ObjectPoolLifecycleTest, PoolDiesBeforeThreads) {
    std::atomic<bool> pool_alive{true};
    std::atomic<bool> stop{false};
    std::atomic<int> operations{0};
    std::vector<std::thread> threads;

    {
        ObjectPool<Node> pool([] { return new Node(42); });

        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&]() {
                while (!stop.load() && pool_alive.load()) {
                    Node* obj = pool.Get();
                    if (obj) {
                        operations.fetch_add(1);
                        std::this_thread::sleep_for(std::chrono::microseconds(10));
                        pool.Put(obj);
                    }
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
        pool_alive.store(false);  // 标记即将销毁
        // pool 在此销毁
    }

    stop.store(true);
    for (auto& t : threads) t.join();

    std::cout << "Operations before pool destruction: " << operations.load() << "\n";
    // 如果到达这里没有崩溃，测试通过
    SUCCEED();
}

// 场景2: 线程先于Pool退出 (安全场景)
TEST_F(ObjectPoolLifecycleTest, ThreadsDieBeforePool) {
    ObjectPool<Node> pool([] { return new Node(42); });
    std::atomic<bool> stop{false};

    {
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&]() {
                while (!stop.load()) {
                    Node* obj = pool.Get();
                    if (obj) pool.Put(obj);
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
        stop.store(true);
        for (auto& t : threads) t.join();
        // 线程在此全部退出
    }

    // Pool继续使用
    Node* obj = pool.Get();
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->value, 42);
    pool.Put(obj);
}

// 场景3: 并发创建和销毁多个Pool
TEST_F(ObjectPoolLifecycleTest, ConcurrentPoolLifecycles) {
    const int kNumPools = 8;
    const int kThreadsPerPool = 4;

    std::vector<std::thread> threads;

    for (int pool_id = 0; pool_id < kNumPools; ++pool_id) {
        threads.emplace_back([pool_id]() {
            auto pool = std::make_unique<ObjectPool<Node>>(
                [] { return new Node(42); }
            );

            std::vector<std::thread> workers;
            std::atomic<bool> stop{false};

            for (int i = 0; i < kThreadsPerPool; ++i) {
                workers.emplace_back([&]() {
                    while (!stop.load()) {
                        Node* obj = pool->Get();
                        if (obj) pool->Put(obj);
                    }
                });
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stop.store(true);
            for (auto& w : workers) w.join();
            // pool 自动析构
        });
    }

    for (auto& t : threads) t.join();
    SUCCEED();
}

// 场景4: 析构超时测试 (Phase 3)
TEST_F(ObjectPoolLifecycleTest, DestructionTimeout) {
    // 这个测试验证析构时的闭锁机制是否有超时保护
    // 需要 ObjectPool 提供析构时间统计

    auto start = std::chrono::high_resolution_clock::now();

    {
        ObjectPool<Node> pool([] { return new Node(42); });
        std::atomic<bool> stop{false};

        // 创建一些活跃线程
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&]() {
                while (!stop.load()) {
                    Node* obj = pool.Get();
                    if (obj) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        pool.Put(obj);
                    }
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stop.store(true);
        for (auto& t : threads) t.join();
        // pool 在此析构
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Total lifecycle duration: " << duration.count() << "ms\n";

    // 析构不应该超过2秒
    EXPECT_LT(duration.count(), 2000);
}

// 场景5: thread_local方案特有测试 - 多Pool共存
TEST_F(ObjectPoolLifecycleTest, MultiplePoolsCoexist) {
    // 验证多个Pool实例可以安全共存
    ObjectPool<Node> pool1([] { return new Node(1); });
    ObjectPool<Node> pool2([] { return new Node(2); });
    ObjectPool<Node> pool3([] { return new Node(3); });

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < 1000; ++j) {
                // 交替使用不同的Pool
                Node* obj1 = pool1.Get();
                Node* obj2 = pool2.Get();
                Node* obj3 = pool3.Get();

                ASSERT_NE(obj1, nullptr);
                ASSERT_NE(obj2, nullptr);
                ASSERT_NE(obj3, nullptr);

                EXPECT_EQ(obj1->value, 1);
                EXPECT_EQ(obj2->value, 2);
                EXPECT_EQ(obj3->value, 3);

                pool1.Put(obj1);
                pool2.Put(obj2);
                pool3.Put(obj3);
            }
        });
    }

    for (auto& t : threads) t.join();
}
```

---

### 5. 内存安全检测

**保持v1.0的内容，补充以下内容**:

#### 5.1 增强的调试计数器

```cpp
// 在 object_pool.hpp 中添加（仅Debug模式）
#ifdef _DEBUG

struct PoolDebugStats {
    std::atomic<long> total_gets{0};
    std::atomic<long> total_puts{0};
    std::atomic<long> cache_hits{0};
    std::atomic<long> cache_misses{0};
    std::atomic<long> factory_calls{0};
    std::atomic<long> current_live{0};

    void Reset() {
        total_gets = 0;
        total_puts = 0;
        cache_hits = 0;
        cache_misses = 0;
        factory_calls = 0;
        current_live = 0;
    }

    void Print() const {
        std::cout << "=== ObjectPool Debug Stats ===\n";
        std::cout << "Total Gets:     " << total_gets << "\n";
        std::cout << "Total Puts:     " << total_puts << "\n";
        std::cout << "Cache Hits:     " << cache_hits << "\n";
        std::cout << "Cache Misses:   " << cache_misses << "\n";
        std::cout << "Factory Calls:  " << factory_calls << "\n";
        std::cout << "Current Live:   " << current_live << "\n";

        long hits = cache_hits.load();
        long total = hits + cache_misses.load();
        double hit_rate = total > 0 ? hits * 100.0 / total : 0;
        std::cout << "Hit Rate:       " << std::fixed << std::setprecision(1)
                  << hit_rate << "%\n";
        std::cout << "==============================\n";
    }
};

// 全局/Per-Pool 统计实例
#endif
```

---

### 6. 性能基准测试 (更新)

**更新预期性能表**:

| 场景 | Baseline | Phase 1 (Map) | Phase 1 (TLS) | Phase 2 | Phase 3 |
|------|----------|---------------|---------------|---------|---------|
| 单线程Get/Put | 50-100ns | **30-50ns** | **5-10ns** | 5-10ns | 5-10ns (+15ns闭锁) |
| 命中时延迟 | N/A | 30-50ns | 5-10ns | 5-10ns | 5-10ns |
| 未命中延迟 | 50-100ns | 60-120ns | 60-120ns | 50-100ns | 50-100ns |
| 4线程并发 | 80-150ns | 50-100ns | 20-40ns | 15-30ns | 20-40ns |
| 本地缓存命中率 | N/A | 30-50% | 30-50% | 60-80% | 60-80% |

> ⚠️ 注意：这些数值需要通过Phase 0实测验证！

---

## 测试执行流程 (更新)

```
┌─────────────────────────────────────────────────────────────────────┐
│                       完整测试流程 v2.0                              │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐ │
│  │ Phase 0: 基准数据收集 (开始前必须完成)                         │ │
│  │   1. 运行 benchmark_components.cpp                            │ │
│  │   2. 运行 test_lscq_usage_pattern.cpp                         │ │
│  │   3. 分析数据，做出方案决策                                    │ │
│  │   4. 更新优化计划文档                                          │ │
│  └───────────────────────────────────────────────────────────────┘ │
│                              ↓                                      │
│  ┌───────────────────────────────────────────────────────────────┐ │
│  │ Phase N 开发                                                   │ │
│  │   ↓                                                            │ │
│  │ 1. 单元测试 → 编译通过 + UT通过                                │ │
│  │   ↓                                                            │ │
│  │ 2. LSCQ真实场景测试 → 4个场景全过                              │ │
│  │   ↓                                                            │ │
│  │ 3. 压力测试 (10分钟) → 无崩溃、无错误                          │ │
│  │   ↓                                                            │ │
│  │ 4. 内存检测 → CRT无泄漏、VS诊断内存稳定                        │ │
│  │   ↓                                                            │ │
│  │ 5. 析构安全测试 → 5个场景全过                                  │ │
│  │   ↓                                                            │ │
│  │ 6. 性能基准对比 → 达到预期提升                                 │ │
│  │   ↓                                                            │ │
│  │ 7. 调试统计验证 → 命中率达标                                   │ │
│  │   ↓                                                            │ │
│  │ ✅ Phase N 验证通过                                            │ │
│  └───────────────────────────────────────────────────────────────┘ │
│                              ↓                                      │
│                    进入下一Phase 或 完成                            │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 检查清单 (更新)

### Phase 0 检查清单

- [ ] 📊 benchmark_components 运行完成
- [ ] 📊 记录 BM_FullPath_MapBased 结果: ___ns
- [ ] 📊 记录 BM_FullPath_ThreadLocal 结果: ___ns
- [ ] 📊 test_lscq_usage_pattern 运行完成
- [ ] 📊 记录 Get/Put 比例: ___
- [ ] 📊 记录潜在命中率: ___%
- [ ] ✅ 方案决策已确定: [ ] Map方案 / [ ] thread_local方案
- [ ] 📝 决策理由已记录

### Phase 1-3 检查清单 (每个Phase)

- [ ] ✅ 所有单元测试通过
- [ ] 🎯 LSCQ真实场景测试4个全过
- [ ] 🔥 压力测试运行10分钟无崩溃
- [ ] 💾 CRT Debug Heap 无泄漏
- [ ] 💾 VS Diagnostic Tools 内存曲线平稳
- [ ] 🛡️ 析构安全测试5个场景全过
- [ ] ⚡ 性能基准达到预期
- [ ] 📊 命中率达到预期: ___%
- [ ] 📝 代码Review无明显问题

---

## 附录: 测试文件目录结构

```
tests/
├── unit/
│   └── test_object_pool.cpp         # 单元测试
├── integration/
│   └── test_lscq_realistic.cpp      # LSCQ真实场景测试 🆕
├── stress/
│   ├── test_object_pool_stress.cpp  # 压力测试
│   └── test_object_pool_longevity.cpp # 长时间测试
├── lifecycle/
│   └── test_object_pool_lifecycle.cpp # 析构安全测试 🆕
├── analysis/
│   └── test_lscq_usage_pattern.cpp  # 使用模式分析 🆕
└── main.cpp                          # 测试入口(带CRT调试)

benchmarks/
├── benchmark_components.cpp          # 组件开销基准 🆕
├── benchmark_object_pool.cpp         # ObjectPool性能基准
└── benchmark_lscq.cpp               # LSCQ性能基准 (已有)
```

---

*文档版本: 2.0*
*更新日期: 2026-01-25*
*下次Review: Phase 0 完成后*
