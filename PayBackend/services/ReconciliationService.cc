#include "ReconciliationService.h"
#include <drogon/drogon.h>

using namespace drogon;

ReconciliationService::ReconciliationService(
  std::shared_ptr<PaymentService> paymentService,
  std::shared_ptr<RefundService> refundService,
  std::shared_ptr<WechatPayClient> wechatClient,
  std::shared_ptr<AlipaySandboxClient> alipayClient,
  std::shared_ptr<drogon::orm::DbClient> dbClient
)
    : paymentService_(paymentService),
      refundService_(refundService),
      wechatClient_(wechatClient),
      alipayClient_(alipayClient),
      dbClient_(dbClient),
      reconcileTimerId_(0),
      reconcileIntervalSeconds_(300),
      reconcileBatchSize_(50)
{
}

void ReconciliationService::startReconcileTimer()
{
    reconcileTimerId_ =
      drogon::app().getLoop()->runEvery(std::chrono::seconds(reconcileIntervalSeconds_), [this]() {
          this->reconcile([](int success, int failed) {
              LOG_INFO << "Reconciliation completed: success=" << success << ", failed=" << failed;
          });
      });

    LOG_INFO << "Reconciliation timer started (interval: " << reconcileIntervalSeconds_ << "s)";
}

void ReconciliationService::stopReconcileTimer()
{
    if (reconcileTimerId_)
    {
        drogon::app().getLoop()->invalidateTimer(reconcileTimerId_);
        reconcileTimerId_ = 0;
        LOG_INFO << "Reconciliation timer stopped";
    }
}

void ReconciliationService::reconcile(std::function<void(int success, int failed)> &&callback)
{
    // Track success and failed counts
    auto successCount = std::make_shared<int>(0);
    auto failedCount = std::make_shared<int>(0);

    // Sync pending WeChat Pay orders (only if configured)
    if (isWeChatConfigured())
    {
        syncPendingWeChatOrders();
    }
    else
    {
        LOG_DEBUG << "WeChat Pay not configured, skipping WeChat order reconciliation";
    }

    // Sync pending Alipay orders (only if configured)
    if (isAlipayConfigured())
    {
        syncPendingAlipayOrders();
    }
    else
    {
        LOG_DEBUG << "Alipay not configured, skipping Alipay order reconciliation";
    }

    // Sync pending refunds (only if WeChat is configured)
    if (isWeChatConfigured())
    {
        syncPendingRefunds();
    }
    else
    {
        LOG_DEBUG << "WeChat Pay not configured, skipping refund reconciliation";
    }

    callback(*successCount, *failedCount);
}

bool ReconciliationService::isWeChatConfigured() const
{
    if (!wechatClient_)
    {
        return false;
    }
    // Check if WeChat client has valid configuration (not placeholders)
    return wechatClient_->isConfigured();
}

bool ReconciliationService::isAlipayConfigured() const
{
    if (!alipayClient_)
    {
        return false;
    }
    // Check if Alipay client has valid configuration (not placeholders)
    return alipayClient_->isConfigured();
}

void ReconciliationService::syncPendingWeChatOrders()
{
    if (!dbClient_)
    {
        return;
    }

    if (!wechatClient_)
    {
        return;
    }

    dbClient_->execSqlAsync(
      "SELECT order_no FROM pay_order WHERE status = $1 AND channel = $2 "
      "ORDER BY updated_at DESC LIMIT $3",
      [this](const drogon::orm::Result &r) {
          for (const auto &row : r)
          {
              const std::string orderNo = row["order_no"].as<std::string>();
              wechatClient_->queryTransaction(
                orderNo, [this, orderNo](const Json::Value &result, const std::string &error) {
                    if (!error.empty())
                    {
                        LOG_WARN << "WeChat query failed for order " << orderNo << ": " << error;
                        return;
                    }
                    paymentService_
                      ->syncOrderStatusFromWechat(orderNo, result, [](const std::string &) {});
                }
              );
          }
      },
      [](const drogon::orm::DrogonDbException &e) {
          LOG_ERROR << "WeChat reconcile query error: " << e.base().what();
      },
      "PAYING",
      "wechat",
      reconcileBatchSize_
    );
}

void ReconciliationService::syncPendingAlipayOrders()
{
    if (!dbClient_)
    {
        return;
    }

    if (!alipayClient_)
    {
        return;
    }

    dbClient_->execSqlAsync(
      "SELECT order_no FROM pay_order WHERE status = $1 AND channel = $2 "
      "ORDER BY updated_at DESC LIMIT $3",
      [this](const drogon::orm::Result &r) {
          for (const auto &row : r)
          {
              const std::string orderNo = row["order_no"].as<std::string>();
              alipayClient_->queryTrade(
                orderNo, [this, orderNo](const Json::Value &result, const std::string &error) {
                    if (!error.empty())
                    {
                        LOG_WARN << "Alipay query failed for order " << orderNo << ": " << error;
                        return;
                    }
                    // Sync order status from Alipay response
                    paymentService_->syncOrderStatusFromAlipay(
                      orderNo, result, [orderNo](const std::string &status) {
                          if (!status.empty())
                          {
                              LOG_INFO << "[ReconciliationService] Order status synced: order_no="
                                       << orderNo << ", status=" << status << ", source=alipay";
                          }
                          else
                          {
                              LOG_DEBUG << "[ReconciliationService] Order " << orderNo
                                        << " status unchanged (no sync needed)";
                          }
                      }
                    );
                }
              );
          }
      },
      [](const drogon::orm::DrogonDbException &e) {
          LOG_ERROR << "Alipay reconcile query error: " << e.base().what();
      },
      "PAYING",
      "alipay",
      reconcileBatchSize_
    );
}

void ReconciliationService::syncPendingRefunds()
{
    if (!dbClient_ || !wechatClient_)
    {
        return;
    }

    dbClient_->execSqlAsync(
      "SELECT refund_no FROM pay_refund WHERE status IN ($1, $2) "
      "ORDER BY updated_at DESC LIMIT $3",
      [this](const drogon::orm::Result &r) {
          for (const auto &row : r)
          {
              const std::string refundNo = row["refund_no"].as<std::string>();
              wechatClient_->queryRefund(
                refundNo, [this, refundNo](const Json::Value &result, const std::string &error) {
                    if (!error.empty())
                    {
                        LOG_WARN << "Wechat refund query failed for " << refundNo << ": " << error;
                        return;
                    }
                    refundService_
                      ->syncRefundStatusFromWechat(refundNo, result, [](const std::string &) {});
                }
              );
          }
      },
      [](const drogon::orm::DrogonDbException &e) {
          LOG_ERROR << "WeChat refund reconcile query error: " << e.base().what();
      },
      "REFUND_INIT",
      "REFUNDING",
      reconcileBatchSize_
    );
}
