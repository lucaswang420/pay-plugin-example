# Drogon Pay Plugin 技术规范

> 本文档定义 Drogon Pay Plugin 项目的技术规范，包括架构设计、编码标准、安全要求等。

---

## 一、架构规范

### [MUST] Drogon 框架优先原则
- 优先使用 Drogon 内置功能，避免引入三方库
- 引入新库必须在 PR 中说明必要性

### [MUST] 分层架构

| 层级 | 职责 | 关键要求 |
|------|------|----------|
| Controller 层 | HTTP 请求/响应 | 薄层设计，验证格式，调用 Service |
| Service 层 | 核心业务逻辑 | PaymentService, RefundService, CallbackService 等 |
| Plugin 层 | 第三方集成 | AlipaySandboxClient, WechatPayClient |
| Model 层 | ORM 映射 | 禁止修改 ORM 类，用 `drogon_ctl` 重新生成 |

### [MUST] 服务架构

项目采用服务导向架构（SOA），核心服务包括：

| 服务 | 职责 | 关键方法 |
|------|------|----------|
| PaymentService | 支付创建和查询 | `createPayment`, `getPaymentById`, `getPaymentsByApiKey` |
| RefundService | 退款处理和查询 | `createRefund`, `getRefundById`, `getRefundsByPaymentId` |
| CallbackService | 支付回调处理 | `handleAlipayCallback`, `handleWechatCallback` |
| IdempotencyService | 幂等性管理 | `checkIdempotency`, `createIdempotencyKey` |
| ReconciliationService | 对账和报表 | `generateDailyReport`, `reconcilePayments` |

### [MUST] 异步编程规范

| 接口类型 | 优先级 | 说明 |
|----------|--------|------|
| 异步回调 | [+] 最高 | `Mapper::findOne`, `execSqlAsync` |
| 同步接口 | [!] 限制 | `Mapper::findBy` with future（非必要禁止） |
| 协程接口 | [-] 禁止 | `CoroMapper`（严格禁止使用） |

**Lambda 捕获规范**:
- [+] 捕获 `sharedCb`: `[sharedCb]`
- [-] 捕获裸指针: `[this]`, `[&var]`
- 如需使用裸指针，必须在 PR 中说明生命周期保障方案 (`shared_from_this`, `weak_ptr`)

**Service API 调用规范**:
- [+] 使用新 Service API: `service->method(request, apiKey, callback)`
- [-] 禁止使用旧 Plugin API: `plugin.method(req, callback)`
- [+] 使用 `std::shared_ptr` 管理 callback 生命周期

---

## 二、数据访问规范

### [MUST] ORM 使用规范

| 操作 | 禁止 | 推荐 |
| :--- | :--- | :--- |
| SELECT | raw SQL | `Mapper::findBy` |
| INSERT | raw INSERT | `Mapper::insert` |
| UPDATE | raw UPDATE | `Mapper::update` |
| JOIN | JOIN 查询 | 拆分查询或 `Criteria::In` |

**允许使用 raw SQL 的特殊情况**:
- [+] PostgreSQL `UPDATE ... RETURNING` (原子操作)
- [+] DDL 操作 (表结构变更，需用 SchemaSetup.cc)
- [+] 批量操作优化 (需说明必要性)
- [+] 测试代码清理

### 关键规范

| 规范项 | 要求 |
|--------|------|
| Callback 生命周期 | 使用 `std::make_shared<CallbackType>(std::move(cb))` |
| 替代 JOIN | 拆分为多个 ORM 查询或使用 `Criteria::In` |
| 错误处理 | 所有异步回调都有错误处理分支 |
| Lambda 捕获 | 捕获 `[sharedCb]` 而非裸指针 |
| 幂等性检查 | 所有写操作必须先检查幂等性键 |

### [MUST] 数据库连接管理
- 读写分离: `dbClientMaster_` (写), `dbClientReader_` (读)
- 连接池配置在 `config.json` 中
- 异步操作使用共享的 DbClientPtr

### [MUST] 数据模型

| 表名 | 用途 | 关键字段 |
|------|------|----------|
| pay_payment | 支付记录 | payment_id, api_key, amount, status |
| pay_refund | 退款记录 | refund_id, payment_id, amount, status |
| pay_callback | 回调记录 | callback_id, payment_id, provider, payload |
| pay_idempotency | 幂等性键 | idempotency_key, operation_result |
| pay_ledger | 账本记录 | ledger_id, payment_id, refund_id, amount |

---

## 三、代码质量规范

### [MUST] 代码风格

| 规范项 | 要求 |
|--------|------|
| 语言标准 | C++17 |
| 风格指南 | Google C++ Style Guide (Drogon 默认) |
| 行长度限制 | 100 字符 |
| 格式化工具 | clang-format 自动格式化 |
| 字符规范 | 禁止 emoji，使用 ASCII 符号如 `[+]`, `[-]`, `[!]`（Windows 兼容性） |

### [MUST] 错误处理

| 错误类型 | 处理要求 |
|----------|----------|
| Drogon 异常 | 必须捕获: `catch (const DrogonDbException &e)` |
| 异步回调失败 | 必须在失败时调用 `(*sharedCb)(errorResult)` |
| 业务逻辑错误 | 使用 `PayErrorCategory` 定义错误码 |
| 日志级别 | `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR` |

### [MUST] 性能优化

| 优化项 | 要求 |
|--------|------|
| 接口选择 | 优先使用异步接口，避免阻塞 |
| 缓存策略 | 合理使用 Redis 缓存幂等性键 |
| 数据库优化 | 使用索引，避免 N+1 查询 |
| 连接池配置 | 根据并发需求调整 |
| 幂等性保护 | 所有写操作必须检查幂等性 |

---

## 四、安全规范

### [MUST] 输入验证

| 验证项 | 要求 |
|--------|------|
| 用户输入 | 所有用户输入必须验证 |
| SQL 查询 | 使用 ORM Criteria，禁止字符串拼接 |
| XSS 防护 | 使用 Drogon 内置 CSP 和模板转义 |
| API 认证 | 所有端点必须验证 API Key |

### [MUST] 支付安全

| 规范项 | 要求 |
|--------|------|
| 幂等性保护 | 所有支付和退款操作必须实现幂等性 |
| 回调验证 | 支付回调必须验证签名和来源 |
| 金额验证 | 所有金额字段必须验证格式和范围 |
| 状态机 | 支付和退款状态转换必须遵循状态机规则 |

### [MUST] 敏感数据保护

| 数据类型 | 保护要求 |
|----------|----------|
| API Key | 使用环境变量 `PAY_API_KEY` |
| 支付宝密钥 | 使用环境变量 `ALIPAY_PRIVATE_KEY` |
| 微信密钥 | 使用环境变量 `WECHAT_PAY_KEY` |
| 日志输出 | 禁止日志中输出敏感信息 (密钥, token) |

### [MUST] API 认证

| 认证方式 | 说明 |
|----------|------|
| API Key | 请求头 `X-API-Key: {key}` |
| Scope 验证 | 验证 API Key 有权限访问指定资源 |
| 速率限制 | 基于 API Key 的速率限制（可选） |

---

## 五、测试规范

### [MUST] 测试覆盖

| 测试类型 | 要求 | 工具 |
|----------|------|------|
| 单元测试 | 每个服务方法 80%+ 覆盖率 | Google Test |
| 集成测试 | API 接口级验证 | Google Test |
| 端到端测试 | 完整支付流程验证 | 前端测试 |

### [MUST] 测试数据管理

| 规范项 | 要求 |
|--------|------|
| 测试数据库 | 使用内存数据库或测试专用数据库 |
| 测试清理 | 每个测试后清理数据 |
| 测试隔离 | 测试之间相互独立 |
| Mock 使用 | 第三方支付服务使用 Mock |

---

## 六、部署规范

### [MUST] 环境配置

| 环境变量 | 用途 |
|----------|------|
| `PAY_DB_PASSWORD` | 数据库密码 |
| `PAY_REDIS_PASSWORD` | Redis 密码 |
| `PAY_API_KEY` | API 认证密钥 |
| `ALIPAY_PRIVATE_KEY` | 支付宝私钥 |
| `WECHAT_PAY_KEY` | 微信支付密钥 |

### [MUST] 监控和日志

| 监控项 | 要求 |
|--------|------|
| 应用指标 | Prometheus 指标暴露在 `/metrics` |
| 日志级别 | 生产环境使用 `LOG_INFO` 及以上 |
| 错误追踪 | 所有错误必须记录堆栈信息 |
| 性能监控 | 记录 P50, P95, P99 延迟 |

---

**文档版本**: v1.0 | **最后更新**: 2026-05-12 | **维护者**: Pay Plugin 开发团队
