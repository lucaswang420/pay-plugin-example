# Pay Plugin Example - Claude Code 项目规范

> 本文档为 Claude Code 提供 Pay Plugin 项目的开发指导，技术规范请参考 [TECH_SPECS.md](TECH_SPECS.md)。

---

## 项目概述

**Drogon Payment Processing Plugin & Vue Admin Dashboard** - 企业级支付处理系统，支持支付宝沙箱、微信支付等多种支付平台，采用服务导向架构（SOA）。

**技术栈**: Drogon C++17 | PostgreSQL 13+ | Redis 6.0+ | Vue 3 + Element Plus | CMake 3.15+ | Conan

**项目结构**: `controllers/` → `services/` → `plugins/` → `models/` (ORM禁止修改)

**关键成就**: 测试覆盖率 80%+ (107+ 用例) | P50 < 15ms, P95 < 39ms | 生产就绪 | 完整 CI/CD

---

## 技术规范

遵循 [Drogon Pay Plugin 技术规范](TECH_SPECS.md)：禁止修改 ORM 类 | 禁止 raw SQL | 优先异步回调 | Lambda 捕获 `[sharedCb]` | 使用 Service API 而非 Plugin API

---

## 构建规则

| 命令 | 说明 |
|------|------|
| `cd PayBackend && scripts\build.bat` | Release 模式（默认） |
| `cd PayBackend && scripts\build.bat -debug` | Debug 模式 |

**注意**: 使用 build.bat 确保 Conan 依赖正确配置，避免直接使用 CMake 或 Visual Studio 构建。

---

## 配置管理

| 配置项 | 文件/变量 |
|--------|-----------|
| 主配置 | `PayBackend/config.json` |
| 敏感信息 | `PAY_DB_PASSWORD`, `PAY_REDIS_PASSWORD`, `PAY_API_KEY`, `ALIPAY_PRIVATE_KEY`, `WECHAT_PAY_KEY` |

---

## 测试规范

| 测试类型 | 命令 |
|----------|------|
| 单元测试 | `cd PayBackend && build/Release/test_payplugin.exe` |
| 集成测试 | `cd PayBackend && build/Release/test_payplugin.exe --gtest_filter=*Integration*` |
| 完整测试 | `scripts/test.bat` |

测试文件: `PayBackend/build/Release/test_payplugin.exe` | 运行位置: `PayBackend/` 目录

---

## 服务架构

| 服务 | 文件 | 职责 |
|------|------|------|
| PaymentService | `services/PaymentService.{h,cc}` | 支付创建和查询 |
| RefundService | `services/RefundService.{h,cc}` | 退款处理和查询 |
| CallbackService | `services/CallbackService.{h,cc}` | 支付回调处理 |
| IdempotencyService | `services/IdempotencyService.{h,cc}` | 幂等性管理 |
| ReconciliationService | `services/ReconciliationService.{h,cc}` | 对账和报表 |

**Service API**: `service->method(request, apiKey, callback)` | **禁止**: 旧 Plugin API

---

## 项目架构要点

### 支付流程

| 流程 | 端点 | 步骤 |
|------|------|------|
| 创建支付 | `POST /api/v1/payments` | 验证 API Key → 检查幂等性 → 创建记录 → 调用第三方 |
| 支付回调 | `POST /api/v1/callbacks/{provider}` | 验证签名 → 更新状态 → 触发业务逻辑 |
| 创建退款 | `POST /api/v1/refunds` | 验证 API Key → 检查支付状态 → 创建退款 → 调用第三方 |
| 查询支付 | `GET /api/v1/payments/{id}` | 验证 API Key → 查询记录 → 返回详情 |

### 数据模型

| 表名 | 模型文件 | 用途 |
|------|----------|------|
| pay_payment | `models/PayPayment.{h,cc}` | 支付记录 |
| pay_refund | `models/PayRefund.{h,cc}` | 退款记录 |
| pay_callback | `models/PayCallback.{h,cc}` | 回调记录 |
| pay_idempotency | `models/PayIdempotency.{h,cc}` | 幂等性键 |
| pay_ledger | `models/PayLedger.{h,cc}` | 账本记录 |

### 第三方集成

| 提供商 | 文件 |
|--------|------|
| 支付宝沙箱 | `plugins/AlipaySandboxClient.{h,cc}` |
| 微信支付 | `plugins/WechatPayClient.{h,cc}` |

### 前端

位置: `PayFrontend/` | 启动: `cd PayFrontend && npm run dev` | 技术栈: Vue 3 + Element Plus + Pinia

---

## 开发流程

| 规范项 | 要求 |
|--------|------|
| Git 操作 | 允许 commit，禁止 push（需审核） |
| 调试代码 | 解决后必须移除，使用 `LOG_DEBUG` |
| 完成标准 | 测试通过 + 静态分析通过 + CI 成功 + 文档更新 |

| 分支类型 | 命名规范 |
|----------|----------|
| feature | `feature/service-api-migration` |
| bugfix | `bugfix/fix-idempotency-check` |
| hotfix | `hotfix/critical-payment-fix` |
| refactor | `refactor/optimize-service-layer` |

---

## 部署监控

| 平台 | 构建 | 测试 | 启动 |
|------|------|------|------|
| Windows | `scripts\build.bat` | `scripts\test.bat` | `.\build\Release\PayServer.exe -c config.json` |
| Linux/macOS | `scripts/build.sh` | `cd build && ctest` | `./PayServer -c config.json` |

| 监控端点 | 说明 |
|----------|------|
| `GET /health` | 健康检查 |
| `GET /metrics` | Prometheus 指标 |
| `GET /api/v1/metrics/payments` | 支付统计 |

---

## 相关文档

| 文档 | 链接 | 说明 |
|------|------|------|
| 技术规范 | [TECH_SPECS.md](TECH_SPECS.md) | 架构、数据访问、安全规范 |
| 项目概述 | [README.md](README.md) | 项目概述和快速开始 |
| 部署文档 | [PayBackend/docs/](PayBackend/docs/) | 部署和运维指南 |
| API 文档 | [PayBackend/docs/api.md](PayBackend/docs/api.md) | API 接口文档 |
| 构建指南 | [PayBackend/scripts/README.build.md](PayBackend/scripts/README.build.md) | 详细构建说明 |

---

**文档版本**: v2.0 | **最后更新**: 2026-05-12 | **维护者**: Pay Plugin 开发团队
