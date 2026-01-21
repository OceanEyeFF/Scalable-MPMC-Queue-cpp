# 阶段9: Git仓库管理与项目发布 - 开发计划

## 概述
完善项目的Git配置、安全策略和元数据文件,为项目的版本管理和开源发布做好准备。

## 任务分解

### Task 1: 创建Git配置文件
- **ID**: task-1
- **type**: default
- **描述**: 创建完整的.gitignore(忽略构建产物、测试输出、IDE配置)、.gitattributes(统一换行符处理)和.gitmessage(提交信息模板)文件
- **文件范围**: .gitignore, .gitattributes, .gitmessage
- **依赖关系**: None
- **测试命令**:
  ```bash
  # 验证.gitignore是否覆盖所有构建产物
  git init && git add -A && git status
  # 验证.gitattributes语法
  git check-attr text *.cpp *.h
  ```
- **测试重点**:
  - .gitignore必须包含: build/, *.gcno, *.gcda, *.profdata, *.profraw, benchmarks/out/, nul
  - .gitignore必须包含: CMakeCache.txt, CMakeFiles/, cmake_install.cmake, Makefile, compile_commands.json
  - .gitignore必须包含: .vscode/, .idea/, *.swp, .DS_Store
  - .gitattributes必须正确设置文本文件和二进制文件的换行符策略
  - .gitmessage必须包含合理的提交信息模板结构(类型、范围、描述等)

### Task 2: 创建安全策略文档
- **ID**: task-2
- **type**: quick-fix
- **描述**: 创建SECURITY.md文档,说明漏洞报告流程、响应时间承诺和联系方式(GitHub Issues)
- **文件范围**: SECURITY.md
- **依赖关系**: None
- **测试命令**:
  ```bash
  # 验证文件存在且格式正确
  test -f SECURITY.md && markdown-lint SECURITY.md
  # 验证包含必要章节
  grep -E "(Reporting|Vulnerability|Security)" SECURITY.md
  ```
- **测试重点**:
  - 文档必须包含明确的漏洞报告流程
  - 必须说明通过GitHub Issues报告安全问题的方法
  - 包含支持的版本信息
  - 包含响应时间预期(如48小时内回复)
  - 使用清晰的Markdown格式,易于阅读

### Task 3: 更新许可证文件
- **ID**: task-3
- **type**: quick-fix
- **描述**: 更新LICENSE文件,填入版权持有人"OceanEyeFF"和当前年份(2026)
- **文件范围**: LICENSE
- **依赖关系**: None
- **测试命令**:
  ```bash
  # 验证版权信息存在
  grep -i "OceanEyeFF" LICENSE
  grep "2026" LICENSE
  # 验证许可证类型保持不变
  grep -E "(MIT License|Apache|BSD)" LICENSE
  ```
- **测试重点**:
  - 版权行必须包含"OceanEyeFF"
  - 年份必须为2026或合理的年份范围(如2024-2026)
  - 许可证类型和条款保持原样,仅更新版权信息
  - 不破坏原有许可证格式

### Task 4: 更新行为准则
- **ID**: task-4
- **type**: quick-fix
- **描述**: 更新CODE_OF_CONDUCT.md中的联系方式,改为通过GitHub Issues报告问题
- **文件范围**: CODE_OF_CONDUCT.md
- **依赖关系**: None
- **测试命令**:
  ```bash
  # 验证包含GitHub Issues联系方式
  grep -i "GitHub Issues" CODE_OF_CONDUCT.md
  # 验证移除了其他联系方式(如邮箱)
  ! grep -E "[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}" CODE_OF_CONDUCT.md || true
  ```
- **测试重点**:
  - 明确说明通过GitHub Issues报告行为准则违规行为
  - 移除或替换原有的邮箱等联系方式
  - 保持行为准则的其他内容完整性
  - 格式清晰,易于社区成员理解

### Task 5: 更新README文档徽章
- **ID**: task-5
- **type**: quick-fix
- **描述**: 在README.md中添加CI工作流徽章,确保所有徽章链接正确且有效
- **文件范围**: README.md
- **依赖关系**: None
- **测试命令**:
  ```bash
  # 验证徽章格式正确
  grep -E "!\[.*\]\(https://.*\)" README.md
  # 验证包含CI徽章
  grep -i "badge" README.md | grep -i "ci\|workflow\|build"
  # 可选: 验证徽章链接可访问(需要网络)
  # curl -I $(grep -oP '(?<=\()https://[^)]+' README.md | head -1)
  ```
- **测试重点**:
  - 添加GitHub Actions CI工作流徽章(格式: ![CI](https://github.com/{user}/{repo}/workflows/{workflow}/badge.svg))
  - 确保现有徽章(如许可证、版本等)链接有效
  - 徽章按逻辑顺序排列(CI、许可证、版本、覆盖率等)
  - 徽章图片能正确渲染,链接指向正确的目标

## 验收标准
- [ ] 创建.gitignore文件,覆盖所有C++/CMake构建产物和IDE配置
- [ ] 创建.gitattributes文件,正确处理文本和二进制文件换行符
- [ ] 创建.gitmessage文件,提供规范的提交信息模板
- [ ] 创建SECURITY.md文档,说明安全漏洞报告流程(通过GitHub Issues)
- [ ] 更新LICENSE文件,版权持有人为"OceanEyeFF"
- [ ] 更新CODE_OF_CONDUCT.md,联系方式改为GitHub Issues
- [ ] 更新README.md,添加CI工作流徽章且所有徽章链接有效
- [ ] 所有文件格式规范,无语法错误
- [ ] 所有配置文件经过实际验证(git命令测试)
- [ ] 代码覆盖率 ≥90% (注: 此阶段主要为配置文件,覆盖率要求不适用)

## 技术说明
- **范围限制**: 本阶段仅处理本地配置,不涉及远程仓库推送或GitHub Release操作
- **Git初始化**: 任务完成后需要执行`git init`初始化仓库,但不包含在本阶段任务中
- **文件编码**: 所有文件使用UTF-8编码,换行符按.gitattributes规则处理
- **版权年份**: 使用2026年作为当前年份
- **联系方式**: 统一使用GitHub Issues作为项目沟通渠道
- **徽章服务**: CI徽章使用GitHub Actions原生徽章服务,格式为`https://github.com/{user}/{repo}/workflows/{workflow}/badge.svg`
- **安全策略**: SECURITY.md遵循GitHub安全建议指南(https://docs.github.com/en/code-security/security-advisories)
- **行为准则**: CODE_OF_CONDUCT.md基于Contributor Covenant标准
- **忽略规则优先级**: .gitignore规则按通用规则→语言规则→项目规则组织,确保覆盖全面

## 关键约束
- 不修改项目源代码(src/、include/、tests/、benchmarks/目录)
- 不修改构建配置(CMakeLists.txt、.github/workflows/)
- 所有变更限于根目录的元数据和配置文件
- 保持现有文档结构,仅更新必要字段
- 遵循开源社区最佳实践和GitHub推荐格式
