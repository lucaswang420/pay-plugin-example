#pragma once

// ============================================================================
// TECHNICAL DEBT NOTICE
// ============================================================================
// This service was NOT implemented using Test-Driven Development (TDD).
//
// Created: 2026-04-11
// TDD Status: Non-compliant
// Test Coverage: Integration tests only (no unit tests)
//
// Issues:
// - Implemented without failing tests first
// - Unit tests were removed due to being incomplete (nullptr mocks)
// - Behavior verified through integration tests only
//
// Priority: Medium
// Planned Action: Reimplement with TDD when time permits
//
// Integration Test Status: See test/ directory for integration tests
// that verify this service's behavior.
//
// Note: When modifying this service, consider adding TDD-compliant
// unit tests for new functionality.
// ============================================================================

#include <drogon/orm/DbClient.h>
#include <drogon/nosql/RedisClient.h>
#include <json/json.h>
#include <functional>
#include <memory>
#include <string>

class IdempotencyService
{
  public:
    enum class CheckStatus
    {
        Started,
        Replay,
        InProgress,
        Conflict,
        Error
    };

    struct CheckResult
    {
        CheckStatus status{CheckStatus::Error};
        Json::Value cachedResult;
        std::string message;
    };

    using CheckCallback = std::function<void(bool canProceed, const Json::Value &cachedResult)>;
    using StatusCallback = std::function<void(const CheckResult &result)>;
    using UpdateCallback = std::function<void(bool success)>;

    IdempotencyService(
      std::shared_ptr<drogon::orm::DbClient> dbClient,
      drogon::nosql::RedisClientPtr redisClient,
      int64_t ttlSeconds = 604800  // 7 days default
    );

    // Check idempotency and optionally reserve key
    void checkAndSet(
      const std::string &idempotencyKey,
      const std::string &requestHash,
      const Json::Value &request,
      CheckCallback &&callback
    );

    void checkAndSetStatus(
      const std::string &idempotencyKey,
      const std::string &requestHash,
      const Json::Value &request,
      StatusCallback &&callback
    );

    // Update result after successful operation
    void updateResult(
      const std::string &idempotencyKey,
      const std::string &requestHash,
      const Json::Value &response,
      UpdateCallback &&callback = [](bool) {}
    );

  private:
    std::shared_ptr<drogon::orm::DbClient> dbClient_;
    drogon::nosql::RedisClientPtr redisClient_;
    int64_t ttlSeconds_;

    void checkDatabase(
      const std::string &idempotencyKey,
      const std::string &requestHash,
      StatusCallback &&callback
    );
};
