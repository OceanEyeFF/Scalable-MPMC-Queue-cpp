# LSCQ项目 StarterPrompt 索引

> **文档说明**: 本文档提供所有StarterPrompt的快速索引和使用指南
> **创建日期**: 2026-01-18
> **维护者**: LSCQ项目团队

---

## 1. StarterPrompt概述

### 1.1 什么是StarterPrompt？
StarterPrompt是**自包含的、结构化的任务描述文档**，用于指导AI Agent或开发者完成特定阶段的开发工作。每个StarterPrompt包含：
- 明确的任务边界（In Scope / Out of Scope）
- 详细的任务清单和验收标准
- 下一阶段的预览和交接文档要求

### 1.2 为什么需要StarterPrompt？
1. **任务隔离**: 每个Phase独立，可由不同的开发会话完成
2. **验收明确**: 清晰的Gate条件，便于质量控制
3. **连贯性**: 通过交接文档保证阶段间的平滑过渡
4. **可追溯**: 每个阶段的交付物和决策都有文档记录

---

## 2. 项目阶段概览

```
Phase 0: 项目初始化 [已完成]
   └─ 技术调研、文档撰写、开发计划制定

Phase 1: 基础设施搭建 (1-2周)
   ├─ 项目结构
   ├─ 构建系统（CMake + Ninja）
   ├─ CAS2检测与抽象层
   └─ 测试框架（Google Test + Google Benchmark）

Phase 2: NCQ实现与验证 (1周)
   ├─ NCQ算法实现（Baseline）
   ├─ Ring Buffer + FAA + CAS2
   ├─ 正确性测试
   └─ 性能Benchmark

Phase 3: SCQ核心实现 (2-3周)
   ├─ Threshold机制（3n-1）
   ├─ isSafe标志位
   ├─ Catchup优化（fixState）
   ├─ Atomic_OR优化
   └─ Livelock测试

Phase 4: SCQP双字宽CAS扩展 (1周)
   ├─ 指针队列（{cycle, T*}）
   ├─ 4n-1 Threshold
   ├─ 队列满检测
   └─ 性能对比

Phase 5: LSCQ无界队列 (2周)
   ├─ 链表结构（SCQNode）
   ├─ Finalize机制
   ├─ Epoch-Based Reclamation
   └─ 内存泄漏测试

Phase 6: 优化与多平台支持 (2-3周)
   ├─ 性能Profiling与优化
   ├─ ARM64移植
   ├─ Fallback方案
   └─ 文档完善与CI/CD

Phase 7: Release编译与性能对比测试 (1-2周)
   ├─ Release编译配置优化
   ├─ 实现基准队列（MSQueue、MutexQueue）
   ├─ 6种Benchmark场景测试
   ├─ 多线程扩展性测试
   └─ 与论文数据对比分析

Phase 8: 文档补全和使用说明 (1-2周)
   ├─ README.md完善
   ├─ 使用教程编写
   ├─ API文档补全（Doxygen）
   ├─ 示例代码创建
   ├─ 故障排查指南
   └─ 贡献指南和License
```

Phase 9: Git仓库管理与项目发布 (2-3天)
   ├─ .gitignore配置(基于实际构建产物)
   ├─ .gitattributes换行符规范
   ├─ LICENSE和项目治理文档
   ├─ 远程仓库配置(Gitee/GitHub)
   ├─ 提交规范(Conventional Commits)
   └─ v1.0.0版本发布
```

**总预估时间**: 15-20周

---

## 3. StarterPrompt文件列表

| Phase | 文件名 | 任务描述 | 预计工期 | 状态 |
|-------|--------|----------|----------|------|
| 1 | [Phase1-基础设施搭建.md](Phase1-基础设施搭建.md) | 项目结构、CMake、CAS2抽象、测试框架 | 1-2周 | ✅ Ready |
| 2 | [Phase2-NCQ实现与验证.md](Phase2-NCQ实现与验证.md) | NCQ算法、Ring Buffer、Baseline性能 | 1周 | ✅ Ready |
| 3 | [Phase3-SCQ核心实现.md](Phase3-SCQ核心实现.md) | Threshold、isSafe、Catchup、Livelock | 2-3周 | ✅ Ready |
| 4 | [Phase4-SCQP双字宽CAS扩展.md](Phase4-SCQP双字宽CAS扩展.md) | 指针队列、4n-1、队列满检测 | 1周 | ✅ Ready |
| 5 | [Phase5-LSCQ无界队列.md](Phase5-LSCQ无界队列.md) | 链表结构、EBR、Finalize、内存回收 | 2周 | ✅ Ready |
| 6 | [Phase6-优化与多平台支持.md](Phase6-优化与多平台支持.md) | Profiling、ARM64、文档、CI/CD | 2-3周 | ✅ Ready |
| 7 | [Phase7-Release编译与性能对比测试.md](Phase7-Release编译与性能对比测试.md) | Release配置、基准队列、6种场景、论文对比 | 1-2周 | ✅ Ready |
| 8 | [Phase8-文档补全和使用说明.md](Phase8-文档补全和使用说明.md) | README、教程、API文档、示例、贡献指南 | 1-2周 | ✅ Ready |
| 9 | [Phase9-Git仓库管理与项目发布.md](Phase9-Git仓库管理与项目发布.md) | .gitignore、LICENSE、远程仓库、v1.0.0发布 | 2-3天 | ✅ Ready |

---

## 4. 使用指南

### 4.1 开始新阶段的步骤

**Step 1: 阅读前置文档**
```bash
# 假设开始Phase 3
1. 阅读 docs/Phase2-交接文档.md（前一阶段的交接文档）
2. 阅读 docs/starter-prompts/Phase3-SCQ核心实现.md（当前阶段的StarterPrompt）
3. 阅读 docs/01-技术实现思路.md 的相关章节
```

**Step 2: 验证前置依赖**
```bash
# 检查Phase 2是否完成
- [ ] docs/Phase2-交接文档.md 已创建
- [ ] 所有Phase 2的验收Gate通过
- [ ] NCQ性能达标
```

**Step 3: 执行任务清单**
按照StarterPrompt中的"详细任务清单"逐项完成：
- Day 1: 数据结构定义
- Day 2-3: 核心算法实现
- Day 4-5: 单元测试
- ...

**Step 4: 验收与交接**
```bash
# 完成所有任务后
1. 运行所有测试（单元测试 + Benchmark）
2. 确认所有验收Gate通过
3. 创建交接文档（docs/Phase3-交接文档.md）
4. 提交代码（如果使用Git）
```

**Step 5: 进入下一阶段**
重复Step 1-4，使用下一个StarterPrompt

---

### 4.2 交接文档的重要性

**每个阶段完成后必须创建交接文档！**

交接文档的作用：
1. **记录关键决策**: 为什么这样实现？遇到了什么问题？
2. **代码位置索引**: 关键函数在哪里？（精确到行号）
3. **性能数据**: 实际Benchmark结果（供下一阶段对比）
4. **接口预留**: 下一阶段需要哪些新接口？
5. **快速启动**: 下一阶段开发者应该先读什么？

**交接文档模板**（参见各StarterPrompt第8节）:
```markdown
# Phase N 交接文档

## 1. 完成情况概览
## 2. 关键代码位置索引
## 3. 算法验证结果
## 4. 性能Benchmark结果
## 5. 已知问题和限制
## 6. Phase N+1接口预留
## 7. 下阶段快速启动指南
## 8. 经验教训和最佳实践
```

---

### 4.3 验收标准（Gate Conditions）

每个Phase都有明确的验收标准，分为以下类别：

| 类别 | 说明 | 示例 |
|------|------|------|
| **G1.x: 编译验收** | 代码零警告编译通过 | `cmake --build build` 成功 |
| **G2.x: 功能验收** | 所有测试通过 | `ctest` 全部✅ |
| **G3.x: 性能验收** | 达到性能目标 | SCQ @ 16 threads > 50 Mops/sec |
| **G4.x: 质量验收** | 代码质量标准 | 覆盖率>90%, clang-tidy无错误 |
| **G5.x: 文档验收** | 交接文档完整 | 包含所有必需章节 |

**只有所有Gate通过，才能进入下一阶段！**

---

## 5. 常见问题（FAQ）

### Q1: 可以跳过某个Phase吗？
**A**: **不可以**。每个Phase都有明确的依赖关系，跳过会导致：
- 缺少必要的基础设施
- 后续Phase无法验证
- 无法追溯问题根源

### Q2: 某个Phase的工期超出预估怎么办？
**A**:
1. 检查是否理解了任务边界（Out of Scope的内容不要做）
2. 优先完成验收Gate，优化可以放到Phase 6
3. 记录在交接文档中，供项目管理参考

### Q3: StarterPrompt中的代码示例可以直接使用吗？
**A**: **可以作为参考，但需要根据实际情况调整**。示例代码展示了核心逻辑，但可能：
- 缺少错误处理
- 不符合你的代码风格
- 需要适配你的构建系统

### Q4: 如何处理StarterPrompt与实际开发的差异？
**A**:
1. 优先遵循StarterPrompt的任务边界和验收标准
2. 如果发现StarterPrompt有问题，记录在交接文档中
3. 更新StarterPrompt（为后续开发者提供改进）

### Q5: 交接文档写多少合适？
**A**: **不少于200行**（Phase 1-2）或**300行**（Phase 3-6）。包含：
- 代码位置索引（精确到行号）
- 实际性能数据（表格）
- 至少1-2个实际问题和解决方案

---

## 6. 最佳实践

### 6.1 任务执行
- ✅ **先读后写**: 充分阅读前置文档再开始编码
- ✅ **小步快跑**: 按Day计划逐步完成，避免一次性写大量代码
- ✅ **持续测试**: 每天结束前运行测试，确保功能正常
- ✅ **记录问题**: 遇到问题立即记录，供交接文档使用

### 6.2 代码质量
- ✅ **遵循现有风格**: 使用`.clang-format`统一格式
- ✅ **充分注释**: 复杂逻辑必须有注释（尤其是Threshold、Catchup）
- ✅ **单元测试覆盖**: 每个公开方法都有测试
- ✅ **Benchmark验证**: 性能测试与论文对比

### 6.3 文档编写
- ✅ **代码位置精确**: 使用行号（如`src/scq.cpp:L45-L52`）
- ✅ **性能数据真实**: 来自实际Benchmark，不要编造
- ✅ **问题描述详细**: 不仅说"遇到问题"，要说"为什么"和"怎么解决"
- ✅ **下阶段预留**: 明确指出下一阶段需要什么

---

## 7. 工具和资源

### 7.1 构建工具
- **CMake 3.20+**: 构建系统
- **Ninja**: 构建生成器（比Make快）
- **Clang++ 14+**: 主编译器

### 7.2 测试工具
- **Google Test 1.12+**: 单元测试框架
- **Google Benchmark 1.8+**: 性能测试框架
- **ThreadSanitizer**: 检测data race
- **AddressSanitizer**: 检测内存泄漏
- **Valgrind**: 深度内存分析

### 7.3 性能分析
- **perf** (Linux): CPU性能分析
- **Intel VTune** (Windows/Linux): 专业性能分析
- **Instruments** (macOS): Xcode内置

### 7.4 文档生成
- **Doxygen**: API文档生成
- **Markdown**: 技术文档编写

---

## 8. 联系方式

### 8.1 项目仓库
- **GitHub**: `https://github.com/yourname/lscq-cpp`
- **Gitee**: `https://gitee.com/yourname/Scaleable-MPMC-Queue-cpp`

### 8.2 文档路径
- **技术文档**: `docs/01-技术实现思路.md`
- **开发计划**: `docs/02-分段开发计划.md`
- **性能方案**: `docs/03-性能验证方案.md`
- **StarterPrompt**: `docs/starter-prompts/`

### 8.3 问题反馈
如果发现StarterPrompt有问题或需要改进，请：
1. 创建Issue描述问题
2. 提出改进建议
3. 提交PR更新文档

---

## 9. 版本历史

| 版本 | 日期 | 变更内容 | 作者 |
|------|------|----------|------|
| v1.0 | 2026-01-18 | 初始版本，创建Phase 1-6的StarterPrompt | LSCQ Team |
| v1.1 | 2026-01-20 | 新增Phase 7-8的StarterPrompt | LSCQ Team |
| v1.2 | 2026-01-20 | 新增Phase 9: Git仓库管理与项目发布 | LSCQ Team |

---

## 10. 附录：快速查找表

### 10.1 按关键技术查找

| 技术关键词 | 相关Phase | 文档位置 |
|------------|-----------|----------|
| CAS2 | Phase 1 | Phase1第4.3节 |
| Ring Buffer | Phase 2 | Phase2第4.2节 |
| Threshold | Phase 3 | Phase3第4.2节 |
| Catchup | Phase 3 | Phase3第4.3节 |
| 指针队列 | Phase 4 | Phase4第4.3节 |
| EBR | Phase 5 | Phase5第4.1节 |
| ARM64 | Phase 6 | Phase6第4.2节 |
| Release编译 | Phase 7 | Phase7第4.1节 |
| Benchmark对比 | Phase 7 | Phase7第4.3节 |
| API文档 | Phase 8 | Phase8第4.3节 |
| 示例代码 | Phase 8 | Phase8第4.4节 |
| Git配置 | Phase 9 | Phase9第3.1-3.4节 |
| 项目治理 | Phase 9 | Phase9第3.5-3.8节 |
| 版本发布 | Phase 9 | Phase9第3.11节 |

### 10.2 按验收类型查找

| 验收类型 | Phase 1 | Phase 2 | Phase 3 | Phase 4 | Phase 5 | Phase 6 | Phase 7 | Phase 8 | Phase 9 |
|----------|---------|---------|---------|---------|---------|---------|---------|---------|---------|
| 编译验收 | 第6.1节 | 第6.1节 | 第6.1节 | 第6.1节 | 第6.1节 | 第6.1节 | 第6.1节 | 第6.1节 | 第5.1节 |
| 功能验收 | 第6.2节 | 第6.2节 | 第6.2节 | 第6.2节 | 第6.2节 | 第6.2节 | -       | 第6.2节 | -       |
| 性能验收 | -       | 第6.3节 | 第6.3节 | 第6.3节 | 第6.3节 | 第6.1节 | 第6.2节 | -       | -       |
| 文档验收 | -       | -       | -       | 第6.4节 | 第6.4节 | 第6.4节 | 第6.4节 | 第6.3节 | 第5.2节 |

### 10.3 按交付物类型查找

| 交付物类型 | 示例文件 | 首次出现 |
|------------|----------|----------|
| 头文件 | `include/lscq/cas2.hpp` | Phase 1 |
| 源文件 | `src/ncq.cpp` | Phase 2 |
| 测试文件 | `tests/unit/test_scq.cpp` | Phase 3 |
| Benchmark | `benchmarks/benchmark_lscq.cpp` | Phase 5 |
| 基准队列 | `benchmarks/baseline/msqueue.hpp` | Phase 7 |
| 性能报告 | `docs/Phase7-性能报告.md` | Phase 7 |
| 示例代码 | `examples/simple_usage.cpp` | Phase 8 |
| API文档 | Doxygen HTML | Phase 8 |
| 使用教程 | `docs/Tutorial.md` | Phase 8 |
| Git配置 | `.gitignore`, `.gitattributes` | Phase 9 |
| 治理文档 | `LICENSE`, `CONTRIBUTING.md` | Phase 9 |
| 发布资产 | `v1.0.0.tar.gz` | Phase 9 |
| 交接文档 | `docs/Phase1-交接文档.md` | Phase 1 |

---

**索引文档版本**: v1.2
**创建日期**: 2026-01-18
**最后更新**: 2026-01-20
**维护者**: LSCQ项目团队

**祝开发顺利！** 🚀
