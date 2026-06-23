# M1 Production Readiness Design

> 配置安全加固 + 健康探针完整化 + CI 静态检查

## Scope

M1 覆盖 `requirements.md` 中的：
- Req 1: 配置与密钥安全加固 (P1) — 除 TLS/HTTPS（留到 M3 部署阶段）
- Req 2: 健康检查与就绪探针完整化 (P2)
- Req 7 AC 7/8: CI 静态检查工具

M1 不包含：TLS 配置（Req 1 AC 3/4）、镜像安全扫描（Req 1 AC 6/8）、审计日志（Req 3 AC 5）。

## WF-1: 配置安全加固

### 目标

消除 config.json 中的硬编码凭据，统一使用 `ConfigLoader` 的 `__env_var:XXX__` 机制。

### 当前状态

- `config.json` 含硬编码：DB 密码 `123456`、Redis 密码 `123456`、API Key `test_key_123456`
- 微信/支付宝配置已使用 `__env_var:XXX__` 占位符（由 `ConfigLoader` 处理）
- `ConfigLoader` 在 `PayPlugin.cc` 中调用，仅处理插件配置，不处理 `db_clients`/`redis_clients` 等顶层配置
- Drogon 支持 `loadConfigJson(const Json::Value &)` 方法，可在 `main.cc` 中先处理 JSON 再加载

### 方案

**main.cc 改造**：

```
.env 加载 → 读取 config.json → ConfigLoader 替换占位符 → StartupValidator 校验 → loadConfigJson
```

替代当前的 `loadConfigFile("./config.json")`。

**config.json 改造**：

| 字段 | 当前值 | 改为 |
|------|--------|------|
| `db_clients[0].passwd` | `"123456"` | `"__env_var:PAY_DB_PASSWORD__"` |
| `redis_clients[0].passwd` | `"123456"` | `"__env_var:PAY_REDIS_PASSWORD__"` |
| `custom_config.pay.api_keys` | `["test_key_123456"]` | `["__env_var:PAY_API_KEY__"]` |
| `custom_config.pay.api_key_scopes` | 含 `test_key_123456` | 保持不变（仅开发环境使用）；生产环境由 StartupValidator 从 `PAY_API_KEY` 环境变量读取 key 值后，以该值为 key 注入 scopes（默认 `["read"]`） |

**api_key_scopes 处理逻辑**：
- config.json 中 `api_key_scopes` 保留开发环境的 key 值，不影响生产
- StartupValidator 在校验通过后，将 `PAY_API_KEY` 环境变量的实际值注册到 Drogon custom_config 中，scopes 使用 `api_key_default_scopes`
- 多 API Key 场景：`PAY_API_KEY` 支持逗号分隔（`key1,key2`），StartupValidator 按分隔符拆分后逐一注册

**新增 `utils/StartupValidator.{h,cc}`**：

- 校验必需环境变量（`PAY_DB_PASSWORD`、`PAY_REDIS_PASSWORD`、`PAY_API_KEY`）非空、非占位符模式（`__env_var:`、`${VAR}`）
- 校验通过后，输出结构化日志列出已加载的敏感配置键名（掩码后值，首尾各 4 字符）
- 校验失败：`LOG_ERROR` 输出缺失变量名 + `exit(1)`

**ConfigLoader 增强**：

- `loadConfig()` 中当环境变量为空时，返回空字符串（当前行为）。StartupValidator 在之后做校验。
- 新增 `maskSensitive(const std::string& value)` 静态方法：首尾各保留 4 字符，中间用 `***` 替换。

**.env.production.example 更新**：

补充 `PAY_DB_PASSWORD`、`PAY_REDIS_PASSWORD`、`PAY_API_KEY` 变量。

**config.json 开发环境兼容**：

开发环境通过 `.env` 文件提供 `PAY_DB_PASSWORD=123456` 等值，config.json 不再含明文。

**环境变量命名规范**：

以下为 M1 后的唯一规范命名，所有 `.env.*.example` 文件和文档将统一更新：

| 环境变量 | 用途 | 当前别名（将废弃） |
|----------|------|-------------------|
| `PAY_DB_PASSWORD` | 数据库密码 | `DB_PASSWORD`, `POSTGRES_PASSWORD` |
| `PAY_REDIS_PASSWORD` | Redis 密码 | `REDIS_PASSWORD` |
| `PAY_API_KEY` | API 密钥（逗号分隔支持多 key） | 无 |
| `WECHAT_PAY_*` | 微信支付配置（已有，保持不变） | — |
| `ALIPAY_SANDBOX_*` | 支付宝沙箱配置（已有，保持不变） | — |

`.env.production.example` 将完整重写（非补充），统一使用上述命名，并移除旧的 `DB_PASSWORD`、`REDIS_PASSWORD` 等变量名。

### 不做的

- Req 1 AC 3/4（TLS 强制 + TLS 版本限制）→ M3
- Req 1 AC 6/8（镜像安全 + Docker secret）→ M3
- Req 1 AC 7（日志掩码过滤器）→ 部分实现（仅 StartupValidator 的掩码输出），完整日志掩码留到 M2
- Req 1 AC 9（启动时掩码审计日志）→ 由 StartupValidator 覆盖

---

## WF-2: 健康探针完整化

### 目标

提供独立的 `/healthz`（liveness）和 `/readyz`（readiness）端点，替换当前仅做空对象判定的 `/health`。

### 当前状态

- `HealthCheckController` 提供 `GET /health`，仅检查 DB/Redis 客户端指针是否为空（不做实际查询）
- Hodor 限流匹配 `^/api/.*`，健康端点 `/health` 不在匹配范围内（已天然排除）
- PayAuthFilter 通过控制器路由声明应用，HealthCheckController 未声明 filter

### 方案

**扩展 HealthCheckController**：

新增两个端点，保留 `/health` 为兼容别名：

| 端点 | 行为 | 超时 | 依赖检查 |
|------|------|------|----------|
| `/healthz` | 进程存活检查 | 100ms | 事件循环 + 框架运行状态，无外部 I/O |
| `/readyz` | 依赖可用性检查 | 1s | DB `SELECT 1` + Redis `PING` + PayPlugin 初始化 + 支付配置可读 |
| `/health` | 兼容别名 → 转发到 `/readyz` 行为 | 1s | 同 `/readyz`，附加 `Deprecation` + `Sunset` 头 |

**`/healthz` 实现**：

- 检查 `drogon::app().getLoop()` 非空（事件循环存活）
- 检查 `drogon::app().isRunning()` 为 true（框架处于运行状态）
- 不调用 `getPlugin<PayPlugin>()`：该 API 内部缓存结果，启动阶段调用会永久缓存 nullptr，属于就绪信号而非存活信号
- 返回 `{"status":"alive"}` + HTTP 200，或 `{"status":"dead"}` + HTTP 503

**`/readyz` 实现**：

- 并发执行探测（使用回调计数器，全部完成或超时后返回）：
  - `dbClient->execSqlAsync("SELECT 1", ...)` （当前仅一个 DB 客户端 "default"，如后续启用读写分离则增加 reader 探测）
  - `redisClient->execCommandAsync("PING", ...)`（如 Redis 未配置则跳过，不计入失败）
  - `drogon::app().getPlugin<PayPlugin>()` 非空（PayPlugin 已初始化）
  - 支付配置文件可读性检查（`std::ifstream` 打开证书/密钥路径）
- 任一关键依赖失败 → HTTP 503 + `{"status":"not_ready","failed":["db","redis",...]}`
- 全部成功 → HTTP 200 + `{"status":"ready"}`

**去抖动逻辑**：

- 使用 `std::atomic<int>` 维护连续失败计数器（默认阈值 N=3），`std::atomic<bool>` 维护 ready 状态
- 使用原子变量而非 mutex，避免阻塞 Drogon I/O 线程
- 连续失败达 N 次后标记 not_ready，单次失败不立即摘流
- 恢复时立即重置计数器，下一次探测成功即回到 ready

**`/health` 兼容**：

- 响应头附加 `Deprecation: true` 和 `Sunset: <发布日期+90天>`（弃用窗口不少于 90 天，具体日期在发布时确定）
- 行为与 `/readyz` 一致

**排除限流和认证**：

- `/healthz`、`/readyz` 路径不匹配 Hodor 的 `^/api/.*` 和 `^/pay/.*` 模式（已天然排除）
- HealthCheckController 不声明 PayAuthFilter（当前已是如此）

**Docker HEALTHCHECK 更新**：

- Dockerfile: `HEALTHCHECK CMD curl -f http://localhost:5566/healthz || exit 1`
- docker-compose.yml: 同上

**systemd watchdog 集成**：

- 在 Runbook 中提供 `payserver.service` 示例文件，配置 `WatchdogSec=30`
- 不在代码中实现 `sd_notify`（C++ 项目引入 libsystemd 依赖过重），改为文档说明

### 不做的

- Req 2 AC 8（代码级 sd_notify 集成）→ 仅提供文档示例
- Req 4（韧性/熔断器）→ M2

---

## WF-3: CI 静态检查

### 目标

在 `ci-linux.yml` 中新增静态检查步骤，阻断 TECH_SPECS 违规代码合入。

### 当前状态

- `ci-linux.yml` 执行 Linux 构建 + 测试，无静态分析
- 5 个服务头文件含 TECHNICAL DEBT NOTICE

### 方案

**在 `ci-linux.yml` 构建步骤之前新增 `static-analysis` job**：

| 检查项 | 实现方式 | 失败行为 |
|--------|----------|----------|
| clang-tidy | `clang-tidy` 核心规则集（bugprone-*, modernize-*, performance-*） | 阻断 |
| Emoji 检测 | `grep -Pn '[\x{1F000}-\x{1FFFF}]' PayBackend/**/*.{h,cc}` | 阻断 |
| 硬编码密钥 | 见下方"硬编码密钥检测策略" | 阻断 |
| CoroMapper 使用 | `grep -rn 'CoroMapper' PayBackend/` | 阻断 |
| 裸指针 Lambda 捕获 | `grep -rn '\[this\]\|\[&\]' PayBackend/` 排除有说明注释的行 | 警告（M1 软阻断） |
| 旧 Plugin API | `grep -rn 'plugin.*->.*method.*req.*callback' PayBackend/test/` | 阻断 |

**硬编码密钥检测策略**：

M1 阶段使用 grep 组合模式匹配常见弱密码和占位符遗漏：

```bash
grep -rn -E '(passwd|password|secret)\s*[:=]\s*"[^_${}][^"]{2,}"' PayBackend/ \
  --include='*.h' --include='*.cc' --include='*.json' \
  | grep -v '__env_var' | grep -v 'test_key_' | grep -v '.example'
```

此模式检测 `passwd: "xxx"` 形式中非占位符（排除 `__env_var`）的硬编码值。后续 M3 可替换为 `gitleaks` 或 `trufflehog` 等专业工具。

**clang-tidy 配置**：

- 新增 `.clang-tidy` 文件在项目根目录，配置规则集
- CI 中仅对修改的文件运行（`git diff --name-only` 获取变更文件）

**裸指针 Lambda 检测策略**：

- 检测 `[this]` 和 `[&]` 捕获
- 排除含有 `// safe:` 或 `// NOLINT` 注释的行
- 这是软阻断：CI 输出警告但不阻断（避免误报过多），后续收紧为硬阻断

### 不做的

- include-what-you-use（可选，M1 不引入）
- 完整的 clang-tidy 修复（仅检测，不自动修复）

---

## 测试计划

### WF-1 测试

| 测试类型 | 测试用例 | 文件 |
|----------|----------|------|
| 单元测试 | `StartupValidator::validate` 缺失必需变量时 exit(1) | `test/StartupValidatorTest.cc` |
| 单元测试 | `StartupValidator::validate` 变量为占位符模式时 exit(1) | 同上 |
| 单元测试 | `StartupValidator::validate` 正常变量通过 | 同上 |
| 单元测试 | `ConfigLoader::maskSensitive` 掩码首尾各 4 字符 | `test/ConfigLoaderTest.cc` |
| 单元测试 | `ConfigLoader::maskSensitive` 短字符串（< 8 字符）全掩码 | 同上 |
| 集成测试 | 服务启动时 config.json 无明文密码，依赖 `.env` 注入 | `test/StartupIntegrationTest.cc` |

### WF-2 测试

| 测试类型 | 测试用例 | 文件 |
|----------|----------|------|
| 集成测试 | `GET /healthz` 返回 200 + `{"status":"alive"}` | `test/HealthCheckTest.cc` |
| 集成测试 | `GET /readyz` 依赖正常时返回 200 | 同上 |
| 集成测试 | `GET /readyz` DB 不可用时返回 503 + failed 数组含 "db" | 同上 |
| 集成测试 | `GET /health` 返回 Deprecation 响应头 | 同上 |
| 单元测试 | 去抖动：连续 N 次失败后状态变为 not_ready | 同上 |
| 单元测试 | 去抖动：恢复后立即重置计数器 | 同上 |

### WF-3 测试

| 测试类型 | 测试用例 | 文件 |
|----------|----------|------|
| CI 验证 | 提交含 `123456` 密码的文件 → CI 阻断 | `.github/workflows/ci-linux.yml` |
| CI 验证 | 提交含 CoroMapper 的代码 → CI 阻断 | 同上 |
| CI 验证 | 提交含 emoji 的代码 → CI 阻断 | 同上 |

---

## 依赖关系

```
WF-1 (配置安全) ──→ WF-2 (健康探针)
                        ↑
WF-3 (CI检查) ──────────┘（无直接依赖，可并行）
```

WF-2 的 `/readyz` 依赖 WF-1 的环境变量注入（DB 密码等需正确配置才能做 `SELECT 1`）。
WF-3 与 WF-1/WF-2 无依赖，可并行开发。

## 验收标准

| 门禁 | 判据 |
|------|------|
| G1 配置无明文 | `grep -rn '123456' PayBackend/config.json` 无命中 |
| G1 启动校验 | 未设置 `PAY_DB_PASSWORD` 时服务 5 秒内退出 + stderr 输出缺失变量名 |
| G2 健康探针 | `/healthz` 返回 200 + `/readyz` 返回 200（依赖正常时）或 503（依赖异常时） |
| G2 兼容性 | `/health` 返回 Deprecation 头 |
| G7 静态检查 | CI 中 clang-tidy + grep 检查通过 |

## 文件变更清单

| 操作 | 文件 |
|------|------|
| 修改 | `PayBackend/main.cc` |
| 修改 | `PayBackend/config.json` |
| 修改 | `PayBackend/controllers/HealthCheckController.h` |
| 修改 | `PayBackend/controllers/HealthCheckController.cc` |
| 修改 | `PayBackend/Dockerfile` |
| 修改 | `PayBackend/docker-compose.yml` |
| 修改 | `PayBackend/.env.production.example`（完整重写，统一变量命名） |
| 修改 | `PayBackend/.env.example`（同步更新变量命名） |
| 修改 | `PayBackend/.env.development.example`（同步更新变量命名） |
| 修改 | `.github/workflows/ci-linux.yml` |
| 新增 | `PayBackend/utils/StartupValidator.h` |
| 新增 | `PayBackend/utils/StartupValidator.cc` |
| 新增 | `.clang-tidy` |
