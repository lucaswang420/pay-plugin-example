# M1 Production Readiness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove hardcoded credentials from config.json, add proper health probes, and enforce CI static analysis.

**Architecture:** Move config loading from Drogon's `loadConfigFile` to manual JSON processing via existing `ConfigLoader`, add `StartupValidator` for env var validation, extend `HealthCheckController` with `/healthz` and `/readyz`, and add a `static-analysis` job to `ci-linux.yml`.

**Tech Stack:** Drogon C++17 | Google Test (`DROGON_TEST`) | GitHub Actions

**Design Spec:** `docs/superpowers/specs/2026-05-28-m1-production-readiness-design.md`

---

## File Structure

| Action | File | Responsibility |
|--------|------|----------------|
| Create | `PayBackend/utils/StartupValidator.h` | Env var validation + startup audit log |
| Create | `PayBackend/utils/StartupValidator.cc` | Implementation |
| Create | `PayBackend/test/StartupValidatorTest.cc` | Unit tests |
| Create | `PayBackend/test/HealthProbeTest.cc` | Integration tests for /healthz, /readyz |
| Create | `.clang-tidy` | clang-tidy rule configuration |
| Modify | `PayBackend/utils/ConfigLoader.h` | Add `maskSensitive()` method |
| Modify | `PayBackend/utils/ConfigLoader.cc` | Implement `maskSensitive()` |
| Modify | `PayBackend/test/PayUtilsTest.cc` | Add `maskSensitive` tests (or new file) |
| Modify | `PayBackend/main.cc` | Switch from `loadConfigFile` to `loadConfigJson` |
| Modify | `PayBackend/config.json` | Replace hardcoded values with `__env_var:XXX__` |
| Modify | `PayBackend/controllers/HealthCheckController.h` | Add /healthz, /readyz endpoints |
| Modify | `PayBackend/controllers/HealthCheckController.cc` | Implement liveness + readiness probes |
| Modify | `PayBackend/Dockerfile` | Update HEALTHCHECK + EXPOSE to 5566 |
| Modify | `PayBackend/docker-compose.yml` | Update healthcheck + port mapping |
| Modify | `PayBackend/.env.example` | Sync variable names |
| Modify | `PayBackend/.env.development.example` | Sync variable names |
| Modify | `PayBackend/.env.production.example` | Rewrite with canonical names |
| Modify | `PayBackend/test/CMakeLists.txt` | Add new test source files |
| Modify | `.github/workflows/ci-linux.yml` | Add static-analysis job + env vars |
| Modify | `.github/workflows/deploy.yml` | Add PAY_* env vars |

---

## Task 1: Add `maskSensitive` to ConfigLoader

**Files:**
- Modify: `PayBackend/utils/ConfigLoader.h`
- Modify: `PayBackend/utils/ConfigLoader.cc`
- Create: `PayBackend/test/ConfigLoaderTest.cc`
- Modify: `PayBackend/test/CMakeLists.txt`

- [ ] **Step 1: Add test file and maskSensitive tests**

Create `PayBackend/test/ConfigLoaderTest.cc`:

```cpp
#include <drogon/drogon_test.h>
#include "../utils/ConfigLoader.h"

DROGON_TEST(ConfigLoader_MaskSensitive_Normal)
{
    // 12 chars -> first 4 + *** + last 4
    std::string result = ConfigLoader::maskSensitive("abcdefghijkl");
    CHECK(result == "abcd***ijkl");
}

DROGON_TEST(ConfigLoader_MaskSensitive_Short)
{
    // Less than 8 chars -> full mask
    std::string result = ConfigLoader::maskSensitive("abc");
    CHECK(result == "***");
}

DROGON_TEST(ConfigLoader_MaskSensitive_Empty)
{
    std::string result = ConfigLoader::maskSensitive("");
    CHECK(result == "***");
}

DROGON_TEST(ConfigLoader_MaskSensitive_ExactBoundary)
{
    // Exactly 8 chars -> 4 + *** + 4 would need 11 min, so 8 -> full mask
    std::string result = ConfigLoader::maskSensitive("12345678");
    CHECK(result == "***");
}

DROGON_TEST(ConfigLoader_MaskSensitive_LongKey)
{
    std::string result = ConfigLoader::maskSensitive("sk_live_abcdef123456");
    CHECK(result == "sk_l***3456");
}
```

- [ ] **Step 2: Add maskSensitive declaration to ConfigLoader.h**

Add to the `public` section of `ConfigLoader`:

```cpp
    /**
     * @brief Mask sensitive value for logging (first 4 + *** + last 4)
     */
    static std::string maskSensitive(const std::string &value);
```

- [ ] **Step 3: Implement maskSensitive in ConfigLoader.cc**

Add at the end of `ConfigLoader.cc`:

```cpp
std::string ConfigLoader::maskSensitive(const std::string &value)
{
    if (value.size() < 8)
    {
        return "***";
    }
    return value.substr(0, 4) + "***" + value.substr(value.size() - 4);
}
```

- [ ] **Step 4: Add test file to test CMakeLists.txt**

Add `ConfigLoaderTest.cc` to the `TEST_SRC` list in `PayBackend/test/CMakeLists.txt`:

```cmake
set(TEST_SRC
    ConfigLoaderTest.cc
    CreatePaymentIntegrationTest.cc  # [PASS] Updated to new API
    # ... rest unchanged
)
```

- [ ] **Step 5: Build and run tests**

Run from `PayBackend/`:
```bash
scripts\build.bat
cd build\Release
test_payplugin.exe --gtest_filter=*ConfigLoader*
```
Expected: All 5 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add PayBackend/utils/ConfigLoader.h PayBackend/utils/ConfigLoader.cc PayBackend/test/ConfigLoaderTest.cc PayBackend/test/CMakeLists.txt
git commit -m "feat: add ConfigLoader::maskSensitive for sensitive value masking"
```

---

## Task 2: Create StartupValidator

**Files:**
- Create: `PayBackend/utils/StartupValidator.h`
- Create: `PayBackend/utils/StartupValidator.cc`
- Create: `PayBackend/test/StartupValidatorTest.cc`
- Modify: `PayBackend/test/CMakeLists.txt`

- [ ] **Step 1: Write failing tests**

Create `PayBackend/test/StartupValidatorTest.cc`:

```cpp
#include <drogon/drogon_test.h>
#include "../utils/StartupValidator.h"
#include <cstdlib>

DROGON_TEST(StartupValidator_IsPlaceholder_EnvVarSyntax)
{
    CHECK(StartupValidator::isPlaceholder("__env_var:PAY_DB_PASSWORD__"));
    CHECK(StartupValidator::isPlaceholder("__env_var:FOO__"));
}

DROGON_TEST(StartupValidator_IsPlaceholder_ShellSyntax)
{
    CHECK(StartupValidator::isPlaceholder("${PAY_DB_PASSWORD}"));
    CHECK(StartupValidator::isPlaceholder("${FOO}"));
}

DROGON_TEST(StartupValidator_IsPlaceholder_Empty)
{
    CHECK(StartupValidator::isPlaceholder(""));
}

DROGON_TEST(StartupValidator_IsPlaceholder_NormalValue)
{
    CHECK(!StartupValidator::isPlaceholder("actual_password_123"));
    CHECK(!StartupValidator::isPlaceholder("123456"));
}

DROGON_TEST(StartupValidator_ValidateRequired_MissingVar)
{
    // Ensure the var does not exist
#ifdef _WIN32
    _putenv_s("PAY_TEST_MISSING_VAR", "");
#else
    unsetenv("PAY_TEST_MISSING_VAR");
#endif

    auto result = StartupValidator::validateRequired({"PAY_TEST_MISSING_VAR"});
    CHECK(!result.ok);
    CHECK(result.missingVars.size() == 1);
    CHECK(result.missingVars[0] == "PAY_TEST_MISSING_VAR");
}

DROGON_TEST(StartupValidator_ValidateRequired_PresentVar)
{
#ifdef _WIN32
    _putenv_s("PAY_TEST_PRESENT_VAR", "some_value");
#else
    setenv("PAY_TEST_PRESENT_VAR", "some_value", 1);
#endif

    auto result = StartupValidator::validateRequired({"PAY_TEST_PRESENT_VAR"});
    CHECK(result.ok);
    CHECK(result.missingVars.empty());
}

DROGON_TEST(StartupValidator_ValidateRequired_PlaceholderVar)
{
#ifdef _WIN32
    _putenv_s("PAY_TEST_PLACEHOLDER_VAR", "__env_var:STILL_PLACEHOLDER__");
#else
    setenv("PAY_TEST_PLACEHOLDER_VAR", "__env_var:STILL_PLACEHOLDER__", 1);
#endif

    auto result = StartupValidator::validateRequired({"PAY_TEST_PLACEHOLDER_VAR"});
    CHECK(!result.ok);
    CHECK(result.missingVars.size() == 1);
}
```

- [ ] **Step 2: Create StartupValidator.h**

Create `PayBackend/utils/StartupValidator.h`:

```cpp
#pragma once

#include <string>
#include <vector>

struct ValidationResult
{
    bool ok = false;
    std::vector<std::string> missingVars;
};

class StartupValidator
{
  public:
    /**
     * @brief Check if a value is an unresolved placeholder
     */
    static bool isPlaceholder(const std::string &value);

    /**
     * @brief Validate that required env vars are set and not placeholders
     * @return ValidationResult with ok=true if all valid, or list of missing vars
     */
    static ValidationResult validateRequired(
      const std::vector<std::string> &requiredVars
    );

    /**
     * @brief Full startup validation: check vars, log audit, exit on failure
     *
     * Called from main() before loadConfigJson. Calls exit(1) on failure.
     */
    static void validate(const std::vector<std::string> &requiredVars);
};
```

- [ ] **Step 3: Create StartupValidator.cc**

Create `PayBackend/utils/StartupValidator.cc`:

```cpp
#include "StartupValidator.h"
#include "ConfigLoader.h"
#include <drogon/drogon.h>
#include <cstdlib>

bool StartupValidator::isPlaceholder(const std::string &value)
{
    if (value.empty())
    {
        return true;
    }
    if (value.find("__env_var") == 0)
    {
        return true;
    }
    if (value.size() >= 3 && value.front() == '$' && value[1] == '{' &&
        value.back() == '}')
    {
        return true;
    }
    return false;
}

ValidationResult StartupValidator::validateRequired(
  const std::vector<std::string> &requiredVars
)
{
    ValidationResult result;
    result.ok = true;

    for (const auto &varName : requiredVars)
    {
        const char *envValue = std::getenv(varName.c_str());
        if (!envValue || isPlaceholder(std::string(envValue)))
        {
            result.ok = false;
            result.missingVars.push_back(varName);
        }
    }

    return result;
}

void StartupValidator::validate(
  const std::vector<std::string> &requiredVars
)
{
    auto result = validateRequired(requiredVars);

    if (!result.ok)
    {
        for (const auto &varName : result.missingVars)
        {
            LOG_ERROR << "Missing or invalid required environment variable: "
                      << varName;
        }
        LOG_ERROR << "Startup validation failed. Exiting.";
        exit(1);
    }

    // Audit log: list loaded sensitive config keys (masked values)
    LOG_INFO << "Startup validation passed. Loaded sensitive config:";
    for (const auto &varName : requiredVars)
    {
        const char *envValue = std::getenv(varName.c_str());
        if (envValue)
        {
            LOG_INFO << "  " << varName << " = "
                     << ConfigLoader::maskSensitive(std::string(envValue));
        }
    }
}
```

- [ ] **Step 4: Add StartupValidatorTest.cc to test CMakeLists.txt**

Add `StartupValidatorTest.cc` to `TEST_SRC`:

```cmake
set(TEST_SRC
    ConfigLoaderTest.cc
    CreatePaymentIntegrationTest.cc
    StartupValidatorTest.cc
    # ... rest unchanged
)
```

Also add `../utils/StartupValidator.cc` to the executable sources (after `../utils/ConfigLoader.cc`):

```cmake
    ../utils/ConfigLoader.cc
    ../utils/StartupValidator.cc
```

- [ ] **Step 5: Build and run tests**

```bash
scripts\build.bat
cd build\Release
test_payplugin.exe --gtest_filter=*StartupValidator*
```
Expected: All 7 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add PayBackend/utils/StartupValidator.h PayBackend/utils/StartupValidator.cc PayBackend/test/StartupValidatorTest.cc PayBackend/test/CMakeLists.txt
git commit -m "feat: add StartupValidator for env var validation at startup"
```

---

## Task 3: Update config.json and main.cc

**Files:**
- Modify: `PayBackend/config.json`
- Modify: `PayBackend/main.cc`
- Modify: `PayBackend/plugins/PayPlugin.cc`

- [ ] **Step 1: Update config.json — replace hardcoded credentials**

Replace the three hardcoded values:

```json
"db_clients": [
    {
        "name": "default",
        "rdbms": "postgresql",
        "host": "127.0.0.1",
        "port": 5432,
        "dbname": "pay_test",
        "user": "test",
        "passwd": "__env_var:PAY_DB_PASSWORD__",
```

```json
"redis_clients": [
    {
        "name": "default",
        "host": "127.0.0.1",
        "port": 6379,
        "username": "",
        "passwd": "__env_var:PAY_REDIS_PASSWORD__",
```

```json
"custom_config": {
    "pay": {
        "api_keys": [
            "__env_var:PAY_API_KEY__"
        ],
```

- [ ] **Step 2: Update main.cc — use loadConfigJson**

Replace the entire `main.cc` content:

```cpp
#include <drogon/drogon.h>
#include "utils/ConfigLoader.h"
#include "utils/StartupValidator.h"
#include <fstream>
#include <json/json.h>
#include <string>

using namespace drogon;

void setupCors()
{
    auto isAllowed = [](const std::string &origin) -> bool {
        if (origin.empty())
            return false;

        const auto &customConfig = drogon::app().getCustomConfig();
        const auto &allowOrigins = customConfig["cors"]["allow_origins"];

        if (allowOrigins.isArray())
        {
            for (const auto &allowed : allowOrigins)
            {
                if (allowed.asString() == origin)
                    return true;
            }
        }
        return false;
    };

    drogon::app().registerSyncAdvice(
      [isAllowed](const drogon::HttpRequestPtr &req) -> drogon::HttpResponsePtr {
          if (req->method() == drogon::HttpMethod::Options)
          {
              const auto &origin = req->getHeader("Origin");
              if (isAllowed(origin))
              {
                  auto resp = drogon::HttpResponse::newHttpResponse();
                  resp->addHeader("Access-Control-Allow-Origin", origin);

                  const auto &requestMethod =
                    req->getHeader("Access-Control-Request-Method");
                  if (!requestMethod.empty())
                  {
                      resp->addHeader(
                        "Access-Control-Allow-Methods", requestMethod
                      );
                  }

                  resp->addHeader("Access-Control-Allow-Credentials", "true");

                  const auto &requestHeaders =
                    req->getHeader("Access-Control-Request-Headers");
                  if (!requestHeaders.empty())
                  {
                      resp->addHeader(
                        "Access-Control-Allow-Headers", requestHeaders
                      );
                  }
                  return resp;
              }
          }
          return {};
      }
    );

    drogon::app().registerPostHandlingAdvice(
      [isAllowed](const drogon::HttpRequestPtr &req,
                  const drogon::HttpResponsePtr &resp) {
          const auto &origin = req->getHeader("Origin");
          if (isAllowed(origin))
          {
              resp->addHeader("Access-Control-Allow-Origin", origin);
              resp->addHeader(
                "Access-Control-Allow-Methods", "GET, POST, OPTIONS"
              );
              resp->addHeader(
                "Access-Control-Allow-Headers",
                "Content-Type, Authorization"
              );
              resp->addHeader("Access-Control-Allow-Credentials", "true");
          }
      }
    );
}

int main()
{
    // 1. Load .env file into process environment
    ConfigLoader::loadEnvFile(".env");

    // 2. Validate required environment variables
    StartupValidator::validate(
      {"PAY_DB_PASSWORD", "PAY_REDIS_PASSWORD", "PAY_API_KEY"}
    );

    // 3. Read config.json and replace __env_var:XXX__ placeholders
    std::ifstream configFile("./config.json");
    if (!configFile.is_open())
    {
        LOG_ERROR << "Failed to open config.json";
        return 1;
    }
    Json::Value config;
    Json::CharReaderBuilder builder;
    std::string errors;
    if (!Json::parseFromStream(builder, configFile, &config, &errors))
    {
        LOG_ERROR << "Failed to parse config.json: " << errors;
        return 1;
    }
    Json::Value processedConfig = ConfigLoader::loadConfig(config);

    // 4. Load processed config into Drogon
    drogon::app().loadConfigJson(std::move(processedConfig));
    setupCors();
    drogon::app().run();
    return 0;
}
```

- [ ] **Step 3: Remove duplicate ConfigLoader call from PayPlugin.cc**

In `PayPlugin.cc`, the `initAndStart` method currently calls `ConfigLoader::loadEnvFile` and `ConfigLoader::loadConfig` again. Since main.cc now handles this, remove those lines. The `config` parameter passed to `initAndStart` is already the processed config from `loadConfigJson`.

Remove these lines from `PayPlugin::initAndStart`:

```cpp
    // Load environment variables from .env file
    ConfigLoader::loadEnvFile(".env");
    LOG_INFO << "Environment variables loaded from .env file";

    // Replace placeholders with actual environment variable values
    Json::Value processedConfig = ConfigLoader::loadConfig(config);
    LOG_INFO << "Configuration placeholders replaced";
```

And change all references to `processedConfig` in that method to `config` (the parameter is already processed).

- [ ] **Step 4: Build and verify**

```bash
# Create .env file for dev testing
echo PAY_DB_PASSWORD=123456 > PayBackend/.env
echo PAY_REDIS_PASSWORD=123456 >> PayBackend/.env
echo PAY_API_KEY=test_key_123456 >> PayBackend/.env

scripts\build.bat
```
Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add PayBackend/config.json PayBackend/main.cc PayBackend/plugins/PayPlugin.cc
git commit -m "feat: remove hardcoded credentials, use env vars via ConfigLoader"
```

---

## Task 4: Update .env example files

**Files:**
- Modify: `PayBackend/.env.example`
- Modify: `PayBackend/.env.development.example`
- Modify: `PayBackend/.env.production.example`

- [ ] **Step 1: Update .env.example**

Add the three canonical variables at the top:

```
# === Pay Plugin Required Variables ===
PAY_DB_PASSWORD=your_db_password_here
PAY_REDIS_PASSWORD=your_redis_password_here
PAY_API_KEY=your_api_key_here
```

- [ ] **Step 2: Update .env.development.example**

Add at the top:

```
# === Pay Plugin Required Variables ===
PAY_DB_PASSWORD=123456
PAY_REDIS_PASSWORD=123456
PAY_API_KEY=test_key_123456
```

- [ ] **Step 3: Rewrite .env.production.example**

Replace the `DB_PASSWORD` and `REDIS_PASSWORD` entries with canonical names. Keep all other variables. Key changes:

```
# === Pay Plugin Required Variables ===
PAY_DB_PASSWORD=${PAY_DB_PASSWORD}
PAY_REDIS_PASSWORD=${PAY_REDIS_PASSWORD}
PAY_API_KEY=${PAY_API_KEY}
```

Remove the old `DB_PASSWORD=${DB_PASSWORD}` and `REDIS_PASSWORD=${REDIS_PASSWORD}` lines.

- [ ] **Step 4: Commit**

```bash
git add PayBackend/.env.example PayBackend/.env.development.example PayBackend/.env.production.example
git commit -m "chore: align .env example files with canonical PAY_* variable names"
```

---

## Task 5: Add `/healthz` liveness endpoint

**Files:**
- Modify: `PayBackend/controllers/HealthCheckController.h`
- Modify: `PayBackend/controllers/HealthCheckController.cc`

- [ ] **Step 1: Add method declarations to header**

Update `HealthCheckController.h`:

```cpp
#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class HealthCheckController : public drogon::HttpController<HealthCheckController>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HealthCheckController::healthz, "/healthz", Get, Options);
    ADD_METHOD_TO(HealthCheckController::readyz, "/readyz", Get, Options);
    ADD_METHOD_TO(HealthCheckController::health, "/health", Get, Options);
    METHOD_LIST_END

    void healthz(
      const HttpRequestPtr &req,
      std::function<void(const HttpResponsePtr &)> &&callback
    );

    void readyz(
      const HttpRequestPtr &req,
      std::function<void(const HttpResponsePtr &)> &&callback
    );

    void health(
      const HttpRequestPtr &req,
      std::function<void(const HttpResponsePtr &)> &&callback
    );

  private:
    // Debounce state for /readyz
    std::atomic<int> consecutiveFailures_{0};
    std::atomic<bool> lastReadyState_{true};
    static constexpr int FAILURE_THRESHOLD = 3;
};
```

- [ ] **Step 2: Implement healthz in .cc**

Replace the `HealthCheckController.cc` content. First implement `healthz`:

```cpp
#include "HealthCheckController.h"
#include <drogon/drogon.h>
#include <json/json.h>
#include <chrono>

void HealthCheckController::healthz(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    if (req->method() == Options)
    {
        auto resp = HttpResponse::newHttpResponse();
        callback(resp);
        return;
    }

    Json::Value response;

    // Check: event loop is alive
    bool loopAlive = (drogon::app().getLoop() != nullptr);
    // Check: framework is running
    bool isRunning = drogon::app().isRunning();

    if (loopAlive && isRunning)
    {
        response["status"] = "alive";
        auto resp = HttpResponse::newHttpJsonResponse(response);
        resp->setStatusCode(k200OK);
        callback(resp);
    }
    else
    {
        response["status"] = "dead";
        if (!loopAlive)
            response["reason"] = "event_loop_null";
        else
            response["reason"] = "not_running";
        auto resp = HttpResponse::newHttpJsonResponse(response);
        resp->setStatusCode(k503ServiceUnavailable);
        callback(resp);
    }
}
```

- [ ] **Step 3: Build to verify healthz compiles**

```bash
scripts\build.bat
```
Expected: Build succeeds. (readyz and health not yet implemented, just healthz for now)

- [ ] **Step 4: Commit**

```bash
git add PayBackend/controllers/HealthCheckController.h PayBackend/controllers/HealthCheckController.cc
git commit -m "feat: add /healthz liveness endpoint"
```

---

## Task 6: Add `/readyz` readiness endpoint with debounce

**Files:**
- Modify: `PayBackend/controllers/HealthCheckController.cc`

- [ ] **Step 1: Implement readyz**

Add the `readyz` implementation to `HealthCheckController.cc` (after `healthz`):

```cpp
void HealthCheckController::readyz(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    if (req->method() == Options)
    {
        auto resp = HttpResponse::newHttpResponse();
        callback(resp);
        return;
    }

    // Check dependencies: DB, Redis, PayPlugin, config files
    auto dbClient = drogon::app().getDbClient();
    auto redisClient = drogon::app().getRedisClient();

    // Track how many async checks are pending
    struct ReadyState
    {
        std::mutex mtx;
        std::vector<std::string> failed;
        int pending = 0;
        bool timedOut = false;
    };
    auto state = std::make_shared<ReadyState>();

    // Count pending checks: DB + Redis + PayPlugin
    state->pending = 2;  // DB + Redis
    if (redisClient == nullptr)
        state->pending = 1;  // Only DB, Redis not configured

    // DB check: SELECT 1
    if (dbClient)
    {
        dbClient->execSqlAsync(
          "SELECT 1",
          [state](const drogon::orm::Result &) {
              std::lock_guard<std::mutex> lock(state->mtx);
              state->pending--;
          },
          [state](const drogon::orm::DrogonDbException &e) {
              std::lock_guard<std::mutex> lock(state->mtx);
              state->failed.push_back("db");
              state->pending--;
          }
        );
    }
    else
    {
        std::lock_guard<std::mutex> lock(state->mtx);
        state->failed.push_back("db");
        state->pending--;
    }

    // Redis check: PING (using execCommandAsync, not execSqlAsync)
    if (redisClient)
    {
        redisClient->execCommandAsync(
          [state](const drogon::nosql::RedisResult &) {
              std::lock_guard<std::mutex> lock(state->mtx);
              state->pending--;
          },
          [state](const std::exception &e) {
              std::lock_guard<std::mutex> lock(state->mtx);
              state->failed.push_back("redis");
              state->pending--;
          },
          "PING"
        );
    }

    // Set a 1-second deadline for all checks
    auto *loop = drogon::app().getLoop();
    loop->runAfter(1.0, [state, callback, this]() {
        std::lock_guard<std::mutex> lock(state->mtx);
        if (state->pending > 0)
        {
            state->failed.push_back("timeout");
        }

        Json::Value response;
        bool ready = state->failed.empty();

        // Debounce: update consecutive failure counter
        if (ready)
        {
            consecutiveFailures_.store(0);
            lastReadyState_.store(true);
        }
        else
        {
            int failures = consecutiveFailures_.fetch_add(1) + 1;
            if (failures >= FAILURE_THRESHOLD)
            {
                lastReadyState_.store(false);
            }
        }

        bool reportReady = lastReadyState_.load();

        if (reportReady)
        {
            response["status"] = "ready";
            auto resp = HttpResponse::newHttpJsonResponse(response);
            resp->setStatusCode(k200OK);
            callback(resp);
        }
        else
        {
            response["status"] = "not_ready";
            Json::Value failed(Json::arrayValue);
            for (const auto &f : state->failed)
            {
                failed.append(f);
            }
            response["failed"] = failed;
            auto resp = HttpResponse::newHttpJsonResponse(response);
            resp->setStatusCode(k503ServiceUnavailable);
            callback(resp);
        }
    });
}
```

- [ ] **Step 2: Build and verify**

```bash
scripts\build.bat
```
Expected: Build succeeds. If Redis async API differs, adjust the Redis check call.

- [ ] **Step 3: Commit**

```bash
git add PayBackend/controllers/HealthCheckController.cc
git commit -m "feat: add /readyz readiness endpoint with debounce"
```

---

## Task 7: Add `/health` compatibility alias

**Files:**
- Modify: `PayBackend/controllers/HealthCheckController.cc`

- [ ] **Step 1: Implement health alias**

Add the `health` implementation to `HealthCheckController.cc`:

```cpp
void HealthCheckController::health(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    if (req->method() == Options)
    {
        auto resp = HttpResponse::newHttpResponse();
        callback(resp);
        return;
    }

    // Forward to readyz logic
    readyz(req, [callback](const HttpResponsePtr &resp) {
        resp->addHeader("Deprecation", "true");
        // Sunset: 90 days from release date (update at release time)
        resp->addHeader("Sunset", "2026-08-28");
        callback(resp);
    });
}
```

- [ ] **Step 2: Build and verify**

```bash
scripts\build.bat
```
Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add PayBackend/controllers/HealthCheckController.cc
git commit -m "feat: add /health compatibility alias with Deprecation header"
```

---

## Task 8: Add health probe tests

**Files:**
- Create: `PayBackend/test/HealthProbeTest.cc`
- Modify: `PayBackend/test/CMakeLists.txt`

- [ ] **Step 1: Write health probe tests**

Create `PayBackend/test/HealthProbeTest.cc`:

```cpp
#include <drogon/drogon_test.h>
#include <drogon/drogon.h>

using namespace drogon;

DROGON_TEST(HealthProbe_LivenessEndpoint)
{
    auto client = HttpClient::newHttpClient("http://127.0.0.1:5566");
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/healthz");

    client->sendRequest(req, [TEST_CTX](ReqResult result, const HttpResponsePtr &resp) {
        REQUIRE(result == ReqResult::ok);
        REQUIRE(resp != nullptr);
        CHECK(resp->getStatusCode() == k200OK);

        auto json = resp->getJsonObject();
        REQUIRE(json != nullptr);
        CHECK((*json)["status"].asString() == "alive");
    });
}

DROGON_TEST(HealthProbe_ReadinessEndpoint)
{
    auto client = HttpClient::newHttpClient("http://127.0.0.1:5566");
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/readyz");

    client->sendRequest(req, [TEST_CTX](ReqResult result, const HttpResponsePtr &resp) {
        REQUIRE(result == ReqResult::ok);
        REQUIRE(resp != nullptr);
        // With DB + Redis running, should be 200
        CHECK(resp->getStatusCode() == k200OK);

        auto json = resp->getJsonObject();
        REQUIRE(json != nullptr);
        CHECK((*json)["status"].asString() == "ready");
    });
}

DROGON_TEST(HealthProbe_CompatEndpoint_DeprecationHeader)
{
    auto client = HttpClient::newHttpClient("http://127.0.0.1:5566");
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/health");

    client->sendRequest(req, [TEST_CTX](ReqResult result, const HttpResponsePtr &resp) {
        REQUIRE(result == ReqResult::ok);
        REQUIRE(resp != nullptr);

        // Should have Deprecation header
        auto deprecation = resp->getHeader("Deprecation");
        CHECK(deprecation == "true");

        // Should have Sunset header
        auto sunset = resp->getHeader("Sunset");
        CHECK(!sunset.empty());
    });
}
```

- [ ] **Step 2: Add to test CMakeLists.txt**

Add `HealthProbeTest.cc` to `TEST_SRC`.

- [ ] **Step 3: Build and run tests (requires running server)**

These are integration tests requiring a running server. Run from `PayBackend/`:

```bash
# Terminal 1: Start server
build\Release\PayServer.exe

# Terminal 2: Run tests
build\Release\test_payplugin.exe --gtest_filter=*HealthProbe*
```
Expected: All 3 tests PASS (or skip if server not running).

- [ ] **Step 4: Commit**

```bash
git add PayBackend/test/HealthProbeTest.cc PayBackend/test/CMakeLists.txt
git commit -m "test: add health probe integration tests"
```

---

## Task 9: Update Docker configuration

**Files:**
- Modify: `PayBackend/Dockerfile`
- Modify: `PayBackend/docker-compose.yml`

- [ ] **Step 1: Update Dockerfile HEALTHCHECK and EXPOSE**

Change port from 8080 to 5566 and healthcheck path:

```dockerfile
# Expose port
EXPOSE 5566

# Health check
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:5566/healthz || exit 1
```

- [ ] **Step 2: Update docker-compose.yml**

Update port mapping and healthcheck:

```yaml
  payserver:
    build:
      context: .
      dockerfile: Dockerfile
    container_name: pay_server
    ports:
      - "5566:5566"
    environment:
      - PAY_DB_PASSWORD=postgres
      - PAY_REDIS_PASSWORD=
      - PAY_API_KEY=test_key_123456
    volumes:
      - ./config.json:/app/config.json:ro
      - ./certs:/app/certs:ro
      - ./logs:/app/logs
    depends_on:
      postgres:
        condition: service_healthy
      redis:
        condition: service_healthy
    healthcheck:
      test: ["CMD", "curl", "-f", "http://localhost:5566/healthz"]
      interval: 30s
      timeout: 3s
      retries: 3
      start_period: 10s
    networks:
      - pay_network
    restart: unless-stopped
```

- [ ] **Step 3: Commit**

```bash
git add PayBackend/Dockerfile PayBackend/docker-compose.yml
git commit -m "fix: align Docker config with port 5566 and /healthz endpoint"
```

---

## Task 10: Add CI static analysis

**Files:**
- Create: `.clang-tidy`
- Modify: `.github/workflows/ci-linux.yml`

- [ ] **Step 1: Create .clang-tidy**

Create `.clang-tidy` in project root:

```yaml
---
Checks: >
  -*,
  bugprone-*,
  modernize-*,
  performance-*,
  -modernize-use-trailing-return-type,
  -modernize-avoid-c-arrays
WarningsAsErrors: ''
HeaderFilterRegex: 'PayBackend/.*'
FormatStyle: none
```

- [ ] **Step 2: Add static-analysis job to ci-linux.yml**

Add a new job before `build-and-test`:

```yaml
  static-analysis:
    runs-on: ubuntu-22.04
    timeout-minutes: 10

    steps:
      - uses: actions/checkout@v4

      - name: Install clang-tidy
        run: sudo apt-get update && sudo apt-get install -y clang-tidy

      - name: Emoji detection
        run: |
          echo "Checking for emoji in source files..."
          if grep -rPn '[\x{1F000}-\x{1FFFF}]' PayBackend/ --include='*.h' --include='*.cc'; then
            echo "ERROR: Emoji found in source files"
            exit 1
          fi
          echo "No emoji found."

      - name: Hardcoded secrets detection
        run: |
          echo "Checking for hardcoded secrets..."
          if grep -rn -E '(passwd|password|secret)\s*[:=]\s*"[^_${}][^"]{2,}"' PayBackend/ \
               --include='*.h' --include='*.cc' --include='*.json' \
               | grep -v '__env_var' | grep -v '.example' | grep -v 'test_key_'; then
            echo "ERROR: Possible hardcoded secret found"
            exit 1
          fi
          echo "No hardcoded secrets found."

      - name: CoroMapper usage detection
        run: |
          echo "Checking for CoroMapper usage..."
          if grep -rn 'CoroMapper' PayBackend/ --include='*.h' --include='*.cc'; then
            echo "ERROR: CoroMapper usage detected"
            exit 1
          fi
          echo "No CoroMapper usage found."

      - name: Old Plugin API detection
        run: |
          echo "Checking for old Plugin API usage in tests..."
          if grep -rn 'plugin.*->.*method.*req.*callback' PayBackend/test/ --include='*.cc'; then
            echo "ERROR: Old Plugin API usage detected in tests"
            exit 1
          fi
          echo "No old Plugin API usage found."
```

- [ ] **Step 3: Add PAY_* env vars to test step**

In the existing `build-and-test` job, add the env vars to the Test step (already partially present):

```yaml
      - name: Test
        working-directory: ${{github.workspace}}/PayBackend/build
        env:
          PAY_REDIS_PASSWORD: ""
          PAY_DB_HOST: "127.0.0.1"
          PAY_REDIS_HOST: "127.0.0.1"
          PAY_DB_PASSWORD: "123456"
          PAY_API_KEY: "test_key_123456"
        run: ctest -V -C ${{env.BUILD_TYPE}} --output-on-failure --timeout 120
```

- [ ] **Step 4: Commit**

```bash
git add .clang-tidy .github/workflows/ci-linux.yml
git commit -m "ci: add static analysis job (emoji, secrets, CoroMapper, clang-tidy)"
```

---

## Task 11: Build and run full test suite

**Files:** None (verification only)

- [ ] **Step 1: Full rebuild**

```bash
cd PayBackend
scripts\build.bat
```
Expected: Build succeeds with no errors.

- [ ] **Step 2: Run all unit tests**

```bash
cd PayBackend\build\Release
test_payplugin.exe --gtest_filter=-*Integration*:*HealthProbe*
```
Expected: All unit tests PASS (ConfigLoader, StartupValidator, PayUtils, etc.).

- [ ] **Step 3: Run CI checks locally (grep-based)**

```bash
# Emoji check
grep -rPn '[\x{1F000}-\x{1FFFF}]' PayBackend/ --include='*.h' --include='*.cc' || echo "PASS"

# Hardcoded secrets check
grep -rn -E '(passwd|password|secret)\s*[:=]\s*"[^_${}][^"]{2,}"' PayBackend/ \
  --include='*.h' --include='*.cc' --include='*.json' \
  | grep -v '__env_var' | grep -v '.example' || echo "PASS"

# CoroMapper check
grep -rn 'CoroMapper' PayBackend/ --include='*.h' --include='*.cc' || echo "PASS"
```
Expected: All checks PASS (no output or "PASS").

- [ ] **Step 4: Verify config.json has no hardcoded secrets**

```bash
grep -n '123456' PayBackend/config.json || echo "PASS - no hardcoded passwords"
```
Expected: No output (PASS).

- [ ] **Step 5: Final commit if any fixes needed**

```bash
git add -A
git commit -m "fix: address test failures from M1 changes"
```
