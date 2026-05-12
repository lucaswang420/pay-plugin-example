#pragma once

#include <drogon/plugins/Plugin.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/nosql/RedisClient.h>
#include <drogon/orm/DbClient.h>
#include <json/json.h>
#include <memory>
#include <string>
#include <trantor/net/EventLoop.h>
#include <trantor/utils/Date.h>
#include "WechatPayClient.h"
#include "AlipaySandboxClient.h"
#include "../services/ReconciliationService.h"

// Forward declarations
class PaymentService;
class RefundService;
class CallbackService;
class IdempotencyService;

class PayPlugin : public drogon::Plugin<PayPlugin>
{
  public:
    PayPlugin() = default;
    void initAndStart(const Json::Value &config) override;
    void shutdown() override;

    // Service accessors
    std::shared_ptr<PaymentService> paymentService();
    std::shared_ptr<RefundService> refundService();
    std::shared_ptr<CallbackService> callbackService();
    std::shared_ptr<IdempotencyService> idempotencyService();

    // Test support: Initialize services with test clients
    // NOTE: This method is for integration testing only
    void setTestClients(
      std::shared_ptr<WechatPayClient> wechatClient,
      std::shared_ptr<AlipaySandboxClient> alipayClient,
      std::shared_ptr<drogon::orm::DbClient> dbClient
    );

  private:
    // Services
    std::shared_ptr<PaymentService> paymentService_;
    std::shared_ptr<RefundService> refundService_;
    std::shared_ptr<CallbackService> callbackService_;
    std::unique_ptr<ReconciliationService> reconciliationService_;
    std::shared_ptr<IdempotencyService> idempotencyService_;

    // Infrastructure
    std::shared_ptr<WechatPayClient> wechatClient_;
    std::shared_ptr<AlipaySandboxClient> alipayClient_;
    std::shared_ptr<drogon::orm::DbClient> dbClient_;
    drogon::nosql::RedisClientPtr redisClient_;

    // Timers
    trantor::TimerId certRefreshTimerId_;

    void startCertRefreshTimer();
};
