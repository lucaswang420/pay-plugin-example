#include "PaymentService.h"
#include "PayErrorCategory.h"
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
}  // namespace

PaymentService::PaymentService(
  std::shared_ptr<WechatPayClient> wechatClient,
  std::shared_ptr<AlipaySandboxClient> alipayClient,
  std::shared_ptr<DbClient> dbClient,
  nosql::RedisClientPtr redisClient,
  std::shared_ptr<IdempotencyService> idempotencyService
)
    : wechatClient_(wechatClient),
      alipayClient_(alipayClient),
      dbClient_(dbClient),
      redisClient_(redisClient),
      idempotencyService_(idempotencyService)
{
}

void PaymentService::createPayment(
  const CreatePaymentRequest &request,
  const std::string &idempotencyKey,
  PaymentCallback &&callback
)
{
    // Calculate request hash for idempotency
    Json::Value reqJson;
    reqJson["order_no"] = request.orderNo;
    reqJson["amount"] = request.amount;
    reqJson["currency"] = request.currency;
    reqJson["description"] = request.description;
    const std::string requestStr = pay::utils::toJsonString(reqJson);

    // Use SHA-256 for cryptographic hashing (more secure than std::hash)
    std::string requestHash = drogon::utils::getSha256(requestStr);

    // Check idempotency
    idempotencyService_->checkAndSet(
      idempotencyKey,
      requestHash,
      [&request]() {
          Json::Value req;
          req["order_no"] = request.orderNo;
          req["amount"] = request.amount;
          return req;
      }(),
      [this,
       request,
       idempotencyKey,
       callback](bool canProceed, const Json::Value &cachedResult) mutable {
          if (!canProceed)
          {
              // Idempotency conflict
              Json::Value error;
              error["code"] = 1004;
              error["message"] = "Idempotency conflict: different parameters for same key";
              callback(error, pay::makePayError(1004, "idempotency key conflict"));
              return;
          }

          if (!cachedResult.isNull())
          {
              // Return cached result
              callback(cachedResult, std::error_code());
              return;
          }

          // Proceed with payment creation
          std::string paymentNo = generatePaymentNo();
          int64_t totalFen = 0;
          if (!pay::utils::parseAmountToFen(request.amount, totalFen))
          {
              Json::Value error;
              error["code"] = 1001;
              error["message"] = "Invalid amount format";
              callback(error, pay::makePayError(1001, "Invalid amount format"));
              return;
          }

          proceedCreatePayment(request, paymentNo, totalFen, std::move(callback));
      }
    );
}

void PaymentService::proceedCreatePayment(
  const CreatePaymentRequest &request,
  const std::string &paymentNo,
  int64_t totalFen,
  PaymentCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<PaymentCallback>(std::move(callback));

    // Create order record in database
    Mapper<PayOrderModel> orderMapper(dbClient_);
    PayOrderModel order;
    order.setOrderNo(request.orderNo);
    order.setUserId(request.userId);
    order.setAmount(request.amount);
    order.setCurrency(request.currency);
    order.setStatus("CREATED");
    order.setChannel(request.channel);
    order.setTitle(request.description);
    order.setCreatedAt(trantor::Date::now());
    // Parse and set expire_at if timeExpire is provided
    if (!request.timeExpire.empty())
    {
        try
        {
            // Parse RFC 3339 format (e.g., "2026-05-07T12:34:56+08:00")
            // trantor::Date can parse ISO 8601 format
            trantor::Date expireDate = trantor::Date::fromDbStringLocal(request.timeExpire);
            order.setExpireAt(expireDate);
        }
        catch (const std::exception &e)
        {
            LOG_WARN << "Failed to parse timeExpire '" << request.timeExpire << "': " << e.what();
            // Continue without setting expire_at
        }
    }

    // Build payment request payload based on channel
    Json::Value payload;

    if (request.channel == "alipay")
    {
        // Alipay API format
        // Convert fen to yuan for Alipay (string format)
        // Use integer arithmetic to avoid floating point precision issues
        const int64_t yuan = totalFen / 100;
        const int64_t cents = totalFen % 100;
        std::ostringstream yuanStream;
        yuanStream << yuan << "." << (cents < 10 ? "0" : "") << cents;
        const std::string totalAmountYuan = yuanStream.str();

        payload["total_amount"] = totalAmountYuan;
        payload["subject"] = request.description;  // Alipay uses 'subject' instead of 'description'
        payload["out_trade_no"] = request.orderNo;

        // Add buyer_id for sandbox testing
        const char *buyerIdEnv = std::getenv("ALIPAY_SANDBOX_BUYER_ID");
        if (buyerIdEnv && strlen(buyerIdEnv) > 0)
        {
            payload["buyer_id"] = std::string(buyerIdEnv);
        }

        if (!request.notifyUrl.empty())
        {
            payload["notify_url"] = request.notifyUrl;
        }
    }
    else
    {
        // WeChat Pay API format (original format)
        payload["description"] = request.description;
        payload["out_trade_no"] = request.orderNo;
        payload["amount"]["total"] = static_cast<Json::Int64>(totalFen);
        payload["amount"]["currency"] = request.currency;

        if (!request.notifyUrl.empty())
        {
            payload["notify_url"] = request.notifyUrl;
        }

        if (!request.sceneInfo.isNull())
        {
            payload["scene_info"] = request.sceneInfo;
        }

        // Add time_expire if provided
        if (!request.timeExpire.empty())
        {
            payload["time_expire"] = request.timeExpire;
        }

        // Add attach if provided
        if (!request.attach.empty())
        {
            payload["attach"] = request.attach;
        }
    }

    const std::string requestPayload = pay::utils::toJsonString(payload);

    // Insert order into database
    try
    {
        orderMapper.insert(
          order,
          [this, request, paymentNo, payload, requestPayload, sharedCb](const PayOrderModel &) {
              LOG_INFO << "[PaymentService] Order created: order_no=" << request.orderNo
                       << ", user_id=" << request.userId << ", amount=" << request.amount
                       << ", creating payment_no=" << paymentNo;
              // Create payment record
              Mapper<PayPaymentModel> paymentMapper(dbClient_);
              PayPaymentModel payment;
              payment.setOrderNo(request.orderNo);
              payment.setPaymentNo(paymentNo);
              payment.setStatus("INIT");
              payment.setAmount(request.amount);
              payment.setRequestPayload(requestPayload);
              payment.setCreatedAt(trantor::Date::now());
              paymentMapper.insert(
                payment,
                [this, request, paymentNo, payload, sharedCb](const PayPaymentModel &) {
                    LOG_INFO << "[PaymentService] Payment record created: payment_no=" << paymentNo
                             << ", order_no=" << request.orderNo << ", channel=" << request.channel;
                    // Helper lambda to handle payment client response
                    auto paymentCallback = [this, request, paymentNo, sharedCb](
                                             const Json::Value &result, const std::string &error
                                           ) {
                        if (!error.empty())
                        {
                            // Handle payment error
                            Json::Value errJson;
                            errJson["error"] = error;
                            const std::string errPayload = pay::utils::toJsonString(errJson);

                            // Update payment status to FAILED
                            Mapper<PayPaymentModel> paymentMapper(dbClient_);
                            auto payCriteria = Criteria(
                              PayPaymentModel::Cols::_payment_no, CompareOperator::EQ, paymentNo
                            );
                            paymentMapper.findOne(
                              payCriteria,
                              [this, errPayload, request, sharedCb](PayPaymentModel payment) {
                                  payment.setStatus("FAIL");
                                  payment.setResponsePayload(errPayload);
                                  Mapper<PayPaymentModel> paymentUpdater(dbClient_);
                                  paymentUpdater.update(
                                    payment,
                                    [this, request, sharedCb](const size_t) {
                                        // Update order status to FAILED
                                        Mapper<PayOrderModel> orderMapper(dbClient_);
                                        auto orderCriteria = Criteria(
                                          PayOrderModel::Cols::_order_no,
                                          CompareOperator::EQ,
                                          request.orderNo
                                        );
                                        orderMapper.findOne(
                                          orderCriteria,
                                          [this, sharedCb](PayOrderModel order) {
                                              order.setStatus("FAILED");
                                              Mapper<PayOrderModel> orderUpdater(dbClient_);
                                              orderUpdater.update(
                                                order,
                                                [](const size_t) {},
                                                [](const DrogonDbException &) {}
                                              );
                                          },
                                          [sharedCb](const DrogonDbException &) {
                                              if (*sharedCb)
                                              {
                                                  Json::Value response;
                                                  response["code"] = 1003;
                                                  response["message"] =
                                                    "Database error during payment failure update";
                                                  (*sharedCb)(
                                                    response,
                                                    pay::makePayError(
                                                      1003,
                                                      "Database error during payment failure update"
                                                    )
                                                  );
                                              }
                                          }
                                        );
                                    },
                                    [sharedCb](const DrogonDbException &) {
                                        if (*sharedCb)
                                        {
                                            Json::Value response;
                                            response["code"] = 1003;
                                            response["message"] =
                                              "Database error during payment failure update";
                                            (*sharedCb)(
                                              response,
                                              pay::makePayError(
                                                1003, "Database error during payment failure update"
                                              )
                                            );
                                        }
                                    }
                                  );
                              },
                              [sharedCb](const DrogonDbException &) {
                                  if (*sharedCb)
                                  {
                                      Json::Value response;
                                      response["code"] = 1003;
                                      response["message"] =
                                        "Database error during payment failure update";
                                      (*sharedCb)(
                                        response,
                                        pay::makePayError(
                                          1003, "Database error during payment failure update"
                                        )
                                      );
                                  }
                              }
                            );

                            // Return error response
                            if (*sharedCb)
                            {
                                Json::Value response;
                                response["code"] = 1002;
                                std::string channelName =
                                  request.channel == "alipay" ? "Alipay" : "WeChat Pay";
                                response["message"] = channelName + " error: " + error;
                                (*sharedCb)(response, pay::makePayError(1002, error));
                            }
                            return;
                        }

                        // Success - update payment and order status
                        const std::string responsePayload = pay::utils::toJsonString(result);

                        Mapper<PayPaymentModel> paymentMapper(dbClient_);
                        auto payCriteria = Criteria(
                          PayPaymentModel::Cols::_payment_no, CompareOperator::EQ, paymentNo
                        );
                        paymentMapper.findOne(
                          payCriteria,
                          [this, request, paymentNo, result, responsePayload, sharedCb](
                            PayPaymentModel payment
                          ) {
                              payment.setStatus("PROCESSING");
                              payment.setResponsePayload(responsePayload);
                              Mapper<PayPaymentModel> paymentUpdater(dbClient_);
                              paymentUpdater.update(
                                payment,
                                [this, request, paymentNo, result, sharedCb](const size_t) {
                                    // Update order status to PAYING
                                    Mapper<PayOrderModel> orderMapper(dbClient_);
                                    auto orderCriteria = Criteria(
                                      PayOrderModel::Cols::_order_no,
                                      CompareOperator::EQ,
                                      request.orderNo
                                    );
                                    orderMapper.findOne(
                                      orderCriteria,
                                      [this, request, paymentNo, result, sharedCb](
                                        PayOrderModel order
                                      ) {
                                          order.setStatus("PAYING");
                                          Mapper<PayOrderModel> orderUpdater(dbClient_);
                                          orderUpdater.update(
                                            order,
                                            [this, request, paymentNo, result, sharedCb](
                                              const size_t
                                            ) {
                                                // Build success response
                                                Json::Value response;
                                                response["code"] = 0;
                                                response["message"] =
                                                  "Payment created successfully";
                                                Json::Value data;
                                                data["order_no"] = request.orderNo;
                                                data["payment_no"] = paymentNo;
                                                data["status"] = "PAYING";

                                                // Add payment channel response details
                                                if (request.channel == "alipay")
                                                {
                                                    // Alipay response
                                                    data["alipay_response"] = result;
                                                    const auto qrCode =
                                                      result.get("qr_code", "").asString();
                                                    if (!qrCode.empty())
                                                    {
                                                        data["qr_code"] = qrCode;
                                                    }
                                                }
                                                else
                                                {
                                                    // WeChat Pay response
                                                    data["wechat_response"] = result;
                                                    const auto codeUrl =
                                                      result.get("code_url", "").asString();
                                                    if (!codeUrl.empty())
                                                    {
                                                        data["code_url"] = codeUrl;
                                                    }
                                                    const auto prepayId =
                                                      result.get("prepay_id", "").asString();
                                                    if (!prepayId.empty())
                                                    {
                                                        data["prepay_id"] = prepayId;
                                                    }
                                                }

                                                response["data"] = data;
                                                if (*sharedCb)
                                                {
                                                    (*sharedCb)(response, std::error_code());
                                                }
                                            },
                                            [sharedCb](const DrogonDbException &e) {
                                                if (*sharedCb)
                                                {
                                                    Json::Value response;
                                                    response["code"] = 1003;
                                                    response["message"] =
                                                      "Database error: " +
                                                      std::string(e.base().what());
                                                    (*sharedCb)(
                                                      response,
                                                      pay::makePayError(
                                                        1003,
                                                        "Database error: " +
                                                          std::string(e.base().what())
                                                      )
                                                    );
                                                }
                                            }
                                          );
                                      },
                                      [sharedCb](const DrogonDbException &e) {
                                          if (*sharedCb)
                                          {
                                              Json::Value response;
                                              response["code"] = 1003;
                                              response["message"] =
                                                "Database error: " + std::string(e.base().what());
                                              (*sharedCb)(
                                                response,
                                                pay::makePayError(
                                                  1003,
                                                  "Database error: " + std::string(e.base().what())
                                                )
                                              );
                                          }
                                      }
                                    );
                                },
                                [sharedCb](const DrogonDbException &e) {
                                    if (*sharedCb)
                                    {
                                        Json::Value response;
                                        response["code"] = 1003;
                                        response["message"] =
                                          "Database error: " + std::string(e.base().what());
                                        (*sharedCb)(
                                          response,
                                          pay::makePayError(
                                            1003, "Database error: " + std::string(e.base().what())
                                          )
                                        );
                                    }
                                }
                              );
                          },
                          [sharedCb](const DrogonDbException &e) {
                              if (*sharedCb)
                              {
                                  Json::Value response;
                                  response["code"] = 1003;
                                  response["message"] =
                                    "Database error: " + std::string(e.base().what());
                                  (*sharedCb)(
                                    response,
                                    pay::makePayError(
                                      1003, "Database error: " + std::string(e.base().what())
                                    )
                                  );
                              }
                          }
                        );
                    };

                    // Route to appropriate payment client based on channel
                    LOG_INFO << "[PaymentService] Calling payment client: channel="
                             << request.channel << ", order_no=" << request.orderNo
                             << ", payment_no=" << paymentNo;
                    if (request.channel == "alipay")
                    {
                        // Call Alipay Sandbox precreate API for QR code payment
                        LOG_DEBUG
                          << "[PaymentService] Calling Alipay precreateTrade API for order: "
                          << request.orderNo;
                        alipayClient_->precreateTrade(payload, paymentCallback);
                    }
                    else
                    {
                        // Call WeChat Pay API to create transaction
                        LOG_DEBUG
                          << "[PaymentService] Calling WeChat Pay createTransactionNative API";
                        wechatClient_->createTransactionNative(payload, paymentCallback);
                    }
                },
                [sharedCb](const DrogonDbException &e) {
                    LOG_ERROR << "Failed to insert payment record: " << e.base().what();
                    if (*sharedCb)
                    {
                        Json::Value response;
                        response["code"] = 1003;
                        response["message"] = "Database error: " + std::string(e.base().what());
                        (*sharedCb)(response, std::make_error_code(std::errc::io_error));
                    }
                }
              );
          },
          [sharedCb](const DrogonDbException &e) {
              if (*sharedCb)
              {
                  Json::Value response;
                  response["code"] = 1003;
                  response["message"] = "Database error: " + std::string(e.base().what());
                  (*sharedCb)(response, std::make_error_code(std::errc::io_error));
              }
          }
        );
    }
    catch (const std::exception &e)
    {
        if (*sharedCb)
        {
            Json::Value response;
            response["code"] = 1003;
            response["message"] = "Exception during payment creation: " + std::string(e.what());
            (*sharedCb)(response, std::make_error_code(std::errc::io_error));
        }
    }
}

void PaymentService::createQRPayment(const Json::Value &request, PaymentCallback &&callback)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<PaymentCallback>(std::move(callback));

    // Extract parameters
    std::string orderNo = request.get("order_no", "").asString();
    std::string amount = request.get("amount", "").asString();
    std::string channel = request.get("channel", "alipay").asString();
    std::string subject = request.get("subject", "Payment").asString();

    if (orderNo.empty() || amount.empty())
    {
        Json::Value response;
        response["code"] = 400;
        response["message"] = "Missing required parameters: order_no, amount";
        (*sharedCb)(response, std::make_error_code(std::errc::invalid_argument));
        return;
    }

    // Build QR payment payload for Alipay
    Json::Value payload;
    payload["out_trade_no"] = orderNo;
    payload["total_amount"] = amount;
    payload["subject"] = subject;

    if (request.isMember("buyer_id"))
    {
        payload["buyer_id"] = request["buyer_id"].asString();
    }

    LOG_INFO << "[PaymentService] Creating QR payment: channel=" << channel
             << ", order_no=" << orderNo << ", amount=" << amount;

    // Call Alipay precreate API
    alipayClient_->precreateTrade(
      payload,
      [this, orderNo, amount, channel, subject, request, sharedCb](
        const Json::Value &result, const std::string &error
      ) {
          if (!error.empty())
          {
              Json::Value response;
              response["code"] = 500;
              response["message"] = "QR payment creation failed: " + error;
              (*sharedCb)(response, std::make_error_code(std::errc::io_error));
              return;
          }

          // Check Alipay response code
          std::string alipayCode = result.get("code", "").asString();
          if (alipayCode != "10000")
          {
              // Alipay business error
              Json::Value response;
              response["code"] = 500;
              std::string subMsg = result.get("sub_msg", "").asString();
              std::string msg = result.get("msg", "").asString();
              std::string fullMessage = "Alipay error: " + msg;
              if (!subMsg.empty())
              {
                  fullMessage += " - " + subMsg;
              }
              response["message"] = fullMessage;
              response["alipay_code"] = alipayCode;
              response["alipay_sub_code"] = result.get("sub_code", "").asString();
              (*sharedCb)(response, std::make_error_code(std::errc::io_error));
              return;
          }

          // Alipay precreate response contains qr_code
          Json::Value data;
          data["order_no"] = orderNo;

          // Extract qr_code from Alipay response
          if (result.isMember("qr_code"))
          {
              data["qr_code"] = result["qr_code"].asString();
          }
          if (result.isMember("out_trade_no"))
          {
              data["out_trade_no"] = result["out_trade_no"].asString();
          }

          // Save order to database
          LOG_INFO << "[PaymentService] Saving order to database: order_no=" << orderNo;
          Mapper<PayOrderModel> orderMapper(dbClient_);
          PayOrderModel newOrder;
          newOrder.setOrderNo(orderNo);
          newOrder.setAmount(amount);
          newOrder.setCurrency("CNY");
          newOrder.setStatus("PAYING");  // Initial status
          newOrder.setChannel(channel);
          newOrder.setTitle(subject);
          newOrder.setUserId(request.get("user_id", "1").asInt64());

          orderMapper.insert(
            newOrder,
            [this, orderNo, amount, channel, subject, data, sharedCb](const PayOrderModel &order) {
                LOG_INFO << "[PaymentService] Order saved successfully: order_no=" << orderNo
                         << ", db_id=" << order.getValueOfId();

                Json::Value response;
                response["code"] = 0;
                response["message"] = "QR code created successfully";
                response["data"] = data;
                (*sharedCb)(response, std::error_code());
            },
            [sharedCb](const DrogonDbException &e) {
                LOG_ERROR << "Failed to save order to database: " << e.base().what();
                Json::Value errorResponse;
                errorResponse["code"] = 500;
                errorResponse["message"] = "Failed to save order: " + std::string(e.base().what());
                (*sharedCb)(errorResponse, std::make_error_code(std::errc::io_error));
            }
          );
      }
    );
}

void PaymentService::queryOrder(const std::string &orderNo, PaymentCallback &&callback)
{
    if (!dbClient_)
    {
        Json::Value response;
        response["code"] = 1003;
        response["message"] = "Database client not available";
        callback(response, std::make_error_code(std::errc::io_error));
        return;
    }

    if (orderNo.empty())
    {
        Json::Value response;
        response["code"] = 1001;
        response["message"] = "Missing order_no parameter";
        callback(response, std::make_error_code(std::errc::invalid_argument));
        return;
    }

    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<PaymentCallback>(std::move(callback));

    // Query order from database
    Mapper<PayOrderModel> orderMapper(dbClient_);
    auto criteria = Criteria(PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo);

    orderMapper.findOne(
      criteria,
      [this, orderNo, sharedCb](const PayOrderModel &order) {
          Json::Value response;
          response["code"] = 0;
          response["message"] = "Order found";
          Json::Value data;
          data["order_no"] = order.getValueOfOrderNo();
          data["amount"] = order.getValueOfAmount();
          data["currency"] = order.getValueOfCurrency();
          data["status"] = order.getValueOfStatus();
          data["channel"] = order.getValueOfChannel();
          data["title"] = order.getValueOfTitle();
          data["user_id"] = static_cast<Json::Int64>(order.getValueOfUserId());

          const std::string channel = order.getValueOfChannel();
          LOG_DEBUG << "[PAYMENT_SERVICE] queryOrder: order_no=" << orderNo
                    << " channel=" << channel << " current_status=" << data["status"].asString();

          // Query real-time status from payment channel API
          if (channel == "wechat" && wechatClient_)
          {
              // Query transaction from WeChat Pay
              wechatClient_->queryTransaction(
                orderNo,
                [this,
                 orderNo,
                 data,
                 sharedCb](const Json::Value &result, const std::string &error) {
                    if (!error.empty())
                    {
                        // Return database data with error header
                        Json::Value innerResponse;
                        innerResponse["code"] = 0;
                        innerResponse["message"] = "Order found (with query error)";
                        innerResponse["data"] = data;
                        innerResponse["data"]["wechat_query_error"] = error;
                        if (*sharedCb)
                        {
                            (*sharedCb)(innerResponse, std::error_code());
                        }
                        return;
                    }

                    // Sync order status from WeChat response
                    syncOrderStatusFromWechat(
                      orderNo, result, [data, result, sharedCb](const std::string &status) {
                          Json::Value innerResponse;
                          innerResponse["code"] = 0;
                          innerResponse["message"] = "Order found";
                          innerResponse["data"] = data;

                          if (!status.empty())
                          {
                              innerResponse["data"]["status"] = status;
                          }
                          const auto channelRefundNo = result.get("refund_id", "").asString();
                          if (!channelRefundNo.empty())
                          {
                              innerResponse["data"]["channel_refund_no"] = channelRefundNo;
                          }
                          innerResponse["data"]["wechat_response"] = result;
                          if (*sharedCb)
                          {
                              (*sharedCb)(innerResponse, std::error_code());
                          }
                      }
                    );
                }
              );
          }
          else if (channel == "alipay" && alipayClient_)
          {
              // Query trade from Alipay
              LOG_DEBUG << "[PAYMENT_SERVICE] Querying Alipay API for order " << orderNo;
              alipayClient_->queryTrade(
                orderNo,
                [this,
                 orderNo,
                 data,
                 sharedCb](const Json::Value &result, const std::string &error) {
                    if (!error.empty())
                    {
                        LOG_ERROR << "[PAYMENT_SERVICE] Alipay query error for " << orderNo << ": "
                                  << error;
                        // Return database data with error header
                        Json::Value innerResponse;
                        innerResponse["code"] = 0;
                        innerResponse["message"] = "Order found (with query error)";
                        innerResponse["data"] = data;
                        innerResponse["data"]["alipay_query_error"] = error;
                        if (*sharedCb)
                        {
                            (*sharedCb)(innerResponse, std::error_code());
                        }
                        return;
                    }

                    LOG_DEBUG << "[PAYMENT_SERVICE] Alipay response for " << orderNo
                              << " code=" << result.get("code", "?").asString()
                              << " trade_status=" << result.get("trade_status", "?").asString();

                    // Sync order status from Alipay response
                    syncOrderStatusFromAlipay(
                      orderNo,
                      result,
                      [data, result, sharedCb, orderNo](const std::string &status) {
                          LOG_DEBUG
                            << "[PAYMENT_SERVICE] syncOrderStatusFromAlipay returned status="
                            << status << " for order " << orderNo;

                          Json::Value innerResponse;
                          innerResponse["code"] = 0;
                          innerResponse["message"] = "Order found";
                          innerResponse["data"] = data;

                          // Always update status if Alipay returns valid status
                          if (!status.empty())
                          {
                              innerResponse["data"]["status"] = status;
                              LOG_DEBUG << "[PAYMENT_SERVICE] Updated order status to: " << status;
                          }
                          else
                          {
                              // If Alipay query failed or returned unknown status,
                              // keep the database status
                              LOG_DEBUG << "[PAYMENT_SERVICE] Alipay query failed, keeping "
                                           "database status: "
                                        << data["status"].asString();
                          }

                          const auto tradeNo = result.get("trade_no", "").asString();
                          if (!tradeNo.empty())
                          {
                              innerResponse["data"]["trade_no"] = tradeNo;
                          }
                          innerResponse["data"]["alipay_response"] = result;

                          // Safely access status field for logging
                          const auto &finalStatus = innerResponse["data"]["status"];
                          if (finalStatus.isString())
                          {
                              LOG_DEBUG << "[PAYMENT_SERVICE] Final response status="
                                        << finalStatus.asString() << " for order " << orderNo;
                          }
                          else
                          {
                              LOG_DEBUG
                                << "[PAYMENT_SERVICE] Final response status=<non-string type>"
                                << " for order " << orderNo;
                          }

                          if (*sharedCb)
                          {
                              (*sharedCb)(innerResponse, std::error_code());
                          }
                      }
                    );
                }
              );
          }
          else
          {
              // Channel not supported or client not available, return database data
              LOG_DEBUG << "[PAYMENT_SERVICE] Using database data for order " << orderNo
                        << " (channel=" << channel
                        << " has_client=" << (alipayClient_ != nullptr || wechatClient_ != nullptr)
                        << ")";
              response["data"] = data;
              if (*sharedCb)
              {
                  (*sharedCb)(response, std::error_code());
              }
          }
      },
      [sharedCb](const DrogonDbException &e) {
          if (*sharedCb)
          {
              Json::Value response;
              response["code"] = 1004;
              response["message"] = "Order not found: " + std::string(e.base().what());
              (*sharedCb)(
                response,
                pay::makePayError(1004, "Order not found: " + std::string(e.base().what()))
              );
          }
      }
    );
}

void PaymentService::syncOrderStatusFromWechat(
  const std::string &orderNo,
  const Json::Value &result,
  std::function<void(const std::string &status)> &&callback
)
{
    const std::string tradeState = result.get("trade_state", "").asString();
    if (tradeState.empty())
    {
        if (callback)
        {
            callback("");
        }
        return;
    }

    // Map trade state to order and payment status
    std::string orderStatus;
    std::string paymentStatus;
    pay::utils::mapTradeState(tradeState, orderStatus, paymentStatus);

    const std::string transactionId = result.get("transaction_id", "").asString();
    const std::string responsePayload = pay::utils::toJsonString(result);

    if (!dbClient_)
    {
        if (callback)
        {
            callback(orderStatus);
        }
        return;
    }

    LOG_INFO << "Sync order status from WeChat: order_no=" << orderNo
             << " trade_state=" << tradeState << " order_status=" << orderStatus
             << " payment_status=" << paymentStatus;

    // Find the latest payment record for this order
    Mapper<PayPaymentModel> paymentMapper(dbClient_);
    auto paymentCriteria = Criteria(PayPaymentModel::Cols::_order_no, CompareOperator::EQ, orderNo);

    paymentMapper.orderBy(PayPaymentModel::Cols::_created_at, SortOrder::DESC)
      .limit(1)
      .findBy(
        paymentCriteria,
        [this, orderNo, orderStatus, paymentStatus, transactionId, responsePayload, callback](
          const std::vector<PayPaymentModel> &rows
        ) {
            if (rows.empty())
            {
                if (callback)
                {
                    callback(orderStatus);
                }
                return;
            }

            auto payment = rows.front();
            const auto paymentNo = payment.getValueOfPaymentNo();

            // Use transaction for atomic updates
            dbClient_->newTransactionAsync([this,
                                            orderNo,
                                            orderStatus,
                                            paymentStatus,
                                            transactionId,
                                            responsePayload,
                                            payment,
                                            paymentNo,
                                            callback](
                                             const std::shared_ptr<Transaction> &transPtr
                                           ) mutable {
                auto rollbackDone = [callback, orderStatus, transPtr](const DrogonDbException &e) {
                    LOG_ERROR << "Reconcile transaction error: " << e.base().what();
                    transPtr->rollback();
                    if (callback)
                    {
                        callback(orderStatus);
                    }
                };

                auto transDb = std::static_pointer_cast<DbClient>(transPtr);

                // If payment is already SUCCESS, only update order
                if (payment.getValueOfStatus() == "SUCCESS")
                {
                    Mapper<PayOrderModel> orderMapper(transPtr);
                    auto orderCriteria =
                      Criteria(PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo);
                    orderMapper.findOne(
                      orderCriteria,
                      [this, orderStatus, paymentNo, callback, transPtr, transDb](
                        PayOrderModel order
                      ) {
                          if (order.getValueOfStatus() != "PAID")
                          {
                              const auto userId = order.getValueOfUserId();
                              const auto orderAmount = order.getValueOfAmount();
                              const auto orderNo = order.getValueOfOrderNo();
                              order.setStatus(orderStatus);
                              Mapper<PayOrderModel> orderUpdater(transPtr);
                              orderUpdater.update(
                                order,
                                [userId,
                                 orderNo,
                                 paymentNo,
                                 orderAmount,
                                 orderStatus,
                                 transPtr,
                                 transDb](const size_t) {
                                    if (orderStatus == "PAID")
                                    {
                                        insertLedgerEntry(
                                          transDb,
                                          userId,
                                          orderNo,
                                          paymentNo,
                                          "PAYMENT",
                                          orderAmount
                                        );
                                    }
                                },
                                [callback, orderStatus, transPtr](const DrogonDbException &e) {
                                    LOG_ERROR << "Reconcile order update error: "
                                              << e.base().what();
                                    transPtr->rollback();
                                    if (callback)
                                    {
                                        callback(orderStatus);
                                    }
                                }
                              );
                          }
                          else
                          {
                              // Order already PAID, no update needed
                          }
                          if (callback)
                          {
                              callback(orderStatus);
                          }
                      },
                      [callback, orderStatus, transPtr](const DrogonDbException &e) {
                          LOG_ERROR << "Reconcile order select error: " << e.base().what();
                          transPtr->rollback();
                          if (callback)
                          {
                              callback(orderStatus);
                          }
                      }
                    );
                    return;
                }

                // Update payment status with concurrency control
                // Check if payment is already in a final state to prevent concurrent updates
                const std::string currentStatus = payment.getValueOfStatus();
                if (currentStatus == "SUCCESS" || currentStatus == "REFUNDED")
                {
                    // Payment already in final state, no need to update
                    LOG_INFO << "[PaymentService] Payment " << paymentNo
                             << " already in final state: " << currentStatus;
                    transPtr->rollback();
                    if (callback)
                    {
                        callback(currentStatus == "SUCCESS" ? "PAID" : "REFUNDED");
                    }
                    return;
                }

                payment.setStatus(paymentStatus);
                payment.setChannelTradeNo(transactionId);
                payment.setResponsePayload(responsePayload);
                Mapper<PayPaymentModel> paymentUpdater(transPtr);
                paymentUpdater.update(
                  payment,
                  [this, orderNo, orderStatus, paymentNo, callback, transPtr, transDb](
                    const size_t
                  ) {
                      // Update order status
                      Mapper<PayOrderModel> orderMapper(transPtr);
                      auto orderCriteria =
                        Criteria(PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo);
                      orderMapper.findOne(
                        orderCriteria,
                        [orderStatus, paymentNo, callback, transPtr, transDb](PayOrderModel order) {
                            if (order.getValueOfStatus() == "PAID")
                            {
                                if (callback)
                                {
                                    callback(orderStatus);
                                }
                                return;
                            }
                            const auto userId = order.getValueOfUserId();
                            const auto orderAmount = order.getValueOfAmount();
                            const auto orderNo = order.getValueOfOrderNo();
                            order.setStatus(orderStatus);
                            Mapper<PayOrderModel> orderUpdater(transPtr);
                            orderUpdater.update(
                              order,
                              [callback,
                               orderStatus,
                               userId,
                               orderNo,
                               paymentNo,
                               orderAmount,
                               transPtr,
                               transDb](const size_t) {
                                  if (orderStatus == "PAID")
                                  {
                                      insertLedgerEntry(
                                        transDb, userId, orderNo, paymentNo, "PAYMENT", orderAmount
                                      );
                                  }
                                  if (callback)
                                  {
                                      callback(orderStatus);
                                  }
                              },
                              [callback, orderStatus, transPtr](const DrogonDbException &e) {
                                  LOG_ERROR << "Reconcile order update error: " << e.base().what();
                                  transPtr->rollback();
                                  if (callback)
                                  {
                                      callback(orderStatus);
                                  }
                              }
                            );
                        },
                        [callback, orderStatus, transPtr](const DrogonDbException &e) {
                            LOG_ERROR << "Reconcile order select error: " << e.base().what();
                            transPtr->rollback();
                            if (callback)
                            {
                                callback(orderStatus);
                            }
                        }
                      );
                  },
                  rollbackDone
                );
            });
        },
        [](const DrogonDbException &e) {
            LOG_ERROR << "Reconcile payment select error: " << e.base().what();
        }
      );
}

void PaymentService::syncOrderStatusFromAlipay(
  const std::string &orderNo,
  const Json::Value &result,
  std::function<void(const std::string &status)> &&callback
)
{
    const std::string responseCode = result.get("code", "").asString();
    if (responseCode != "10000")
    {
        // Alipay API call failed or trade not found
        if (callback)
        {
            callback("");
        }
        return;
    }

    const std::string tradeStatus = result.get("trade_status", "").asString();
    if (tradeStatus.empty())
    {
        if (callback)
        {
            callback("");
        }
        return;
    }

    // Map Alipay trade_status to order and payment status
    std::string orderStatus;
    std::string paymentStatus;

    if (tradeStatus == "TRADE_SUCCESS" || tradeStatus == "TRADE_FINISHED")
    {
        orderStatus = "PAID";
        paymentStatus = "SUCCESS";
    }
    else if (tradeStatus == "WAIT_BUYER_PAY")
    {
        orderStatus = "PAYING";
        paymentStatus = "PROCESSING";
    }
    else if (tradeStatus == "TRADE_CLOSED")
    {
        orderStatus = "FAILED";
        paymentStatus = "FAILED";
    }
    else
    {
        // Unknown status
        LOG_WARN << "Unknown Alipay trade_status: " << tradeStatus << " for order " << orderNo;
        if (callback)
        {
            callback("");
        }
        return;
    }

    const std::string transactionId = result.get("trade_no", "").asString();
    const std::string responsePayload = pay::utils::toJsonString(result);

    if (!dbClient_)
    {
        if (callback)
        {
            callback(orderStatus);
        }
        return;
    }

    LOG_DEBUG << "Sync order status from Alipay: order_no=" << orderNo
              << " trade_status=" << tradeStatus << " order_status=" << orderStatus
              << " payment_status=" << paymentStatus;

    // Find the latest payment record for this order
    Mapper<PayPaymentModel> paymentMapper(dbClient_);
    auto paymentCriteria = Criteria(PayPaymentModel::Cols::_order_no, CompareOperator::EQ, orderNo);

    paymentMapper.orderBy(PayPaymentModel::Cols::_created_at, SortOrder::DESC)
      .limit(1)
      .findBy(
        paymentCriteria,
        [this, orderNo, orderStatus, paymentStatus, transactionId, responsePayload, callback](
          const std::vector<PayPaymentModel> &rows
        ) {
            if (rows.empty())
            {
                if (callback)
                {
                    callback(orderStatus);
                }
                return;
            }

            auto payment = rows.front();
            const auto paymentNo = payment.getValueOfPaymentNo();

            // Use transaction for atomic updates
            dbClient_->newTransactionAsync([this,
                                            orderNo,
                                            orderStatus,
                                            paymentStatus,
                                            transactionId,
                                            responsePayload,
                                            payment,
                                            paymentNo,
                                            callback](
                                             const std::shared_ptr<Transaction> &transPtr
                                           ) mutable {
                auto rollbackDone = [callback, orderStatus, transPtr](const DrogonDbException &e) {
                    LOG_ERROR << "Alipay reconcile transaction error: " << e.base().what();
                    transPtr->rollback();
                    if (callback)
                    {
                        callback(orderStatus);
                    }
                };

                auto transDb = std::static_pointer_cast<DbClient>(transPtr);

                // If payment is already SUCCESS, only update order
                if (payment.getValueOfStatus() == "SUCCESS")
                {
                    Mapper<PayOrderModel> orderMapper(transPtr);
                    auto orderCriteria =
                      Criteria(PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo);
                    orderMapper.findOne(
                      orderCriteria,
                      [this, orderStatus, paymentNo, callback, transPtr, transDb](
                        PayOrderModel order
                      ) {
                          if (order.getValueOfStatus() != "PAID")
                          {
                              const auto userId = order.getValueOfUserId();
                              const auto orderAmount = order.getValueOfAmount();
                              const auto orderNo = order.getValueOfOrderNo();
                              order.setStatus(orderStatus);
                              Mapper<PayOrderModel> orderUpdater(transPtr);
                              orderUpdater.update(
                                order,
                                [userId,
                                 orderNo,
                                 paymentNo,
                                 orderAmount,
                                 orderStatus,
                                 transPtr,
                                 transDb](const size_t) {
                                    if (orderStatus == "PAID")
                                    {
                                        insertLedgerEntry(
                                          transDb,
                                          userId,
                                          orderNo,
                                          paymentNo,
                                          "PAYMENT",
                                          orderAmount
                                        );
                                    }
                                },
                                [callback, orderStatus, transPtr](const DrogonDbException &e) {
                                    LOG_ERROR << "Alipay reconcile order update error: "
                                              << e.base().what();
                                    transPtr->rollback();
                                    if (callback)
                                    {
                                        callback(orderStatus);
                                    }
                                }
                              );
                          }
                          else
                          {
                              // Order already PAID, no update needed
                          }
                          if (callback)
                          {
                              callback(orderStatus);
                          }
                      },
                      [callback, orderStatus, transPtr](const DrogonDbException &e) {
                          LOG_ERROR << "Alipay reconcile order select error: " << e.base().what();
                          transPtr->rollback();
                          if (callback)
                          {
                              callback(orderStatus);
                          }
                      }
                    );
                    return;
                }

                // Update payment status with concurrency control
                // Check if payment is already in a final state to prevent concurrent updates
                const std::string currentStatus = payment.getValueOfStatus();
                if (currentStatus == "SUCCESS" || currentStatus == "REFUNDED")
                {
                    // Payment already in final state, no need to update
                    LOG_INFO << "[PaymentService] Payment " << paymentNo
                             << " already in final state: " << currentStatus;
                    transPtr->rollback();
                    if (callback)
                    {
                        callback(currentStatus == "SUCCESS" ? "PAID" : "REFUNDED");
                    }
                    return;
                }

                payment.setStatus(paymentStatus);
                payment.setChannelTradeNo(transactionId);
                payment.setResponsePayload(responsePayload);
                Mapper<PayPaymentModel> paymentUpdater(transPtr);
                paymentUpdater.update(
                  payment,
                  [this, orderNo, orderStatus, paymentNo, callback, transPtr, transDb](
                    const size_t
                  ) {
                      // Update order status
                      Mapper<PayOrderModel> orderMapper(transPtr);
                      auto orderCriteria =
                        Criteria(PayOrderModel::Cols::_order_no, CompareOperator::EQ, orderNo);
                      orderMapper.findOne(
                        orderCriteria,
                        [orderStatus, paymentNo, callback, transPtr, transDb](PayOrderModel order) {
                            if (order.getValueOfStatus() == "PAID")
                            {
                                if (callback)
                                {
                                    callback(orderStatus);
                                }
                                return;
                            }
                            const auto userId = order.getValueOfUserId();
                            const auto orderAmount = order.getValueOfAmount();
                            const auto orderNo = order.getValueOfOrderNo();
                            order.setStatus(orderStatus);
                            Mapper<PayOrderModel> orderUpdater(transPtr);
                            orderUpdater.update(
                              order,
                              [callback,
                               orderStatus,
                               userId,
                               orderNo,
                               paymentNo,
                               orderAmount,
                               transPtr,
                               transDb](const size_t) {
                                  if (orderStatus == "PAID")
                                  {
                                      insertLedgerEntry(
                                        transDb, userId, orderNo, paymentNo, "PAYMENT", orderAmount
                                      );
                                  }
                                  if (callback)
                                  {
                                      callback(orderStatus);
                                  }
                              },
                              [callback, orderStatus, transPtr](const DrogonDbException &e) {
                                  LOG_ERROR << "Alipay reconcile order update error: "
                                            << e.base().what();
                                  transPtr->rollback();
                                  if (callback)
                                  {
                                      callback(orderStatus);
                                  }
                              }
                            );
                        },
                        [callback, orderStatus, transPtr](const DrogonDbException &e) {
                            LOG_ERROR << "Alipay reconcile order select error: " << e.base().what();
                            transPtr->rollback();
                            if (callback)
                            {
                                callback(orderStatus);
                            }
                        }
                      );
                  },
                  rollbackDone
                );
            });
        },
        [](const DrogonDbException &e) {
            LOG_ERROR << "Alipay reconcile payment select error: " << e.base().what();
        }
      );
}

void PaymentService::reconcileSummary(const std::string &date, PaymentCallback &&callback)
{
    if (!dbClient_)
    {
        Json::Value response;
        response["code"] = 1003;
        response["message"] = "Database client not available";
        callback(response, std::make_error_code(std::errc::io_error));
        return;
    }

    auto responded = std::make_shared<std::atomic<bool>>(false);
    auto pending = std::make_shared<std::atomic<int>>(2);
    auto summary = std::make_shared<Json::Value>();
    (*summary)["paying_orders"] = 0;
    (*summary)["refunding_refunds"] = 0;
    (*summary)["oldest_paying_updated"] = "";
    (*summary)["oldest_refund_updated"] = "";

    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<PaymentCallback>(std::move(callback));

    auto finishIfReady = [sharedCb, responded, pending, summary]() {
        if (pending->fetch_sub(1) != 1)
        {
            return;
        }
        if (responded->exchange(true))
        {
            return;
        }
        if (*sharedCb)
        {
            Json::Value response;
            response["code"] = 0;
            response["message"] = "Reconciliation summary";
            response["data"] = *summary;
            (*sharedCb)(response, std::error_code());
        }
    };

    // Query paying orders
    dbClient_->execSqlAsync(
      "SELECT COUNT(*) AS cnt, MIN(updated_at) AS oldest_updated "
      "FROM pay_order WHERE status = $1",
      [summary, finishIfReady](const Result &r) {
          if (!r.empty())
          {
              const auto &row = r.front();
              (*summary)["paying_orders"] = row["cnt"].as<int64_t>();
              if (!row["oldest_updated"].isNull())
              {
                  (*summary)["oldest_paying_updated"] = row["oldest_updated"].as<std::string>();
              }
          }
          finishIfReady();
      },
      [sharedCb, responded](const DrogonDbException &e) {
          if (responded->exchange(true))
          {
              return;
          }
          if (*sharedCb)
          {
              Json::Value response;
              response["code"] = 1003;
              response["message"] = "Database error: " + std::string(e.base().what());
              (*sharedCb)(response, std::make_error_code(std::errc::io_error));
          }
      },
      "PAYING"
    );

    // Query refunding refunds
    dbClient_->execSqlAsync(
      "SELECT COUNT(*) AS cnt, MIN(updated_at) AS oldest_updated "
      "FROM pay_refund WHERE status IN ($1, $2)",
      [summary, finishIfReady](const Result &r) {
          if (!r.empty())
          {
              const auto &row = r.front();
              (*summary)["refunding_refunds"] = row["cnt"].as<int64_t>();
              if (!row["oldest_updated"].isNull())
              {
                  (*summary)["oldest_refund_updated"] = row["oldest_updated"].as<std::string>();
              }
          }
          finishIfReady();
      },
      [sharedCb, responded](const DrogonDbException &e) {
          if (responded->exchange(true))
          {
              return;
          }
          if (*sharedCb)
          {
              Json::Value response;
              response["code"] = 1003;
              response["message"] = "Database error: " + std::string(e.base().what());
              (*sharedCb)(response, std::make_error_code(std::errc::io_error));
          }
      },
      "REFUND_INIT",
      "REFUNDING"
    );
}

std::string PaymentService::generatePaymentNo()
{
    // Generate unique payment number
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 99999999);

    std::ostringstream oss;
    time_t now = std::time(nullptr);
    struct tm tmInfo;
#ifdef _WIN32
    localtime_s(&tmInfo, &now);
#else
    localtime_r(&now, &tmInfo);
#endif
    oss << "PAY" << std::put_time(&tmInfo, "%Y%m%d%H%M%S");
    oss << std::setfill('0') << std::setw(8) << dis(gen);

    return oss.str();
}

void PaymentService::queryOrderList(
  const std::string &status,
  const int64_t userId,
  const size_t limit,
  const size_t offset,
  PaymentCallback &&callback
)
{
    LOG_DEBUG << "[PAYMENT_SERVICE] queryOrderList called with status=" << status
              << ", userId=" << userId << ", limit=" << limit << ", offset=" << offset;

    // Build base SQL query with parameter placeholders to prevent SQL injection
    std::string sql =
      "SELECT po.order_no, po.user_id, po.amount, po.currency, "
      "po.status, po.channel, po.title, po.created_at, po.updated_at, "
      "pp.payment_no, pp.channel_trade_no, pp.response_payload "
      "FROM pay_order po "
      "LEFT JOIN pay_payment pp ON po.order_no = pp.order_no "
      "WHERE 1=1";

    // Build parameter list and count
    std::vector<std::string> params;
    size_t paramIndex = 1;

    // Add status filter if provided (use parameterized query)
    if (!status.empty() && status != "all")
    {
        sql += " AND po.status = $" + std::to_string(paramIndex++);
        params.push_back(status);
    }

    // Add user_id filter if provided (0 means no filter, use parameterized query)
    if (userId > 0)
    {
        sql += " AND po.user_id = $" + std::to_string(paramIndex++);
        params.push_back(std::to_string(userId));
    }

    // Add ordering and pagination
    sql += " ORDER BY po.created_at DESC";

    // Add limit (use parameterized query)
    size_t actualLimit = (limit > 0 && limit <= 100) ? limit : 50;
    sql += " LIMIT $" + std::to_string(paramIndex++);
    params.push_back(std::to_string(actualLimit));

    // Add offset (use parameterized query)
    sql += " OFFSET $" + std::to_string(paramIndex++);
    params.push_back(std::to_string(offset));

    LOG_DEBUG << "[PAYMENT_SERVICE] Executing parameterized SQL with " << params.size() << " parameters";

    // Execute parameterized query to prevent SQL injection
    if (params.size() == 4)
    {
        dbClient_->execSqlAsync(
          sql,
          [callback](const Result &result) {
              try
              {
                  Json::Value response;
                  response["code"] = 200;
                  response["message"] = "Success";
                  response["data"] = Json::Value(Json::arrayValue);

                  for (size_t i = 0; i < result.size(); ++i)
                  {
                      const auto &row = result[i];

                      Json::Value order;
                      order["order_no"] = row["order_no"].as<std::string>();
                      order["user_id"] = row["user_id"].as<int64_t>();
                      order["amount"] = row["amount"].as<std::string>();
                      order["currency"] = row["currency"].as<std::string>();
                      order["status"] = row["status"].as<std::string>();
                      order["channel"] = row["channel"].as<std::string>();
                      order["title"] = row["title"].as<std::string>();
                      order["created_at"] = row["created_at"].as<std::string>();
                      order["updated_at"] = row["updated_at"].as<std::string>();

                      // Add payment info if exists
                      if (!row["payment_no"].isNull())
                      {
                          order["payment_no"] = row["payment_no"].as<std::string>();
                      }
                      if (!row["channel_trade_no"].isNull())
                      {
                          order["trade_no"] = row["channel_trade_no"].as<std::string>();
                      }
                      if (!row["updated_at"].isNull())
                      {
                          order["paid_at"] = row["updated_at"].as<std::string>();
                      }
                      if (!row["response_payload"].isNull())
                      {
                          // Parse JSON from response_payload
                          try
                          {
                              Json::Value channelResponse;
                              Json::Reader reader;
                              reader.parse(row["response_payload"].as<std::string>(), channelResponse);
                              order["channel_response"] = channelResponse;
                          }
                          catch (...)
                          {
                              // If parsing fails, skip channel_response
                          }
                      }

                      response["data"].append(order);
                  }

                  LOG_DEBUG << "[PAYMENT_SERVICE] queryOrderList found " << response["data"].size()
                            << " orders";
                  callback(response, std::error_code());
              }
              catch (const std::exception &e)
              {
                  LOG_ERROR << "[PAYMENT_SERVICE] Exception in queryOrderList: " << e.what();
                  Json::Value error;
                  error["code"] = 1500;
                  error["message"] = "Internal server error";
                  callback(error, std::make_error_code(std::errc::io_error));
              }
          },
          [callback](const DrogonDbException &e) {
              LOG_ERROR << "[PAYMENT_SERVICE] Database error in queryOrderList: " << e.base().what();
              Json::Value error;
              error["code"] = 1500;
              error["message"] = "Database error: " + std::string(e.base().what());
              callback(error, std::make_error_code(std::errc::io_error));
          },
          params[0].c_str(),
          params[1].c_str(),
          params[2].c_str(),
          params[3].c_str()
        );
    }
    else
    {
        // Handle cases with fewer parameters (dynamic filtering)
        dbClient_->execSqlAsync(
          sql,
          [callback](const Result &result) {
              try
              {
                  Json::Value response;
                  response["code"] = 200;
                  response["message"] = "Success";
                  response["data"] = Json::Value(Json::arrayValue);

                  for (size_t i = 0; i < result.size(); ++i)
                  {
                      const auto &row = result[i];

                      Json::Value order;
                      order["order_no"] = row["order_no"].as<std::string>();
                      order["user_id"] = row["user_id"].as<int64_t>();
                      order["amount"] = row["amount"].as<std::string>();
                      order["currency"] = row["currency"].as<std::string>();
                      order["status"] = row["status"].as<std::string>();
                      order["channel"] = row["channel"].as<std::string>();
                      order["title"] = row["title"].as<std::string>();
                      order["created_at"] = row["created_at"].as<std::string>();
                      order["updated_at"] = row["updated_at"].as<std::string>();

                      // Add payment info if exists
                      if (!row["payment_no"].isNull())
                      {
                          order["payment_no"] = row["payment_no"].as<std::string>();
                      }
                      if (!row["channel_trade_no"].isNull())
                      {
                          order["trade_no"] = row["channel_trade_no"].as<std::string>();
                      }
                      if (!row["updated_at"].isNull())
                      {
                          order["paid_at"] = row["updated_at"].as<std::string>();
                      }
                      if (!row["response_payload"].isNull())
                      {
                          // Parse JSON from response_payload
                          try
                          {
                              Json::Value channelResponse;
                              Json::Reader reader;
                              reader.parse(row["response_payload"].as<std::string>(), channelResponse);
                              order["channel_response"] = channelResponse;
                          }
                          catch (...)
                          {
                              // If parsing fails, skip channel_response
                          }
                      }

                      response["data"].append(order);
                  }

                  LOG_DEBUG << "[PAYMENT_SERVICE] queryOrderList found " << response["data"].size()
                            << " orders";
                  callback(response, std::error_code());
              }
              catch (const std::exception &e)
              {
                  LOG_ERROR << "[PAYMENT_SERVICE] Exception in queryOrderList: " << e.what();
                  Json::Value error;
                  error["code"] = 1500;
                  error["message"] = "Internal server error";
                  callback(error, std::make_error_code(std::errc::io_error));
              }
          },
          [callback](const DrogonDbException &e) {
              LOG_ERROR << "[PAYMENT_SERVICE] Database error in queryOrderList: " << e.base().what();
              Json::Value error;
              error["code"] = 1500;
              error["message"] = "Database error: " + std::string(e.base().what());
              callback(error, std::make_error_code(std::errc::io_error));
          }
        );
    }
}
