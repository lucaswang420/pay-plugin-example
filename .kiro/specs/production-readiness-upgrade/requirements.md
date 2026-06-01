# Requirements Document

> 生产化改造主 Spec — production-readiness-upgrade

## Introduction

Pay Plugin 项目（基于 Drogon 的 C++ 支付插件 + Vue 3 管理前端）目前在 README 中标注为 "Production Ready v1.0.0"，但代码与配置审计显示其与真正的生产化运行之间仍有显著差距：配置中存在硬编码密码与 API Key、健康检查仅做空对象判定、可观测性体系仅落地少量指标、对外部支付网关缺乏熔断与重试、CI/CD 中存在与目标平台不一致的 AWS ECS 部署 stub、数据库缺乏迁移工具、核心服务自述"未走 TDD"且单元测试已被移除、SLO/Runbook/部分文档缺失。

本主 Spec 的定位是 **路线图 (roadmap) 与跨子 Spec 的验收门禁 (acceptance gates)**：

[+] 它定义"生产化"对本项目意味着什么（目标、范围、优先级、SLO 初值、合规约束）
[+] 它将改造工作拆分为 7 个独立可交付的子 Spec
[+] 它定义跨子 Spec 的统一交付标准（命名规范、测试门槛、观测要求、文档要求）
[-] 它不直接产出实现代码，具体实现 / 设计在子 Spec 中完成

### 子 Spec 拆分（按优先级降序，与本文档 Requirement 编号对应）

| 优先级 | 子 Spec 名称 | 对应 Requirement |
| :--- | :--- | :--- |
| P1 | secrets-and-config-hardening | Requirement 1 |
| P2 | health-and-resilience（健康部分） | Requirement 2 |
| P3 | observability-stack | Requirement 3 |
| P4 | health-and-resilience（韧性部分） | Requirement 4 |
| P5 | release-and-deployment | Requirement 5 |
| P6 | db-migration-and-backup | Requirement 6 |
| P7 | test-quality-gates | Requirement 7 |
| P8 | runbook-and-slo | Requirement 8 |

注：P2 与 P4 共用一个子 Spec `health-and-resilience`，因为健康探针与降级/熔断在实现层耦合度高。交付时内部仍按 P2（健康部分，M1）→ P4（韧性部分，M2）分阶段验收。

### 目标部署平台

- **操作系统**：Ubuntu 22.04 LTS（裸机或 VM）
- **运行时**：Docker 24+ / docker compose v2，systemd 作为容器 / 二进制守护进程
- **不在范围**：Kubernetes 编排、多机房灾备（v1 不做）、新支付渠道接入

---

## Glossary

- **SLO (Service Level Objective)**：服务等级目标，对可用率、延迟等用户可感知指标的承诺值。
- **SLI (Service Level Indicator)**：用于度量 SLO 的具体指标（如成功请求占比、P95 延迟）。
- **Error Budget**：在一个滚动窗口内允许失败的额度（= 1 - SLO）。
- **Liveness Probe**：判断"进程是否还活着"，失败触发重启。
- **Readiness Probe**：判断"进程是否已准备好接流量"，失败触发摘流。
- **Burn Rate Alert**：错误预算燃烧速率告警，根据短/长窗口同时告警避免抖动。
- **IaC (Infrastructure as Code)**：基础设施即代码，本项目使用 shell / Ansible playbook / docker compose 文件作为 IaC 载体。
- **SBOM (Software Bill of Materials)**：软件物料清单，列出镜像内的全部依赖与版本，便于漏洞追溯。
- **PBT (Property-Based Testing)**：基于性质的测试，对随机生成的输入验证不变量。
- **Runbook**：操作手册，为常见事故 / 周期任务提供可执行步骤。
- **Audit Log**：审计日志，记录"谁在何时用何身份对何资源做了何操作"。
- **Idempotency Key**：幂等键，保证同一业务操作多次提交只生效一次。
- **Hodor**：项目内置的请求限流器（Rate Limiter），基于滑动窗口算法，对 `/pay/create`、`/pay/refund` 等写端点实施速率限制。详见 `services/` 中的实现。

---

## Constraints and Assumptions

### 技术约束

- 后端遵循 `TECH_SPECS.md`：Drogon 框架优先，ORM 优先（裸 SQL 仅限文档列出的特殊情况），异步回调优先，禁用 `CoroMapper`，Lambda 必须捕获 `sharedCb` 而非裸指针，C++17，clang-format。
- 字符规范：禁止 emoji，路径与命令用 ASCII；状态/列表标记使用 `[+] / [-] / [!]`。
- 数据库：PostgreSQL 13+；缓存：Redis 6+；构建：CMake 3.15+ + Conan；测试：Google Test。
- 前端：Vue 3 + Element Plus + Pinia + Vite。

### 部署约束

- 仅支持 Ubuntu 22.04 LTS；最小 4 核 4GB 内存；推荐 8 核 8GB + SSD。
- 不依赖 Kubernetes；不依赖任何特定云厂商的托管服务（RDS、ElastiCache 等）。
- CI 使用 GitHub Actions（已存在 `ci-linux.yml`、`ci-macos.yml`、`ci-windows.yml`、`deploy.yml`）；其中 `deploy.yml` 现引用 AWS ECS，需在子 Spec `release-and-deployment` 中重写为面向 Ubuntu 22.04 的部署流水线。

### 假设

- 生产环境对 PostgreSQL / Redis 拥有独立的运维 / 备份能力（可以是同一台 VM，也可以是独立实例）。
- 生产环境拥有合法的 TLS 证书（Let's Encrypt 或其他 CA）。
- 团队拥有至少一个集中式日志/指标收集端点（自建 Prometheus + Loki 或托管服务）。

---

## Out of Scope

- [-] Kubernetes 编排与 Helm Chart
- [-] 多机房灾备 / 异地多活
- [-] 新支付渠道接入（保持当前支付宝沙箱 + 微信支付）
- [-] 重写支付/退款核心业务逻辑（仅做生产化加固，不做功能变更）
- [-] 完整的前端权限/账号体系（PayFrontend 仍作为内部管理工具，本轮只做最小安全加固）

---

## Requirements

### Requirement 1: 配置与密钥安全加固 (P1)

**User Story:** 作为安全负责人，我希望项目中所有敏感配置（数据库口令、Redis 口令、API Key、第三方支付密钥）都通过环境变量或外部密钥源注入，并在生产强制启用 HTTPS，避免任何凭据进入代码仓库或镜像层，从而降低凭据泄漏与中间人攻击风险。

**子 Spec：** `secrets-and-config-hardening`
**依赖：** 无（基础前置）

#### Acceptance Criteria

1. WHEN 仓库代码（含 `config.json`、`.env*`、Dockerfile、CI 工作流）被静态扫描时, THEN THE SYSTEM SHALL 不包含任何明文数据库口令、Redis 口令、API Key、私钥或证书内容。
2. WHEN 服务在生产配置下启动时, IF 任一必需环境变量未设置或为占位符（如 `__env_var:XXX__`、`${VAR}` 未替换、空字符串）, THEN THE SYSTEM SHALL 在 5 秒内以非零退出码失败启动并在 stderr 输出缺失变量名（不输出值）。
3. WHEN 生产配置被加载时, THE SYSTEM SHALL 强制 `listeners[0].https = true` 且 `cert/key` 路径必须存在且文件权限不超过 `0400`（私钥仅 owner 可读），否则启动失败。
4. WHEN HTTPS 监听器初始化时, THE SYSTEM SHALL 仅启用 TLS 1.2 与 TLS 1.3，禁用 SSLv2/SSLv3/TLS 1.0/TLS 1.1。
5. WHERE 当前 `custom_config.pay.api_keys` 在 `config.json` 中以明文数组形式存在, THE SYSTEM SHALL 将 API Key 来源迁移到「环境变量列表 + 可选的密钥文件加载（路径来自环境变量）」二选一，且 `config.json` 内不得保留任何真实可用的 API Key。
6. WHEN 容器镜像构建完成时, THE SYSTEM SHALL 不在镜像层内包含 `.env` 文件、`certs/` 目录下的私钥、或任何带有 `*_KEY`、`*_PASSWORD`、`*_SECRET` 命名的明文文件。
7. IF 日志输出包含 API Key、密码、私钥、access_token 或任何标记为敏感的字段, THEN THE SYSTEM SHALL 对其做掩码（首尾各保留不超过 4 字符，中间用 `***` 替换）。
8. WHERE Docker 部署被使用, THE SYSTEM SHALL 提供以 `docker secret` 或环境变量挂载的标准注入示例，并在文档中明示禁止使用 `--env` 命令行直接传入敏感值。
9. WHEN 启动检查执行时, THE SYSTEM SHALL 输出一条结构化日志记录所有"已加载但已掩码"的敏感配置键名（仅键名，不含值），便于运维核对。

---

### Requirement 2: 健康检查与就绪探针完整化 (P2)

**User Story:** 作为 SRE，我希望服务暴露独立的存活探针 (liveness) 与就绪探针 (readiness)，并且就绪探针真正反映 PostgreSQL、Redis 与第三方支付网关的可用性，避免在依赖不可用时仍接收流量造成业务面失败。

**子 Spec：** `health-and-resilience`（健康部分）
**依赖：** Req 1（健康检查需使用掩码后的配置）

#### Acceptance Criteria

1. THE SYSTEM SHALL 提供两个独立 HTTP 端点：`GET /healthz`（liveness）与 `GET /readyz`（readiness），均返回 JSON。
2. WHEN `/healthz` 被请求时, THE SYSTEM SHALL 仅校验进程内基本不变量（事件循环未死锁、关键单例已初始化），并在 100ms 内返回响应，且不进行任何外部 I/O；IF 任一进程内不变量校验失败（事件循环死锁或关键单例未初始化）, THEN THE SYSTEM SHALL 返回 HTTP 503，OTHERWISE THE SYSTEM SHALL 返回 HTTP 200。
3. WHEN `/readyz` 被请求时, THE SYSTEM SHALL 并发执行下列依赖探测并在 1 秒内返回结果：
   - 对 `dbClientMaster` 执行 `SELECT 1`（异步回调）
   - 对 `dbClientReader` 执行 `SELECT 1`（如读写分离启用）
   - 对 Redis 默认客户端执行 `PING`
   - 对支付宝沙箱 / 微信支付的"配置层"探测（仅检查证书/密钥可读，不发起网络调用）
4. WHEN `/readyz` 的任一关键依赖探测失败时, THE SYSTEM SHALL 返回 HTTP 503 与 JSON 主体 `{"status":"not_ready","failed":[...]}`，并在 `failed` 数组中列出失败依赖名称。
5. WHEN `/readyz` 连续探测失败 N 次（默认 3 次，去抖动避免瞬时抖动误摘流）, THE SYSTEM SHALL 在最后一次失败后 1 秒内将状态置为 not_ready；WHEN 单次探测失败但未达连续 N 次阈值时, THE SYSTEM SHALL 维持上一次的 ready 状态。
6. WHEN `/health` 旧端点被请求时, THE SYSTEM SHALL 保留为兼容别名（指向 `/readyz` 行为），并通过 `Deprecation` 响应头与 `Sunset` 响应头声明弃用计划，弃用窗口不少于 90 天。
7. THE SYSTEM SHALL 不对探针端点应用 `PayAuthFilter` 或 `Hodor` 限流。
8. THE SYSTEM SHALL 提供 systemd watchdog 集成示例（`WatchdogSec` + `sd_notify`）作为 Ubuntu 22.04 部署 Runbook 的一部分。
9. WHEN 依赖恢复时, THE SYSTEM SHALL 在下一次探测周期内（≤ 1 秒）将 `/readyz` 重新置为 200。

---

### Requirement 3: 可观测性体系建设 (P3)

**User Story:** 作为开发与 SRE，我希望服务输出结构化日志、贯通的 trace_id、完整的业务/基础指标，使得现有 `alerts.yml` 中引用的所有指标都真实可用，并能在跨进程链路中定位单笔交易问题。

**子 Spec：** `observability-stack`
**依赖：** Req 1（日志掩码策略需先定义）

#### Acceptance Criteria

1. THE SYSTEM SHALL 输出 JSON 结构化日志，每条日志至少包含字段：`ts`（ISO8601）、`level`、`logger`、`msg`、`trace_id`、`span_id`、`request_id`、`api_key_id`（掩码后）。`trace_id` / `span_id` 的生成与传播基于 W3C Trace Context 格式，不强制引入 OpenTelemetry SDK（可自研轻量实现），但须保证格式合规。
2. WHEN HTTP 请求进入时, IF 请求头包含 `X-Request-Id` 或 W3C `traceparent`, THEN THE SYSTEM SHALL 透传至全部下游日志、外部 HTTP 调用与异步任务；OTHERWISE THE SYSTEM SHALL 生成 UUIDv4 作为 `request_id` 并以 `traceparent` 形式生成 W3C trace。
3. THE SYSTEM SHALL 暴露 `/metrics` 端点（Prometheus 文本格式），且必须 emit 下列指标（含 alerts.yml 引用的全部指标）：
   - `http_requests_total{method,route,status}` (counter)
   - `http_request_duration_seconds_bucket{method,route}` (histogram, buckets ≤ 10ms..10s)
   - `payment_attempts_total{channel}` / `payment_success_total{channel}` / `payment_failure_total{channel,reason}` (counter)
   - `refund_attempts_total{channel}` / `refund_success_total{channel}` / `refund_failure_total{channel,reason}` (counter)
   - `db_query_duration_seconds_bucket{client,op}` (histogram)
   - `db_connections_active` / `db_connections_max` (gauge)
   - `redis_command_duration_seconds_bucket{op}` (histogram)
   - `redis_connections_active` / `redis_connections_max` (gauge)
   - `external_call_duration_seconds_bucket{provider,op,outcome}` (histogram, provider ∈ {alipay,wechat})
   - `idempotency_hit_total` / `idempotency_miss_total` (counter)
   - `reconcile_diff_total{kind}` (counter)
4. WHEN `alerts.yml` 中定义的告警规则被加载到 Prometheus 时, THE SYSTEM SHALL 保证所有引用的指标在生产负载下能产生数据点（至少 1 个非零样本），即不存在「告警引用不存在的指标」。
5. THE SYSTEM SHALL 写入审计日志（独立文件 / sink），每次成功或失败的 `/pay/create`、`/pay/refund`、`/pay/refund/query`、`/pay/query` 调用记录：`request_id`、`api_key_id`（掩码）、`scope`、`route`、`amount`（如适用）、`order_no` / `payment_no` / `refund_no`、`outcome`、`reason`、`source_ip`。
6. THE SYSTEM SHALL 实现 W3C Trace Context 出站传播：所有对支付宝 / 微信网关的 HTTP 调用必须设置 `traceparent` 请求头。
7. THE SYSTEM SHALL 在日志、指标、审计日志中对敏感字段（API Key、私钥、access_token、卡号、手机号等）应用统一掩码策略；日志体上线前需通过自动化检测（CI 步骤）。
8. WHERE 现有 `pay_auth_*` 指标已存在, THE SYSTEM SHALL 保留其向后兼容并补充 `pay_auth_*_total` 命名约定；WHERE 当前未存在任何 `pay_auth_*` 指标, THE SYSTEM SHALL 仍然新增 `pay_auth_*_total` 指标。
9. THE SYSTEM SHALL 提供日志采集到 Loki（或文件 + Promtail）的标准配置示例，作为 Ubuntu 22.04 Runbook 一部分。

---

### Requirement 4: 韧性与高可用 (P4)

**User Story:** 作为业务负责人，我希望对外部支付网关的依赖失败、慢响应或局部故障被隔离，关键写操作具备多层幂等兜底，且具备运行时降级能力，避免单一外部依赖将整个支付服务拖垮。

**子 Spec：** `health-and-resilience`（韧性部分）
**依赖：** Req 2（熔断器状态需在 /readyz 中反映）、Req 3（韧性指标需 emit 到 /metrics）

#### Acceptance Criteria

1. THE SYSTEM SHALL 对所有出站 HTTP 调用（支付宝、微信支付）实施分级超时：连接超时 ≤ 2s、读超时 ≤ 5s、整体 deadline ≤ 10s，且可通过配置覆盖。
2. WHEN 出站调用因可重试错误（5xx、超时、连接重置）失败时, THE SYSTEM SHALL 应用指数退避（基数 200ms，因子 2，最多 3 次，含 jitter ±20%）并保证幂等性；非幂等接口禁止重试。
3. THE SYSTEM SHALL 对每个外部 provider 实施熔断器：滑动窗口 30s 内错误率 ≥ 50% 且样本 ≥ 20 时进入 Open 状态，30s 后进入 Half-Open 单探测，连续 3 次成功后回到 Closed。
4. WHEN 熔断器处于 Open 状态时, THE SYSTEM SHALL 立即返回业务错误码 `provider_unavailable`，不发起出站调用，并 emit `external_call_duration_seconds_bucket{outcome="circuit_open"}`。
5. THE SYSTEM SHALL 提供运行时降级开关（feature flag），最少包括：`disable_alipay`、`disable_wechat`、`disable_reconcile_timer`、`force_idempotency_redis_only`，开关变更应在 5 秒内生效，无需重启进程。
6. WHEN `/pay/create` 写入路径被执行时, THE SYSTEM SHALL 在三层做幂等兜底：
   - 应用层：`IdempotencyService.checkAndSet`
   - DB 层：`pay_idempotency.idempotency_key` 唯一索引
   - 状态机：业务状态字段 + `UPDATE ... WHERE status = ...` 条件更新（推荐方案；行级版本号作为可选增强）
7. WHEN 同一 `idempotency_key` 在并发条件下被多次提交时, THE SYSTEM SHALL 保证最多一次副作用，重复请求返回首次成功响应（HTTP 200 + 幂等命中标记）；WHEN 非并发场景下出站调用失败时, THE SYSTEM SHALL 不返回缓存响应而是按正常错误传播路径返回失败。
8. THE SYSTEM SHALL 对 `/pay/create` 与 `/pay/refund` 维持现有 `Hodor` 限流，并补充按 `api_key_id` 维度的速率限制（默认 `api_key_id` 200/min，可覆盖）。
9. WHEN 限流被触发时, THE SYSTEM SHALL 返回 HTTP 429 + `Retry-After` 响应头并写入审计日志；未触发限流时仅按通用错误日志路径记录。
10. THE SYSTEM SHALL 不出现「Lambda 捕获裸指针」「使用 CoroMapper」等违反 `TECH_SPECS.md` 的实现方式，在 PR 审查中作为一票否决项。

---

### Requirement 5: 发布与部署改造（Ubuntu 22.04） (P5)

**User Story:** 作为运维，我希望发布流程是「构建 → 镜像扫描 → 推送制品 → Ubuntu 22.04 节点拉取部署 → 健康检查 → 失败回滚」的可重复流水线，并消除当前与目标平台不一致的 AWS ECS 残留 stub。

**子 Spec：** `release-and-deployment`
**依赖：** Req 1（镜像不得含明文密钥）、Req 2（部署脚本调用 /readyz 自检）

#### Acceptance Criteria

1. THE SYSTEM SHALL 统一对外暴露端口：`config.json` 监听端口、`Dockerfile` 的 `EXPOSE`、`docker-compose.yml` 的端口映射、健康检查脚本必须指向同一端口（建议保留 `5566`，并在文档中明示）。
2. THE SYSTEM SHALL 在 CI 流水线中集成镜像漏洞扫描（Trivy 或同等工具），且当出现 CVSS ≥ 7.0 的高危漏洞时阻断发布。
3. THE SYSTEM SHALL 生成并发布每次构建的 SBOM（CycloneDX 或 SPDX 格式），与镜像同 tag 关联存档至少 90 天。
4. WHERE `.github/workflows/deploy.yml` 当前包含 AWS ECS / RDS 调用, THE SYSTEM SHALL 重写为面向 Ubuntu 22.04 的部署流水线：
   - 通过 SSH（公钥认证 + bastion 可选）连接目标节点
   - 在节点上执行 `docker compose pull && docker compose up -d`
   - 等待 `/readyz` 在 5 分钟内返回 200，否则回滚到上一镜像 tag
5. THE SYSTEM SHALL 提供发布策略文档，区分两种场景：**单节点**采用「停机更新 + 快速回滚（≤ 5 分钟）」策略；**多节点**采用蓝绿或滚动部署策略。文档中分别说明执行步骤与回滚流程。
6. THE SYSTEM SHALL 提供 systemd unit 文件示例（`payserver.service`），支持 `Restart=on-failure`、`WatchdogSec`、`User=payuser`、`AmbientCapabilities=`（最小权限）。
7. THE SYSTEM SHALL 提供 Ubuntu 22.04 一键安装脚本（bash 或 Ansible playbook），从干净系统到运行起服务总耗时不超过 30 分钟，并在脚本结束后调用 `/readyz` 自检。
8. THE SYSTEM SHALL 在镜像中以非 root 用户运行（已在 Dockerfile 中存在 `payuser`），且所有写路径（日志、临时文件）通过卷挂载，镜像本身保持只读（`read_only: true` 兼容）。
9. WHEN 发布失败时, THE SYSTEM SHALL 自动触发回滚到上一稳定镜像 tag；回滚过程允许在执行中（即"in-progress"是合法状态），并 SHALL 在 5 分钟内完成回滚使 `/readyz` 恢复 200 状态；回滚事件须写入审计日志与告警渠道。
10. THE SYSTEM SHALL 在 CI 中加入「端口一致性」与「Dockerfile/`docker-compose.yml`/config.json 端口号 grep 一致」的静态检查步骤。

---

### Requirement 6: 数据库迁移与备份治理 (P6)

**User Story:** 作为 DBA / 后端开发，我希望数据库 schema 变更通过受控的迁移工具执行、可回滚、可审计，备份具备加密与异地副本，并定期演练恢复流程，防止数据丢失或不可用。

**子 Spec：** `db-migration-and-backup`
**依赖：** Req 1（数据库口令来自环境变量注入）

#### Acceptance Criteria

1. THE SYSTEM SHALL 自研轻量迁移命令（读取 `sql/migrations/*.sql` 按版本号排序执行），维护 `schema_migrations` 表（含 `version`、`applied_at`、`checksum`）。不引入额外语言运行时（如 sqitch/Go），避免增加构建链复杂度。
2. THE SYSTEM SHALL 为每个迁移文件提供成对的 `up`/`down` 脚本；缺失 `down` 脚本的迁移必须在 PR 中显式标注「不可回滚」并经审批。
3. WHEN 服务启动时, IF `schema_migrations.version` 落后于代码内置版本, THEN THE SYSTEM SHALL 拒绝启动并提示运维执行迁移命令；服务进程本身不得自动执行迁移（仅通过显式命令）。
4. THE SYSTEM SHALL 把现有 `sql/000_*.sql`、`sql/001_*.sql`、`sql/002_*.sql` 纳入新迁移工具的版本化管理，且不得变更已应用迁移的内容（不可变性）。
5. THE SYSTEM SHALL 启用 PostgreSQL 慢查询日志（`log_min_duration_statement = 200ms`）并在 Runbook 中记录慢查询排查步骤。
6. THE SYSTEM SHALL 对 `pay_payment`、`pay_refund`、`pay_callback`、`pay_idempotency`、`pay_ledger` 五张关键表完成索引复核，关键查询必须在 EXPLAIN 输出中走索引；任何 N+1 模式需在 PR 审查中阻断。
7. WHEN 备份脚本运行时, THE SYSTEM SHALL 对备份产物做对称加密（如 GPG 或 `openssl enc`），密钥不得与备份产物存放于同一节点。
8. THE SYSTEM SHALL 至少保留 7 天本地备份 + 30 天异地副本（异地副本可为对象存储 / 远程 SFTP）。
9. THE SYSTEM SHALL 提供季度恢复演练脚本，自动从最新备份在隔离环境恢复数据库并校验 `pay_payment` 行数与最新一笔订单 ID。
10. THE SYSTEM SHALL 在 Runbook 中记录主备切换、PITR (Point-In-Time Recovery)、误删数据 (DROP TABLE) 等三种恢复场景的步骤。

---

### Requirement 7: 测试与质量门禁 (P7)

**User Story:** 作为开发 leader，我希望 CI 流水线的覆盖率、契约、压测与 PBT 形成质量门禁，使得既有"未走 TDD"技术债被偿还，且新代码不再引入回退。

**子 Spec：** `test-quality-gates`
**依赖：** Req 4（幂等/熔断测试验证）、Req 3（覆盖率工具需配合 CI 指标采集）

#### Acceptance Criteria

1. THE SYSTEM SHALL 为 `PaymentService`、`RefundService`、`CallbackService`、`IdempotencyService`、`ReconciliationService` 五个服务补齐单元测试，移除头文件中的 "TECHNICAL DEBT NOTICE / Not implemented using TDD" 段落（或更新为"已偿还"）。
2. THE SYSTEM SHALL 在 CI 中接入覆盖率工具（`gcovr` 或 `lcov`），整体行覆盖率门槛：v1 ≥ 70%，v1.1 收敛至 ≥ 80%；低于门槛 CI 失败。
3. THE SYSTEM SHALL 引入对幂等性、金额累加不变量、状态机迁移合法性的属性测试（PBT，最低 100 例随机输入），框架建议 RapidCheck。
4. THE SYSTEM SHALL 编写支付宝沙箱、微信支付的契约测试（可使用录制回放或 wiremock），保证适配层修改时能立即检测协议偏移。
5. THE SYSTEM SHALL 编写 `/pay/create`、`/pay/refund`、`/pay/query` 的前后端契约（OpenAPI 或类似），CI 校验前端 axios 调用与契约一致。
6. THE SYSTEM SHALL 引入压测基线（k6 或 wrk2），在**本地环境**跑完整「100 RPS 持续 5 分钟」的支付创建场景，记录 P50/P95/P99 与错误率到制品（`docs/benchmarks/`）；CI nightly 跑轻量 smoke test（10 RPS 持续 1 分钟）做退化检测，当 P95 退化超过 20% 触发告警。完整压测需稳定的本地硬件环境，不依赖 CI runner。
7. THE SYSTEM SHALL 在 CI 中增加静态检查：clang-tidy（核心规则集）、include-what-you-use（可选）、`grep` 阻断 emoji 与硬编码密钥。
8. THE SYSTEM SHALL 在 CI 中阻断「使用 `CoroMapper`」「Lambda 捕获裸指针 `[this]`、`[&]` 不带说明注释」「调用旧 Plugin API `plugin.method(req, callback)`」三类违规。
9. WHEN 测试涉及外部依赖时, THE SYSTEM SHALL 使用专用测试数据库 / 独立 Redis 实例，且每个测试自清理；WHERE 测试之间需要隔离, THE SYSTEM SHALL 至少使用独立的逻辑数据库 / Redis DB index（共享物理实例可接受，但 production 物理实例严禁复用）；不允许污染生产或共享生产数据。
10. THE SYSTEM SHALL 在 PR 模板中要求作者声明：是否新增/变更对外接口、是否需要新增契约测试、是否需要新增 SLO 监控点。

---

### Requirement 8: SLO/SLI、Runbook 与文档落地 (P8)

**User Story:** 作为运维 / 业务负责人，我希望项目有可度量的 SLO/SLI、可执行的 Runbook，并且 README 中引用的所有文档都真实存在、与代码同步，避免出现「Production Ready 标语 vs 文档缺失」的不一致。

**子 Spec：** `runbook-and-slo`
**依赖：** Req 3（SLO 指标需已在 /metrics 中暴露）、Req 4（韧性相关 Runbook）

#### Acceptance Criteria

1. THE SYSTEM SHALL 在 `docs/slo.md` 中定义并维护以下 SLO（**建议初值，需运营 review**）：
   - 服务可用率：30 天滚动 ≥ 99.9%
   - 关键支付路径（`/pay/create`、`/pay/refund`、`/pay/query`、`/pay/refund/query`）：P95 ≤ 200ms、P99 ≤ 500ms
   - 支付成功率（业务）：≥ 99%（5 分钟滚动）
   - 退款成功率（业务）：≥ 99%（5 分钟滚动）
   - 回调处理延迟：P95 ≤ 1s 完成入库
2. THE SYSTEM SHALL 配置错误预算燃烧告警（multi-window multi-burn-rate）：
   - 1h 窗口 14.4× 速率（critical）
   - 6h 窗口 6× 速率（warning）
   - 24h 窗口 3× 速率（info）
3. THE SYSTEM SHALL 提供下列 Runbook（位于 `docs/runbook/`），每篇含「触发条件 / 影响评估 / 处置步骤 / 升级路径 / 验证恢复 / 复盘模板」：
   - `incident-payment-success-rate-drop.md`
   - `incident-provider-circuit-open.md`（支付宝/微信熔断）
   - `incident-db-pool-exhausted.md`
   - `incident-redis-down.md`
   - `incident-secret-leak.md`（密钥泄漏应急）
   - `task-reconcile-diff.md`（对账差异处理）
   - `task-db-failover.md`（主备切换）
   - `task-rotate-api-key.md`（API Key 轮换）
4. THE SYSTEM SHALL 为对账差异（`reconcile_diff_total > 0`）提供告警与人工介入入口（前端管理页面或 CLI 命令二选一），允许运营标注「已核销 / 升级 / 忽略」并写入审计。
5. WHEN README 列出文档链接时, THE SYSTEM SHALL 保证每个链接指向真实存在的文档；CI 中加入 markdown 链接检查（lychee 或同等工具），IF 断链则阻断构建；THE SYSTEM SHALL 同时检查文档最后修改时间，IF 超过 90 天未更新则在 CI 中输出警告（不阻断构建），并在 `docs/production-readiness-checklist.md` 中标记为「待 review」。
6. THE SYSTEM SHALL 在 README 中替换"Production Ready"等无依据自我评级，改为「合规清单」或「生产化检查清单完成度」，每项对应本 spec 的 Requirement 编号。
7. THE SYSTEM SHALL 在 `docs/operations_manual.md` 中合并 / 重写当前 README 引用但缺失的文档：deployment_guide、monitoring_setup、security_checklist、api_configuration_guide、e2e_testing_guide。
8. THE SYSTEM SHALL 维护一份「生产化改造完成度看板」（可放 `docs/production-readiness-checklist.md`），列出全部 P1–P8 维度的子项与状态（done / in-progress / not-started）。

---

## Acceptance Gates（跨子 Spec 验收门禁）

主 Spec 视为达成生产化目标的判据：以下门禁全部通过即可在 README 中真实声明 "Production Ready"。

| 门禁 | 判据 | 关联 Requirement |
| :--- | :--- | :--- |
| G1 配置无明文 | 仓库静态扫描 0 命中 | Req 1 |
| G2 健康探针 | `/healthz` + `/readyz` + systemd watchdog 端到端通过 | Req 2 |
| G3 指标对齐 | `alerts.yml` 引用的全部指标在生产负载下产生数据点 | Req 3 |
| G4 韧性达标 | 支付宝/微信熔断器测试通过 + 三层幂等兜底测试通过 | Req 4 |
| G5 部署一致 | 端口一致性 + 镜像扫描 + SBOM + Ubuntu 22.04 一键脚本演练成功 | Req 5 |
| G6 迁移可控 | 迁移工具上线 + 季度恢复演练通过 | Req 6 |
| G7 质量门禁 | 行覆盖率 ≥ 70%（v1）+ PBT + 契约 + 压测基线在 CI nightly 通过 | Req 7 |
| G8 文档闭环 | SLO + Runbook + README 链接检查 0 失败 | Req 8 |

---

## Milestones（建议时间线）

| 里程碑 | 目标 | 关联门禁 |
| :--- | :--- | :--- |
| M1（2 周） | 配置安全 + 健康探针 + Req 7 静态检查（AC 7/8 提前落地） | G1 + G2 |
| M2（4 周） | 可观测性 + 韧性 | G3 + G4 |
| M3（3 周） | 部署 + 数据库 | G5 + G6 |
| M4（3 周） | 测试 + 文档 / SLO（Req 7 其余 AC） | G7 + G8 |
| **合计** | **12 周** | **G1–G8 全通过** |

时间线为建议初值，实际节奏需结合团队容量调整。

注：Req 7 的静态检查（AC 7 clang-tidy、AC 8 违规模式阻断）在 M1 落地，是因为 Req 4 AC 10 要求 PR 审查一票否决 TECH_SPECS 违规，需先有自动化检查工具支撑。
