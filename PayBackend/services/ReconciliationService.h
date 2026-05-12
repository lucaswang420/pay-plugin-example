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
#include "PaymentService.h"
#include "RefundService.h"
#include "../plugins/WechatPayClient.h"
#include "../plugins/AlipaySandboxClient.h"
#include <functional>
#include <memory>
#include <string>
#include <trantor/net/EventLoop.h>

class ReconciliationService
{
  public:
    ReconciliationService(
      std::shared_ptr<PaymentService> paymentService,
      std::shared_ptr<RefundService> refundService,
      std::shared_ptr<WechatPayClient> wechatClient,
      std::shared_ptr<AlipaySandboxClient> alipayClient,
      std::shared_ptr<drogon::orm::DbClient> dbClient
    );

    void startReconcileTimer();
    void stopReconcileTimer();

    void reconcile(std::function<void(int success, int failed)> &&callback);

  private:
    void syncPendingWeChatOrders();
    void syncPendingAlipayOrders();
    void syncPendingRefunds();

    bool isWeChatConfigured() const;
    bool isAlipayConfigured() const;

    std::shared_ptr<PaymentService> paymentService_;
    std::shared_ptr<RefundService> refundService_;
    std::shared_ptr<WechatPayClient> wechatClient_;
    std::shared_ptr<AlipaySandboxClient> alipayClient_;
    std::shared_ptr<drogon::orm::DbClient> dbClient_;
    trantor::TimerId reconcileTimerId_;
    int reconcileIntervalSeconds_;
    int reconcileBatchSize_;
};
