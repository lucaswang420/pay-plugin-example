#include "RefundService.h"
#include "../models/PayRefund.h"
#include "../models/PayOrder.h"
#include "../models/PayPayment.h"
#include "../models/PayLedger.h"
#include "../models/PayIdempotency.h"
#include "../utils/PayUtils.h"
#include <drogon/drogon.h>
#include <random>
#include <sstream>
#include <iomanip>

using namespace drogon;
using namespace drogon::orm;

// Model type aliases for convenience
namespace
{
using PayRefundModel = drogon_model::pay_test::PayRefund;
using PayOrderModel = drogon_model::pay_test::PayOrder;
using PayPaymentModel = drogon_model::pay_test::PayPayment;
using PayLedgerModel = drogon_model::pay_test::PayLedger;
using PayIdempotencyModel = drogon_model::pay_test::PayIdempotency;
}  // namespace

namespace
{
// Helper functions adapted from PayPlugin.cc
void insertLedgerEntry(
  const std::shared_ptr<DbClient> &dbClient,
  int64_t userId,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &entryType,
  const std::string &amount
)
{
    if (!dbClient)
    {
        return;
    }

    auto insertRow = [dbClient, userId, orderNo, paymentNo, entryType, amount]() {
        PayLedgerModel ledger;
        ledger.setUserId(userId);
        ledger.setOrderNo(orderNo);
        if (paymentNo.empty())
        {
            ledger.setPaymentNoToNull();
        }
        else
        {
            ledger.setPaymentNo(paymentNo);
        }
        ledger.setEntryType(entryType);
        ledger.setAmount(amount);
        ledger.setCreatedAt(trantor::Date::now());

        Mapper<PayLedgerModel> ledgerMapper(dbClient);
        ledgerMapper.insert(
          ledger,
          [](const PayLedgerModel &) {},
          [](const DrogonDbException &e) {
              LOG_ERROR << "Ledger insert error: " << e.base().what();
          }
        );
    };

    if (orderNo.empty() || entryType.empty())
    {
        insertRow();
        return;
    }

    if (paymentNo.empty())
    {
        dbClient->execSqlAsync(
          "SELECT 1 FROM pay_ledger WHERE order_no = $1 "
          "AND entry_type = $2 AND payment_no IS NULL LIMIT 1",
          [insertRow](const Result &rows) {
              if (rows.empty())
              {
                  insertRow();
              }
          },
          [](const DrogonDbException &e) {
              LOG_ERROR << "Ledger lookup error: " << e.base().what();
          },
          orderNo,
          entryType
        );
        return;
    }

    dbClient->execSqlAsync(
      "SELECT 1 FROM pay_ledger WHERE order_no = $1 "
      "AND entry_type = $2 AND payment_no = $3 LIMIT 1",
      [insertRow](const Result &rows) {
          if (rows.empty())
          {
              insertRow();
          }
      },
      [](const DrogonDbException &e) { LOG_ERROR << "Ledger lookup error: " << e.base().what(); },
      orderNo,
      entryType,
      paymentNo
    );
}

void storeIdempotencySnapshot(
  const std::shared_ptr<DbClient> &dbClient,
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const std::string &responseSnapshot,
  int64_t ttlSeconds
)
{
    if (!dbClient || idempotencyKey.empty())
    {
        return;
    }

    PayIdempotencyModel idemp;
    idemp.setIdempotencyKey(idempotencyKey);
    idemp.setRequestHash(requestHash);
    idemp.setResponseSnapshot(responseSnapshot);
    const auto now = trantor::Date::now();
    const auto expiresAt =
      trantor::Date(now.microSecondsSinceEpoch() + ttlSeconds * static_cast<int64_t>(1000000));
    idemp.setExpireAt(expiresAt);

    Mapper<PayIdempotencyModel> idempMapper(dbClient);
    idempMapper.insert(
      idemp,
      [](const PayIdempotencyModel &) {},
      [](const DrogonDbException &e) {
          LOG_ERROR << "Idempotency insert error: " << e.base().what();
      }
    );
}

std::string toRfc3339Utc(const trantor::Date &when)
{
    const auto seconds = static_cast<time_t>(when.microSecondsSinceEpoch() / 1000000);
    std::tm tmUtc{};
#ifdef _WIN32
    gmtime_s(&tmUtc, &seconds);
#else
    gmtime_r(&seconds, &tmUtc);
#endif
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tmUtc) == 0)
    {
        return {};
    }
    return buffer;
}

std::string toJsonString(const Json::Value &json)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, json);
}
}  // namespace

RefundService::RefundService(
  std::shared_ptr<WechatPayClient> wechatClient,
  std::shared_ptr<AlipaySandboxClient> alipayClient,
  std::shared_ptr<drogon::orm::DbClient> dbClient,
  std::shared_ptr<IdempotencyService> idempotencyService
)
    : wechatClient_(wechatClient),
      alipayClient_(alipayClient),
      dbClient_(dbClient),
      idempotencyService_(idempotencyService)
{
}

void RefundService::createRefund(
  const CreateRefundRequest &request,
  const std::string &idempotencyKey,
  RefundCallback &&callback
)
{
    // Calculate request hash for idempotency
    Json::Value reqJson;
    reqJson["order_no"] = request.orderNo;
    reqJson["amount"] = request.amount;
    reqJson["reason"] = request.reason;
    const std::string requestStr = pay::utils::toJsonString(reqJson);

    // Use SHA-256 for cryptographic hashing (more secure than std::hash)
    std::string requestHash = drogon::utils::getSha256(requestStr);

    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    // Wrap callback to save response to idempotency cache before calling user callback
    auto wrappedCallback = [this,
                            idempotencyKey,
                            requestHash,
                            sharedCb](const Json::Value &response, const std::error_code &error) {
        LOG_INFO << "[RefundService] wrappedCallback: key=" << idempotencyKey
                 << ", error=" << error.value() << ", has_data=" << response.isMember("data");
        if (!idempotencyKey.empty() && !error && response.isMember("data"))
        {
            LOG_INFO << "[RefundService] Saving idempotency snapshot for key=" << idempotencyKey;
            // Save successful response to idempotency cache
            idempotencyService_->updateResult(idempotencyKey, requestHash, response, [](bool success) {
                if (success)
                {
                    LOG_INFO << "[RefundService] Idempotency snapshot saved successfully";
                }
                else
                {
                    LOG_WARN << "[RefundService] Failed to save idempotency snapshot";
                }
            });
        }
        // Call user callback
        if (*sharedCb)
        {
            (*sharedCb)(response, error);
        }
    };
    auto wrappedSharedCb = std::make_shared<RefundCallback>(std::move(wrappedCallback));

    // Skip idempotency check for empty key (used in tests)
    if (idempotencyKey.empty())
    {
        LOG_DEBUG << "[RefundService] Empty idempotency key, skipping check";
        proceedRefund(request, idempotencyKey, requestHash, std::move(*wrappedSharedCb));
        return;
    }

    // Check idempotency
    idempotencyService_->checkAndSet(
      idempotencyKey,
      requestHash,
      [&request]() {
          Json::Value req;
          req["order_no"] = request.orderNo;
          req["amount"] = request.amount;
          req["reason"] = request.reason;
          return req;
      }(),
      [this, request, idempotencyKey, requestHash, wrappedSharedCb](
        bool canProceed, const Json::Value &cachedResult
      ) mutable {
          if (!canProceed)
          {
              // Idempotency conflict
              if (*wrappedSharedCb)
              {
                  Json::Value error;
                  error["code"] = 1004;
                  error["message"] = "Idempotency conflict: different parameters for same key";
                  (*wrappedSharedCb)(error, std::error_code(409, std::system_category()));
              }
              return;
          }

          if (!cachedResult.isNull())
          {
              // Return cached result
              if (*wrappedSharedCb)
              {
                  (*wrappedSharedCb)(cachedResult, std::error_code());
              }
              return;
          }

          // Proceed with refund creation. Wrap the callback so that if the
          // operation fails after the key was reserved, the in-flight
          // reservation is released (preventing key poisoning on retry).
          auto proceedCb =
            [this, idempotencyKey, requestHash, wrappedSharedCb](
              const Json::Value &response, const std::error_code &error) {
                if (!idempotencyKey.empty() && error)
                {
                    idempotencyService_->clearReservation(
                      idempotencyKey,
                      requestHash,
                      [wrappedSharedCb, response, error](bool) {
                          if (*wrappedSharedCb)
                          {
                              (*wrappedSharedCb)(response, error);
                          }
                      });
                    return;
                }
                if (*wrappedSharedCb)
                {
                    (*wrappedSharedCb)(response, error);
                }
            };
          proceedRefund(request, idempotencyKey, requestHash, std::move(proceedCb));
      }
    );
}

void RefundService::proceedRefund(
  const CreateRefundRequest &request,
  const std::string &idempotencyKey,
  const std::string &requestHash,
  RefundCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    // Enhanced input validation
    // Validate reason field (only check max length, allow empty)
    if (request.reason.size() > 80)
    {
        if (*sharedCb)
        {
            Json::Value error;
            error["code"] = 1400;
            error["message"] = "reason too long (max 80 characters)";
            (*sharedCb)(error, std::error_code(1400, std::system_category()));
        }
        return;
    }

    // Validate order_no format (basic check)
    if (request.orderNo.empty() || request.orderNo.length() > 64)
    {
        if (*sharedCb)
        {
            Json::Value error;
            error["code"] = 1400;
            error["message"] = "invalid order_no format";
            (*sharedCb)(error, std::error_code(1400, std::system_category()));
        }
        return;
    }

    // Validate amount format
    if (request.amount.empty())
    {
        if (*sharedCb)
        {
            Json::Value error;
            error["code"] = 1400;
            error["message"] = "amount cannot be empty";
            (*sharedCb)(error, std::error_code(1400, std::system_category()));
        }
        return;
    }

    // Validate funds_account
    if (
      !request.fundsAccount.empty() && request.fundsAccount != "AVAILABLE" &&
      request.fundsAccount != "UNSETTLED"
    )
    {
        if (*sharedCb)
        {
            Json::Value error;
            error["code"] = 1400;
            error["message"] = "invalid funds_account (must be AVAILABLE or UNSETTLED)";
            (*sharedCb)(error, std::error_code(1400, std::system_category()));
        }
        return;
    }

    // Enhanced URL validation
    if (!request.notifyUrl.empty())
    {
        if (request.notifyUrl.find("http://") != 0 && request.notifyUrl.find("https://") != 0)
        {
            if (*sharedCb)
            {
                Json::Value error;
                error["code"] = 1400;
                error["message"] = "invalid notify_url (must start with http:// or https://)";
                (*sharedCb)(error, std::error_code(1400, std::system_category()));
            }
            return;
        }
        if (request.notifyUrl.length() > 512)
        {
            if (*sharedCb)
            {
                Json::Value error;
                error["code"] = 1400;
                error["message"] = "notify_url too long (max 512 characters)";
                (*sharedCb)(error, std::error_code(1400, std::system_category()));
            }
            return;
        }
    }

    const std::string refundNo =
      request.refundNo.empty() ? drogon::utils::getUuid() : request.refundNo;
    const std::string &orderNo = request.orderNo;
    const std::string &amount = request.amount;
    const std::string &reason = request.reason;
    const std::string &paymentNo = request.paymentNo;

    // If paymentNo not provided, find the latest payment for this order
    if (paymentNo.empty())
    {
        Mapper<PayPaymentModel> paymentMapper(dbClient_);
        auto payCriteria = Criteria(PayPaymentModel::Cols::_order_no, CompareOperator::EQ, orderNo);
        paymentMapper.orderBy(PayPaymentModel::Cols::_created_at, SortOrder::DESC)
          .limit(1)
          .findBy(
            payCriteria,
            [this,
             request,
             idempotencyKey,
             requestHash,
             refundNo,
             orderNo,
             amount,
             reason,
             sharedCb](const std::vector<PayPaymentModel> &rows) mutable {
                if (rows.empty())
                {
                    if (*sharedCb)
                    {
                        Json::Value error;
                        error["code"] = 1404;
                        error["message"] = "Payment not found";
                        (*sharedCb)(error, std::error_code(1404, std::system_category()));
                    }
                    return;
                }
                CreateRefundRequest newRequest = request;
                newRequest.paymentNo = rows.front().getValueOfPaymentNo();
                proceedRefund(newRequest, idempotencyKey, requestHash, std::move(*sharedCb));
            },
            [sharedCb](const DrogonDbException &e) mutable {
                if (*sharedCb)
                {
                    Json::Value error;
                    error["code"] = 1500;
                    error["message"] = std::string("Database error: ") + e.base().what();
                    (*sharedCb)(error, std::error_code(1500, std::system_category()));
                }
            }
          );
        return;
    }

    // Validate payment exists and is successful
    Mapper<PayPaymentModel> paymentValidateMapper(dbClient_);
    auto paymentCriteria =
      Criteria(PayPaymentModel::Cols::_payment_no, CompareOperator::EQ, paymentNo) &&
      Criteria(PayPaymentModel::Cols::_order_no, CompareOperator::EQ, orderNo);
    paymentValidateMapper.findOne(
      paymentCriteria,
      [this,
       request,
       idempotencyKey,
       requestHash,
       refundNo,
       orderNo,
       paymentNo,
       amount,
       reason,
       sharedCb](const PayPaymentModel &payment) mutable {
          if (payment.getValueOfStatus() != "SUCCESS")
          {
              if (*sharedCb)
              {
                  Json::Value error;
                  error["code"] = 1409;
                  error["message"] = "payment not successful";
                  (*sharedCb)(error, std::error_code(1409, std::system_category()));
              }
              return;
          }
          proceedOrderFlow(
            request,
            idempotencyKey,
            requestHash,
            refundNo,
            orderNo,
            paymentNo,
            amount,
            reason,
            std::move(*sharedCb)
          );
      },
      [sharedCb](const DrogonDbException &e) mutable {
          if (*sharedCb)
          {
              Json::Value error;
              error["code"] = 1404;
              error["message"] = std::string("Payment not found: ") + e.base().what();
              (*sharedCb)(error, std::error_code(1404, std::system_category()));
          }
      }
    );
}

void RefundService::proceedOrderFlow(
  const CreateRefundRequest &request,
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const std::string &refundNo,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &amount,
  const std::string &reason,
  RefundCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    Mapper<PayOrderModel> orderMapper(dbClient_);
    auto criteria = Criteria(PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo);
    orderMapper.findOne(
      criteria,
      [this,
       request,
       idempotencyKey,
       requestHash,
       refundNo,
       orderNo,
       paymentNo,
       amount,
       reason,
       sharedCb](const PayOrderModel &order) mutable {
          const std::string orderAmount = order.getValueOfAmount();
          const std::string currency = order.getValueOfCurrency();
          const std::string orderStatus = order.getValueOfStatus();

          if (orderStatus != "PAID")
          {
              if (*sharedCb)
              {
                  Json::Value error;
                  error["code"] = 1409;
                  error["message"] = "order not paid";
                  (*sharedCb)(error, std::error_code(1409, std::system_category()));
              }
              return;
          }

          int64_t refundFen = 0;
          int64_t totalFen = 0;
          if (
            !pay::utils::parseAmountToFen(amount, refundFen) ||
            !pay::utils::parseAmountToFen(orderAmount, totalFen)
          )
          {
              if (*sharedCb)
              {
                  Json::Value error;
                  error["code"] = 1400;
                  error["message"] = "Invalid amount format";
                  (*sharedCb)(error, std::error_code(1400, std::system_category()));
              }
              return;
          }
          if (refundFen <= 0 || refundFen > totalFen)
          {
              if (*sharedCb)
              {
                  Json::Value error;
                  error["code"] = 1400;
                  error["message"] = "Invalid refund amount";
                  (*sharedCb)(error, std::error_code(1400, std::system_category()));
              }
              return;
          }

          proceedWithAmountCheck(
            request,
            idempotencyKey,
            requestHash,
            refundNo,
            orderNo,
            paymentNo,
            amount,
            refundFen,
            totalFen,
            currency,
            reason,
            std::move(*sharedCb)
          );
      },
      [sharedCb](const DrogonDbException &e) mutable {
          if (*sharedCb)
          {
              Json::Value error;
              error["code"] = 1404;
              error["message"] = std::string("Order not found: ") + e.base().what();
              (*sharedCb)(error, std::error_code(1404, std::system_category()));
          }
      }
    );
}

void RefundService::proceedWithAmountCheck(
  const CreateRefundRequest &request,
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const std::string &refundNo,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &amount,
  int64_t refundFen,
  int64_t totalFen,
  const std::string &currency,
  const std::string &reason,
  RefundCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    // Check for already successful refund with same details
    dbClient_->execSqlAsync(
      "SELECT refund_no, order_no, status, channel_refund_no "
      "FROM pay_refund "
      "WHERE order_no = $1 AND payment_no = $2 AND amount = $3 AND status = $4 "
      "ORDER BY updated_at DESC LIMIT 1",
      [this,
       request,
       idempotencyKey,
       requestHash,
       refundNo,
       orderNo,
       paymentNo,
       amount,
       refundFen,
       totalFen,
       currency,
       reason,
       sharedCb](const Result &r) mutable {
          if (!r.empty())
          {
              if (*sharedCb)
              {
                  Json::Value response;
                  response["code"] = 0;
                  response["message"] = "Refund already successful";
                  Json::Value data;
                  data["refund_no"] = r.front()["refund_no"].as<std::string>();
                  data["order_no"] = r.front()["order_no"].as<std::string>();
                  data["payment_no"] = paymentNo;
                  data["refund_amount"] = amount;
                  data["status"] = r.front()["status"].as<std::string>();
                  if (!r.front()["channel_refund_no"].isNull())
                  {
                      data["channel_refund_no"] = r.front()["channel_refund_no"].as<std::string>();
                  }
                  response["data"] = data;
                  (*sharedCb)(response, std::error_code());
              }
              return;
          }
          proceedWithInProgressCheck(
            request,
            idempotencyKey,
            requestHash,
            refundNo,
            orderNo,
            paymentNo,
            amount,
            refundFen,
            totalFen,
            currency,
            reason,
            std::move(*sharedCb)
          );
      },
      [sharedCb](const DrogonDbException &e) {
          if (*sharedCb)
          {
              Json::Value error;
              error["code"] = 1500;
              error["message"] = std::string("Database error: ") + e.base().what();
              (*sharedCb)(error, std::error_code(1500, std::system_category()));
          }
      },
      orderNo,
      paymentNo,
      amount,
      "REFUND_SUCCESS"
    );
}

void RefundService::proceedWithInProgressCheck(
  const CreateRefundRequest &request,
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const std::string &refundNo,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &amount,
  int64_t refundFen,
  int64_t totalFen,
  const std::string &currency,
  const std::string &reason,
  RefundCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    // Check for refund already in progress
    dbClient_->execSqlAsync(
      "SELECT COUNT(*) AS cnt FROM pay_refund "
      "WHERE order_no = $1 AND payment_no = $2 AND amount = $3 "
      "AND status IN ($4, $5)",
      [this,
       request,
       idempotencyKey,
       requestHash,
       refundNo,
       orderNo,
       paymentNo,
       amount,
       refundFen,
       totalFen,
       currency,
       reason,
       sharedCb](const Result &r) mutable {
          if (!r.empty() && r.front()["cnt"].as<int64_t>() > 0)
          {
              if (*sharedCb)
              {
                  Json::Value error;
                  error["code"] = 1409;
                  error["message"] = "refund already in progress";
                  (*sharedCb)(error, std::error_code(1409, std::system_category()));
              }
              return;
          }
          proceedWithInsert(
            request,
            idempotencyKey,
            requestHash,
            refundNo,
            orderNo,
            paymentNo,
            amount,
            refundFen,
            totalFen,
            currency,
            reason,
            std::move(*sharedCb)
          );
      },
      [sharedCb](const DrogonDbException &e) {
          if (*sharedCb)
          {
              Json::Value error;
              error["code"] = 1500;
              error["message"] = std::string("Database error: ") + e.base().what();
              (*sharedCb)(error, std::error_code(1500, std::system_category()));
          }
      },
      orderNo,
      paymentNo,
      amount,
      "REFUND_INIT",
      "REFUNDING"
    );
}

void RefundService::proceedWithInsert(
  const CreateRefundRequest &request,
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const std::string &refundNo,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &amount,
  int64_t refundFen,
  int64_t totalFen,
  const std::string &currency,
  const std::string &reason,
  RefundCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    // Check total refunded amount doesn't exceed paid amount
    dbClient_->execSqlAsync(
      "SELECT COALESCE(SUM(CAST(amount AS NUMERIC)), 0) AS sum_amount "
      "FROM pay_refund WHERE order_no = $1 "
      "AND status IN ($2, $3, $4)",
      [this,
       request,
       idempotencyKey,
       requestHash,
       refundNo,
       orderNo,
       paymentNo,
       amount,
       refundFen,
       totalFen,
       currency,
       reason,
       sharedCb](const Result &r) mutable {
          if (r.empty())
          {
              proceedWithRefundInsert(
                request,
                idempotencyKey,
                requestHash,
                refundNo,
                orderNo,
                paymentNo,
                amount,
                refundFen,
                totalFen,
                currency,
                reason,
                std::move(*sharedCb)
              );
              return;
          }
          const auto sumText = r.front()["sum_amount"].as<std::string>();
          int64_t refundedFen = 0;
          if (!pay::utils::parseAmountToFen(sumText, refundedFen))
          {
              if (*sharedCb)
              {
                  Json::Value error;
                  error["code"] = 1500;
                  error["message"] = "Invalid refund sum";
                  (*sharedCb)(error, std::error_code(1500, std::system_category()));
              }
              return;
          }
          if (refundedFen + refundFen > totalFen)
          {
              if (*sharedCb)
              {
                  Json::Value error;
                  error["code"] = 409;
                  error["message"] = "refund amount exceeds paid";
                  (*sharedCb)(error, std::error_code(409, std::system_category()));
              }
              return;
          }
          proceedWithRefundInsert(
            request,
            idempotencyKey,
            requestHash,
            refundNo,
            orderNo,
            paymentNo,
            amount,
            refundFen,
            totalFen,
            currency,
            reason,
            std::move(*sharedCb)
          );
      },
      [sharedCb](const DrogonDbException &e) {
          if (*sharedCb)
          {
              Json::Value error;
              error["code"] = 1500;
              error["message"] = std::string("Database error: ") + e.base().what();
              (*sharedCb)(error, std::error_code(1500, std::system_category()));
          }
      },
      orderNo,
      "REFUND_INIT",
      "REFUNDING",
      "REFUND_SUCCESS"
    );
}

void RefundService::proceedWithRefundInsert(
  const CreateRefundRequest &request,
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const std::string &refundNo,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &amount,
  int64_t refundFen,
  int64_t totalFen,
  const std::string &currency,
  const std::string &reason,
  RefundCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    // First, query the order to get the payment channel
    Mapper<PayOrderModel> orderMapper(dbClient_);
    auto orderCriteria = Criteria(PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo);
    orderMapper.findOne(
      orderCriteria,
      [this,
       request,
       refundNo,
       orderNo,
       paymentNo,
       amount,
       refundFen,
       totalFen,
       currency,
       reason,
       idempotencyKey,
       requestHash,
       sharedCb](const PayOrderModel &order) mutable {
          std::string channel = order.getValueOfChannel();
          LOG_INFO << "[RefundService] Processing refund: order_no=" << orderNo
                   << ", payment_no=" << paymentNo << ", refund_no=" << refundNo
                   << ", channel=" << channel << ", amount=" << amount;

          // Insert refund record
          Mapper<PayRefundModel> refundMapper(dbClient_);
          PayRefundModel refund;
          refund.setRefundNo(refundNo);
          refund.setOrderNo(orderNo);
          refund.setPaymentNo(paymentNo);
          refund.setStatus("REFUND_INIT");
          refund.setAmount(amount);
          refund.setCreatedAt(trantor::Date::now());
          refundMapper.insert(
            refund,
            [this,
             channel,
             request,
             idempotencyKey,
             requestHash,
             refundNo,
             orderNo,
             paymentNo,
             amount,
             refundFen,
             totalFen,
             currency,
             reason,
             sharedCb](const PayRefundModel &) mutable {
                // Route to appropriate payment client based on channel
                if (channel == "alipay")
                {
                    // Alipay refund
                    if (!alipayClient_)
                    {
                        if (*sharedCb)
                        {
                            Json::Value error;
                            error["code"] = 1501;
                            error["message"] = "Alipay client not ready";
                            error["data"]["refund_no"] = refundNo;
                            error["data"]["order_no"] = orderNo;
                            error["data"]["payment_no"] = paymentNo;
                            error["data"]["refund_amount"] = amount;
                            error["data"]["status"] = "REFUND_FAIL";
                            (*sharedCb)(error, std::error_code(1501, std::system_category()));
                        }
                        return;
                    }

                    Json::Value payload;
                    payload["out_trade_no"] = orderNo;
                    payload["refund_amount"] = amount;
                    if (!reason.empty())
                    {
                        payload["refund_reason"] = reason;
                    }

                    alipayClient_->refund(
                      payload,
                      [this, refundNo, orderNo, paymentNo, amount, sharedCb](
                        const Json::Value &result, const std::string &error
                      ) mutable {
                          if (!error.empty())
                          {
                              const std::string errorMessage = "Alipay error: " + error;
                              Json::Value errJson;
                              errJson["error"] = errorMessage;
                              updateRefundWithError(refundNo, errorMessage, errJson);
                              if (*sharedCb)
                              {
                                  Json::Value response;
                                  response["code"] = 1502;
                                  response["message"] = errorMessage;
                                  response["data"]["refund_no"] = refundNo;
                                  response["data"]["order_no"] = orderNo;
                                  response["data"]["payment_no"] = paymentNo;
                                  response["data"]["refund_amount"] = amount;
                                  response["data"]["status"] = "REFUND_FAIL";
                                  response["data"]["error"] = errorMessage;
                                  response["data"]["alipay_response"] = errJson;
                                  (*sharedCb)(
                                    response, std::error_code(1502, std::system_category())
                                  );
                              }
                              return;
                          }

                          // Check Alipay response
                          std::string alipayCode = result.get("code", "").asString();
                          if (alipayCode != "10000")
                          {
                              const std::string errorMessage =
                                "Alipay refund failed: " + result.get("msg", "").asString();
                              updateRefundWithError(refundNo, errorMessage, result);
                              if (*sharedCb)
                              {
                                  Json::Value response;
                                  response["code"] = 1502;
                                  response["message"] = errorMessage;
                                  response["data"]["refund_no"] = refundNo;
                                  response["data"]["order_no"] = orderNo;
                                  response["data"]["payment_no"] = paymentNo;
                                  response["data"]["refund_amount"] = amount;
                                  response["data"]["status"] = "REFUND_FAIL";
                                  (*sharedCb)(
                                    response, std::error_code(1502, std::system_category())
                                  );
                              }
                              return;
                          }

                          std::string refundStatus = "REFUND_SUCCESS";
                          const std::string refundId = result.get("refund_id", "").asString();
                          updateRefundWithSuccess(
                            refundNo,
                            refundStatus,
                            refundId,
                            result,
                            orderNo,
                            paymentNo,
                            amount,
                            std::move(*sharedCb)
                          );
                      }
                    );
                }
                else if (channel == "wechat")
                {
                    // WeChat refund
                    if (!wechatClient_)
                    {
                        const std::string errMsg = "wechat client not ready";
                        Json::Value errJson;
                        errJson["error"] = errMsg;
                        updateRefundWithError(refundNo, errMsg, errJson);
                        if (*sharedCb)
                        {
                            Json::Value response;
                            response["code"] = 0;
                            response["message"] = "ok";
                            response["data"]["refund_no"] = refundNo;
                            response["data"]["order_no"] = orderNo;
                            response["data"]["payment_no"] = paymentNo;
                            response["data"]["amount"] = amount;
                            response["data"]["status"] = "REFUND_FAIL";
                            response["data"]["error"] = errMsg;
                            (*sharedCb)(response, std::error_code());
                        }
                        return;
                    }

                    Json::Value payload;
                    payload["out_trade_no"] = orderNo;
                    payload["out_refund_no"] = refundNo;
                    if (!reason.empty())
                    {
                        payload["reason"] = reason;
                    }
                    if (!request.notifyUrl.empty())
                    {
                        payload["notify_url"] = request.notifyUrl;
                    }
                    if (!request.fundsAccount.empty())
                    {
                        payload["funds_account"] = request.fundsAccount;
                    }
                    payload["amount"]["refund"] = static_cast<Json::Int64>(refundFen);
                    payload["amount"]["total"] = static_cast<Json::Int64>(totalFen);
                    payload["amount"]["currency"] = currency;

                    wechatClient_->refund(
                      payload,
                      [this, refundNo, orderNo, paymentNo, amount, sharedCb](
                        const Json::Value &result, const std::string &error
                      ) mutable {
                          if (!error.empty())
                          {
                              const std::string errorMessage = "WeChat error: " + error;
                              Json::Value errJson;
                              errJson["error"] = errorMessage;
                              updateRefundWithError(refundNo, errorMessage, errJson);
                              if (*sharedCb)
                              {
                                  Json::Value response;
                                  response["code"] = 0;
                                  response["message"] = "Refund created with error status";
                                  response["data"]["refund_no"] = refundNo;
                                  response["data"]["order_no"] = orderNo;
                                  response["data"]["payment_no"] = paymentNo;
                                  response["data"]["amount"] = amount;
                                  response["data"]["status"] = "REFUND_FAIL";
                                  response["data"]["error"] = errorMessage;
                                  response["data"]["wechat_response"] = errJson;
                                  (*sharedCb)(response, std::error_code());
                              }
                              return;
                          }

                          std::string refundStatus = "REFUNDING";
                          const std::string wechatStatus = result.get("status", "").asString();
                          const std::string refundId = result.get("refund_id", "").asString();
                          if (wechatStatus == "SUCCESS")
                          {
                              refundStatus = "REFUND_SUCCESS";
                          }
                          else if (wechatStatus == "CLOSED")
                          {
                              refundStatus = "REFUND_FAIL";
                          }

                          updateRefundWithSuccess(
                            refundNo,
                            refundStatus,
                            refundId,
                            result,
                            orderNo,
                            paymentNo,
                            amount,
                            std::move(*sharedCb)
                          );
                      }
                    );
                }
                else
                {
                    // Unknown channel
                    if (*sharedCb)
                    {
                        Json::Value error;
                        error["code"] = 1500;
                        error["message"] = "Unknown payment channel: " + channel;
                        error["data"]["refund_no"] = refundNo;
                        error["data"]["order_no"] = orderNo;
                        error["data"]["payment_no"] = paymentNo;
                        error["data"]["refund_amount"] = amount;
                        error["data"]["status"] = "REFUND_FAIL";
                        (*sharedCb)(error, std::error_code(1500, std::system_category()));
                    }
                }
            },
            [sharedCb](const DrogonDbException &e) {
                if (*sharedCb)
                {
                    Json::Value error;
                    error["code"] = 1500;
                    error["message"] = std::string("Database error: ") + e.base().what();
                    (*sharedCb)(error, std::error_code(1500, std::system_category()));
                }
            }
          );
      },
      [sharedCb](const DrogonDbException &e) {
          if (*sharedCb)
          {
              Json::Value error;
              error["code"] = 1500;
              error["message"] = std::string("Database error: ") + e.base().what();
              (*sharedCb)(error, std::error_code(1500, std::system_category()));
          }
      }
    );
}

void RefundService::updateRefundWithError(
  const std::string &refundNo,
  const std::string &errorMessage,
  const Json::Value &errJson
)
{
    Mapper<PayRefundModel> refundMapper(dbClient_);
    auto criteria = Criteria(PayRefundModel::Cols::_refund_no, CompareOperator::EQ, refundNo);
    refundMapper.findOne(
      criteria,
      [this, errorMessage, errJson, refundNo](PayRefundModel refund) {
          refund.setStatus("REFUND_FAIL");
          Mapper<PayRefundModel> refundUpdater(dbClient_);
          refundUpdater.update(
            refund,
            [this, errJson, refundNo](const size_t) {
                dbClient_->execSqlAsync(
                  "UPDATE pay_refund SET response_payload = $1 "
                  "WHERE refund_no = $2",
                  [](const Result &) {},
                  [](const DrogonDbException &e) {
                      LOG_WARN << "Refund error payload update error: " << e.base().what();
                  },
                  toJsonString(errJson),
                  refundNo
                );
            },
            [](const DrogonDbException &) {}
          );
      },
      [](const DrogonDbException &) {}
    );
}

void RefundService::updateRefundWithSuccess(
  const std::string &refundNo,
  const std::string &refundStatus,
  const std::string &refundId,
  const Json::Value &result,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &amount,
  RefundCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    Mapper<PayRefundModel> refundMapper(dbClient_);
    auto criteria = Criteria(PayRefundModel::Cols::_refund_no, CompareOperator::EQ, refundNo);
    refundMapper.findOne(
      criteria,
      [this, refundNo, refundStatus, refundId, result, orderNo, paymentNo, amount, sharedCb](
        PayRefundModel refund
      ) mutable {
          refund.setStatus(refundStatus);
          refund.setChannelRefundNo(refundId);
          Mapper<PayRefundModel> refundUpdater(dbClient_);
          refundUpdater.update(
            refund,
            [this, refundNo, refundStatus, refundId, result, orderNo, paymentNo, amount, sharedCb](
              const size_t
            ) {
                dbClient_->execSqlAsync(
                  "UPDATE pay_refund SET response_payload = $1 "
                  "WHERE refund_no = $2",
                  [this,
                   refundNo,
                   refundStatus,
                   refundId,
                   result,
                   orderNo,
                   paymentNo,
                   amount,
                   sharedCb](const Result &) {
                      // Update order status to REFUNDED after successful refund
                      Mapper<PayOrderModel> orderMapper(dbClient_);
                      auto orderCriteria =
                        Criteria(PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo);
                      orderMapper.findOne(
                        orderCriteria,
                        [this,
                         refundNo,
                         refundStatus,
                         refundId,
                         result,
                         orderNo,
                         paymentNo,
                         amount,
                         sharedCb](PayOrderModel order) mutable {
                            order.setStatus("REFUNDED");
                            Mapper<PayOrderModel> orderUpdater(dbClient_);
                            orderUpdater.update(
                              order,
                              [this,
                               refundNo,
                               refundStatus,
                               refundId,
                               result,
                               orderNo,
                               paymentNo,
                               amount,
                               sharedCb](const size_t) {
                                  LOG_INFO
                                    << "[RefundService] Refund completed: refund_no=" << refundNo
                                    << ", order_no=" << orderNo << ", status=" << refundStatus
                                    << ", amount=" << amount;
                                  if (*sharedCb)
                                  {
                                      Json::Value response;
                                      response["code"] = 0;
                                      response["message"] = "Refund created successfully";
                                      Json::Value data;
                                      data["refund_no"] = refundNo;
                                      data["order_no"] = orderNo;
                                      data["payment_no"] = paymentNo;
                                      data["refund_amount"] = amount;
                                      data["status"] = refundStatus;
                                      data["channel_refund_no"] = refundId;
                                      data["wechat_response"] = result;
                                      response["data"] = data;
                                      (*sharedCb)(response, std::error_code());
                                  }
                              },
                              [sharedCb](const DrogonDbException &e) {
                                  if (*sharedCb)
                                  {
                                      Json::Value error;
                                      error["code"] = 1500;
                                      error["message"] =
                                        std::string("Database error: ") + e.base().what();
                                      (*sharedCb)(
                                        error, std::error_code(1500, std::system_category())
                                      );
                                  }
                              }
                            );
                        },
                        [sharedCb](const DrogonDbException &e) {
                            if (*sharedCb)
                            {
                                Json::Value error;
                                error["code"] = 1500;
                                error["message"] =
                                  std::string("Database error: ") + e.base().what();
                                (*sharedCb)(error, std::error_code(1500, std::system_category()));
                            }
                        }
                      );
                  },
                  [sharedCb](const DrogonDbException &e) {
                      if (*sharedCb)
                      {
                          Json::Value error;
                          error["code"] = 1500;
                          error["message"] = std::string("Database error: ") + e.base().what();
                          (*sharedCb)(error, std::error_code(1500, std::system_category()));
                      }
                  },
                  toJsonString(result),
                  refundNo
                );
            },
            [sharedCb](const DrogonDbException &e) {
                if (*sharedCb)
                {
                    Json::Value error;
                    error["code"] = 1500;
                    error["message"] = std::string("Database error: ") + e.base().what();
                    (*sharedCb)(error, std::error_code(1500, std::system_category()));
                }
            }
          );
      },
      [sharedCb](const DrogonDbException &e) {
          if (*sharedCb)
          {
              Json::Value error;
              error["code"] = 1500;
              error["message"] = std::string("Database error: ") + e.base().what();
              (*sharedCb)(error, std::error_code(1500, std::system_category()));
          }
      }
    );
}

void RefundService::queryRefund(const std::string &refundNo, RefundCallback &&callback)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<RefundCallback>(std::move(callback));

    Mapper<PayRefundModel> refundMapper(dbClient_);
    auto criteria = Criteria(PayRefundModel::Cols::_refund_no, CompareOperator::EQ, refundNo);
    refundMapper.findOne(
      criteria,
      [this, refundNo, sharedCb](const PayRefundModel &refund) {
          Json::Value response;
          response["code"] = 0;
          response["message"] = "Query refund successful";
          Json::Value data;
          data["refund_no"] = refund.getValueOfRefundNo();
          data["order_no"] = refund.getValueOfOrderNo();
          data["payment_no"] = refund.getValueOfPaymentNo();
          data["status"] = refund.getValueOfStatus();
          data["refund_amount"] = refund.getValueOfAmount();
          data["channel_refund_no"] = refund.getValueOfChannelRefundNo();
          data["updated_at"] = toRfc3339Utc(refund.getValueOfUpdatedAt());
          response["data"] = data;

          if (!wechatClient_)
          {
              if (*sharedCb)
              {
                  (*sharedCb)(response, std::error_code());
              }
              return;
          }

          wechatClient_->queryRefund(
            refundNo,
            [this,
             refundNo,
             response,
             sharedCb](const Json::Value &result, const std::string &error) mutable {
                if (!error.empty())
                {
                    if (*sharedCb)
                    {
                        (*sharedCb)(response, std::error_code());
                    }
                    return;
                }

                syncRefundStatusFromWechat(
                  refundNo,
                  result,
                  [response, result, sharedCb](const std::string &status) mutable {
                      if (!status.empty())
                      {
                          response["data"]["status"] = status;
                      }
                      response["data"]["wechat_response"] = result;
                      if (*sharedCb)
                      {
                          (*sharedCb)(response, std::error_code());
                      }
                  }
                );
            }
          );
      },
      [sharedCb](const DrogonDbException &e) {
          if (*sharedCb)
          {
              Json::Value error;
              error["code"] = 1404;
              error["message"] = std::string("Refund not found: ") + e.base().what();
              (*sharedCb)(error, std::error_code(1404, std::system_category()));
          }
      }
    );
}

void RefundService::syncRefundStatusFromWechat(
  const std::string &refundNo,
  const Json::Value &result,
  std::function<void(const std::string &status)> &&callback
)
{
    const std::string wechatStatus = result.get("status", "").asString();
    if (wechatStatus.empty())
    {
        if (callback)
        {
            callback("");
        }
        return;
    }

    const std::string refundStatus = pay::utils::mapRefundStatus(wechatStatus);
    const std::string refundId = result.get("refund_id", "").asString();

    LOG_INFO << "Sync refund status from WeChat: refund_no=" << refundNo
             << " wechat_status=" << wechatStatus << " refund_status=" << refundStatus;

    if (refundStatus.empty())
    {
        LOG_WARN << "Unknown refund status from WeChat: " << wechatStatus;
        if (callback)
        {
            callback("");
        }
        return;
    }

    if (!dbClient_)
    {
        if (callback)
        {
            callback(refundStatus);
        }
        return;
    }

    Mapper<PayRefundModel> refundMapper(dbClient_);
    auto criteria = Criteria(PayRefundModel::Cols::_refund_no, CompareOperator::EQ, refundNo);
    refundMapper.findOne(
      criteria,
      [this, refundStatus, refundId, refundNo, result, callback](PayRefundModel refund) {
          if (refund.getValueOfStatus() == "REFUND_SUCCESS")
          {
              if (callback)
              {
                  callback(refundStatus);
              }
              return;
          }
          const auto orderNo = refund.getValueOfOrderNo();
          const auto paymentNo = refund.getValueOfPaymentNo();
          const auto refundAmount = refund.getValueOfAmount();

          dbClient_->newTransactionAsync([this,
                                          refundStatus,
                                          refundId,
                                          refundNo,
                                          result,
                                          callback,
                                          refund,
                                          orderNo,
                                          paymentNo,
                                          refundAmount](
                                           const std::shared_ptr<Transaction> &transPtr
                                         ) mutable {
              auto rollbackDone = [callback, refundStatus, transPtr](const DrogonDbException &e) {
                  LOG_ERROR << "Reconcile refund update error: " << e.base().what();
                  transPtr->rollback();
                  if (callback)
                  {
                      callback(refundStatus);
                  }
              };

              auto transDb = std::static_pointer_cast<DbClient>(transPtr);

              refund.setStatus(refundStatus);
              refund.setChannelRefundNo(refundId);
              Mapper<PayRefundModel> refundUpdater(transPtr);
              refundUpdater.update(
                refund,
                [this,
                 refundStatus,
                 orderNo,
                 paymentNo,
                 refundAmount,
                 refundNo,
                 result,
                 transPtr,
                 transDb,
                 callback](const size_t) {
                    const std::string responsePayload = toJsonString(result);
                    transPtr->execSqlAsync(
                      "UPDATE pay_refund SET response_payload = $1 "
                      "WHERE refund_no = $2",
                      [refundStatus, transPtr](const Result &) {},
                      [callback, refundStatus, transPtr](const DrogonDbException &e) {
                          LOG_ERROR << "Reconcile refund payload update error: " << e.base().what();
                          transPtr->rollback();
                          if (callback)
                          {
                              callback(refundStatus);
                          }
                      },
                      responsePayload,
                      refundNo
                    );
                    if (refundStatus == "REFUND_SUCCESS")
                    {
                        Mapper<PayOrderModel> orderMapper(transPtr);
                        auto orderCriteria =
                          Criteria(PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo);
                        orderMapper.findOne(
                          orderCriteria,
                          [orderNo, paymentNo, refundAmount, transDb](const PayOrderModel &order) {
                              insertLedgerEntry(
                                transDb,
                                order.getValueOfUserId(),
                                orderNo,
                                paymentNo,
                                "REFUND",
                                refundAmount
                              );
                          },
                          [callback, refundStatus, transPtr](const DrogonDbException &e) {
                              LOG_ERROR << "Refund ledger order lookup error: " << e.base().what();
                              transPtr->rollback();
                              if (callback)
                              {
                                  callback(refundStatus);
                              }
                          }
                        );
                    }
                },
                rollbackDone
              );
          });
      },
      [callback](const DrogonDbException &e) {
          LOG_ERROR << "Refund lookup error during sync: " << e.base().what();
          if (callback)
          {
              callback("");
          }
      }
    );
}
