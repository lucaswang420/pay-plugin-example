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
#include "IdempotencyService.h"
#include "../plugins/WechatPayClient.h"
#include "../plugins/AlipaySandboxClient.h"
#include <json/json.h>
#include <functional>
#include <memory>
#include <string>

struct CreatePaymentRequest
{
    std::string orderNo;
    std::string amount;
    std::string currency;
    std::string description;
    std::string notifyUrl;
    int64_t userId;
    Json::Value sceneInfo;
    std::string channel;     // Payment channel: "alipay" or "wechat"
    std::string timeExpire;  // Order expiration time (RFC 3339 format)
    std::string attach;      // Additional data
};

class PaymentService
{
  public:
    using PaymentCallback =
      std::function<void(const Json::Value &result, const std::error_code &error)>;

    PaymentService(
      std::shared_ptr<WechatPayClient> wechatClient,
      std::shared_ptr<AlipaySandboxClient> alipayClient,
      std::shared_ptr<drogon::orm::DbClient> dbClient,
      drogon::nosql::RedisClientPtr redisClient,
      std::shared_ptr<IdempotencyService> idempotencyService
    );

    void createPayment(
      const CreatePaymentRequest &request,
      const std::string &idempotencyKey,
      PaymentCallback &&callback
    );

    void createQRPayment(const Json::Value &request, PaymentCallback &&callback);

    void queryOrder(const std::string &orderNo, PaymentCallback &&callback);

    void queryOrderList(
      const std::string &status,
      const int64_t userId,
      const size_t limit,
      const size_t offset,
      PaymentCallback &&callback
    );

    void syncOrderStatusFromWechat(
      const std::string &orderNo,
      const Json::Value &wechatResult,
      std::function<void(const std::string &status)> &&callback
    );

    void syncOrderStatusFromAlipay(
      const std::string &orderNo,
      const Json::Value &alipayResult,
      std::function<void(const std::string &status)> &&callback
    );

    void reconcileSummary(const std::string &date, PaymentCallback &&callback);

  private:
    std::shared_ptr<WechatPayClient> wechatClient_;
    std::shared_ptr<AlipaySandboxClient> alipayClient_;
    std::shared_ptr<drogon::orm::DbClient> dbClient_;
    drogon::nosql::RedisClientPtr redisClient_;
    std::shared_ptr<IdempotencyService> idempotencyService_;

    std::string generatePaymentNo();
    void proceedCreatePayment(
      const CreatePaymentRequest &request,
      const std::string &paymentNo,
      int64_t totalFen,
      PaymentCallback &&callback
    );
};
