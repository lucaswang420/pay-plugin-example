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
#include "../plugins/WechatPayClient.h"
#include <json/json.h>
#include <functional>
#include <memory>
#include <string>

class CallbackService
{
  public:
    using CallbackResult =
      std::function<void(const Json::Value &result, const std::error_code &error)>;

    CallbackService(
      std::shared_ptr<WechatPayClient> wechatClient,
      std::shared_ptr<drogon::orm::DbClient> dbClient
    );

    void handlePaymentCallback(
      const std::string &body,
      const std::string &signature,
      const std::string &timestamp,
      const std::string &nonce,
      const std::string &serialNo,
      CallbackResult &&callback
    );

    void handleRefundCallback(
      const std::string &body,
      const std::string &signature,
      const std::string &timestamp,
      const std::string &nonce,
      const std::string &serialNo,
      CallbackResult &&callback
    );

  private:
    bool verifySignature(
      const std::string &body,
      const std::string &signature,
      const std::string &timestamp,
      const std::string &nonce,
      const std::string &serialNo
    );

    std::shared_ptr<WechatPayClient> wechatClient_;
    std::shared_ptr<drogon::orm::DbClient> dbClient_;
};
