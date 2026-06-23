#include "PayPlugin.h"
#include "../services/PaymentService.h"
#include "../services/RefundService.h"
#include "../services/CallbackService.h"
#include "../services/ReconciliationService.h"
#include "../services/IdempotencyService.h"
#include <drogon/drogon.h>

void PayPlugin::initAndStart(const Json::Value &config)
{
    LOG_INFO << "Initializing PayPlugin...";

    // 1. Get infrastructure
    dbClient_ = drogon::app().getDbClient();
    if (!dbClient_)
    {
        LOG_ERROR << "Failed to get database client";
        return;
    }

    redisClient_ = drogon::app().getRedisClient();
    if (!redisClient_)
    {
        LOG_WARN << "Redis client not available, idempotency will be database-only";
    }

    // 2. Create WechatPayClient
    try
    {
        wechatClient_ = std::make_shared<WechatPayClient>(config["wechat"]);
        LOG_INFO << "WechatPayClient created";
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "Failed to create WechatPayClient: " << e.what();
        return;
    }

    // 2.5. Create AlipaySandboxClient (optional)
    if (config.isMember("alipay_sandbox"))
    {
        try
        {
            alipayClient_ =
              std::make_shared<AlipaySandboxClient>(config["alipay_sandbox"]);
            LOG_INFO << "AlipaySandboxClient created";
        }
        catch (const std::exception &e)
        {
            LOG_WARN << "Failed to create AlipaySandboxClient: " << e.what();
            // Non-fatal: continue without Alipay support
        }
    }
    else
    {
        LOG_INFO << "AlipaySandboxClient not configured (optional)";
    }

    // 3. Create IdempotencyService (no dependencies)
    int64_t idempotencyTtl = config["idempotency"].get("ttl", 604800).asInt64();
    idempotencyService_ =
      std::make_shared<IdempotencyService>(dbClient_, redisClient_, idempotencyTtl);
    LOG_INFO << "IdempotencyService created";

    // 4. Create business Services (depend on IdempotencyService)
    paymentService_ = std::make_shared<PaymentService>(
      wechatClient_, alipayClient_, dbClient_, redisClient_, idempotencyService_
    );
    LOG_INFO << "PaymentService created";

    refundService_ =
      std::make_shared<RefundService>(wechatClient_, alipayClient_, dbClient_, idempotencyService_);
    LOG_INFO << "RefundService created";

    callbackService_ = std::make_shared<CallbackService>(wechatClient_, dbClient_);
    LOG_INFO << "CallbackService created";

    // 5. Create and start ReconciliationService (depends on PaymentService and RefundService)
    reconciliationService_ = std::make_unique<ReconciliationService>(
      paymentService_, refundService_, wechatClient_, alipayClient_, dbClient_
    );
    reconciliationService_->startReconcileTimer();
    LOG_INFO << "ReconciliationService created and timer started";

    // 6. Start certificate refresh timer
    startCertRefreshTimer();

    LOG_INFO << "PayPlugin initialization complete";
}

void PayPlugin::shutdown()
{
    LOG_INFO << "Shutting down PayPlugin...";

    // Stop reconciliation timer
    if (reconciliationService_)
    {
        reconciliationService_->stopReconcileTimer();
    }

    // Stop certificate refresh timer
    if (certRefreshTimerId_)
    {
        drogon::app().getLoop()->invalidateTimer(certRefreshTimerId_);
        certRefreshTimerId_ = 0;
    }

    LOG_INFO << "PayPlugin shutdown complete";
}

std::shared_ptr<PaymentService> PayPlugin::paymentService()
{
    return paymentService_;
}

std::shared_ptr<RefundService> PayPlugin::refundService()
{
    return refundService_;
}

std::shared_ptr<CallbackService> PayPlugin::callbackService()
{
    if (!callbackService_)
    {
        callbackService_ = std::make_shared<CallbackService>(wechatClient_, dbClient_);
    }
    return callbackService_;
}

std::shared_ptr<IdempotencyService> PayPlugin::idempotencyService()
{
    return idempotencyService_;
}

void PayPlugin::setTestClients(
  std::shared_ptr<WechatPayClient> wechatClient,
  std::shared_ptr<AlipaySandboxClient> alipayClient,
  std::shared_ptr<drogon::orm::DbClient> dbClient
)
{
    LOG_DEBUG << "PayPlugin::setTestClients called for testing";

    // Store test clients
    wechatClient_ = wechatClient;
    alipayClient_ = alipayClient;
    dbClient_ = dbClient;

    // Create IdempotencyService with test clients (no Redis for tests)
    idempotencyService_ = std::make_shared<IdempotencyService>(dbClient_, nullptr, 604800);

    // Create business services with test clients
    paymentService_ = std::make_shared<PaymentService>(
      wechatClient_, alipayClient_, dbClient_, nullptr, idempotencyService_
    );

    refundService_ =
      std::make_shared<RefundService>(wechatClient_, alipayClient_, dbClient_, idempotencyService_);

    callbackService_ = std::make_shared<CallbackService>(wechatClient_, dbClient_);

    // Note: ReconciliationService is NOT created for tests
    // (it would start background timers)
}

void PayPlugin::startCertRefreshTimer()
{
    if (!wechatClient_)
    {
        return;
    }

    auto loop = drogon::app().getLoop();
    if (!loop)
    {
        return;
    }

    // Initial certificate download
    wechatClient_->downloadCertificates([](const Json::Value &, const std::string &err) {
        if (!err.empty())
        {
            LOG_ERROR << "Failed to download wechat certificates on startup: " << err;
        }
        else
        {
            LOG_INFO << "Successfully downloaded wechat certificates on startup";
        }
    });

    // Set up periodic refresh (every 12 hours by default)
    certRefreshTimerId_ = loop->runEvery(43200.0, [this]() {
        if (!wechatClient_)
        {
            return;
        }
        wechatClient_->downloadCertificates([](const Json::Value &, const std::string &err) {
            if (!err.empty())
            {
                LOG_WARN << "Wechat certificate refresh failed: " << err;
            }
            else
            {
                LOG_INFO << "Wechat certificates refreshed";
            }
        });
    });
}
