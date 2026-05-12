#include "CallbackService.h"
#include "../utils/PayUtils.h"
#include "PayErrorCategory.h"
#include "../models/PayOrder.h"
#include "../models/PayPayment.h"
#include "../models/PayRefund.h"
#include "../models/PayCallback.h"
#include "../models/PayIdempotency.h"
#include "../models/PayLedger.h"
#include <drogon/drogon.h>
#include <sstream>

using namespace drogon;
using PayOrderModel = drogon_model::pay_test::PayOrder;
using PayPaymentModel = drogon_model::pay_test::PayPayment;
using PayRefundModel = drogon_model::pay_test::PayRefund;
using PayCallbackModel = drogon_model::pay_test::PayCallback;
using PayIdempotencyModel = drogon_model::pay_test::PayIdempotency;
using PayLedgerModel = drogon_model::pay_test::PayLedger;

namespace
{

void insertLedgerEntry(
  const std::shared_ptr<drogon::orm::DbClient> &dbClient,
  int64_t userId,
  const std::string &orderNo,
  const std::string &paymentNo,
  const std::string &entryType,
  const std::string &amount,
  std::function<void()> onSuccess
)
{
    if (!dbClient)
    {
        LOG_ERROR << "[CallbackService] DbClient is null in insertLedgerEntry";
        if (onSuccess)
            onSuccess();
        return;
    }

    PayLedgerModel ledger;
    ledger.setUserId(userId);
    ledger.setOrderNo(orderNo);
    ledger.setPaymentNo(paymentNo);
    ledger.setEntryType(entryType);
    ledger.setAmount(amount);
    ledger.setCreatedAt(trantor::Date::now());

    drogon::orm::Mapper<PayLedgerModel> mapper(dbClient);
    mapper.insert(
      ledger,
      [entryType, orderNo, paymentNo, amount, onSuccess](const PayLedgerModel &) {
          LOG_INFO << "[CallbackService] Ledger entry inserted: entry_type=" << entryType
                   << ", order_no=" << orderNo << ", payment_no=" << paymentNo
                   << ", amount=" << amount;
          if (onSuccess)
              onSuccess();
      },
      [entryType, orderNo, paymentNo, onSuccess](const drogon::orm::DrogonDbException &e) {
          LOG_ERROR << "[CallbackService] Failed to insert ledger entry: entry_type=" << entryType
                    << ", order_no=" << orderNo << ", error: " << e.base().what();
          // Continue even if ledger insert fails - don't block the callback
          if (onSuccess)
              onSuccess();
      }
    );
}

}  // namespace

CallbackService::CallbackService(
  std::shared_ptr<WechatPayClient> wechatClient,
  std::shared_ptr<drogon::orm::DbClient> dbClient
)
    : wechatClient_(wechatClient), dbClient_(dbClient)
{
}

void CallbackService::handlePaymentCallback(
  const std::string &body,
  const std::string &signature,
  const std::string &timestamp,
  const std::string &nonce,
  const std::string &serialNo,
  CallbackResult &&callback
)
{
    if (!wechatClient_)
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "wechat client not ready";
        callback(error, std::error_code(1400, std::system_category()));
        return;
    }

    auto respond = [callback](const Json::Value &result, const std::string &errorMsg) {
        if (!errorMsg.empty())
        {
            Json::Value error;
            error["code"] = "FAIL";
            error["message"] = errorMsg;
            callback(error, std::error_code(1400, std::system_category()));
            return;
        }
        callback(result, std::error_code());
    };

    // Verify signature first
    if (!verifySignature(body, signature, timestamp, nonce, serialNo))
    {
        LOG_WARN << "[CallbackService] Signature verification failed";
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "signature verification failed";
        callback(error, std::error_code(1400, std::system_category()));
        return;
    }
    LOG_INFO << "[CallbackService] Signature verified successfully";

    // Parse callback body
    Json::CharReaderBuilder builder;
    Json::Value notifyJson;
    std::string parseErrors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(body.data(), body.data() + body.size(), &notifyJson, &parseErrors))
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid json";
        respond(error, "invalid json");
        return;
    }

    // Validate event_type
    const std::string eventType = notifyJson.get("event_type", "").asString();
    if (eventType.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing event_type";
        respond(error, "missing event_type");
        return;
    }

    if (eventType.rfind("TRANSACTION.", 0) != 0)
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid transaction event_type";
        respond(error, "invalid transaction event_type");
        return;
    }

    // Validate resource
    if (!notifyJson.isMember("resource"))
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing resource";
        callback(error, std::error_code(1400, std::system_category()));
        return;
    }

    const auto &resource = notifyJson["resource"];
    const std::string resourceType = notifyJson.get("resource_type", "").asString();
    if (resourceType.empty() || resourceType != "encrypt-resource")
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "unsupported resource_type";
        respond(error, "unsupported resource_type");
        return;
    }

    const std::string algorithm = resource.get("algorithm", "").asString();
    if (algorithm.empty() || algorithm != "AEAD_AES_256_GCM")
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "unsupported resource algorithm";
        respond(error, "unsupported resource algorithm");
        return;
    }

    const std::string ciphertext = resource.get("ciphertext", "").asString();
    const std::string nonceStr = resource.get("nonce", "").asString();
    const std::string associatedData = resource.get("associated_data", "").asString();
    if (ciphertext.empty() || nonceStr.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid resource";
        respond(error, "invalid resource");
        return;
    }

    if (associatedData != "transaction")
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid transaction associated_data";
        respond(error, "invalid transaction associated_data");
        return;
    }

    // Decrypt resource
    LOG_INFO << "[CallbackService] Decrypting resource...";
    std::string plaintext;
    std::string decryptError;
    if (!wechatClient_
           ->decryptResource(ciphertext, nonceStr, associatedData, plaintext, decryptError))
    {
        LOG_WARN << "[CallbackService] Decryption failed: " << decryptError;
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = decryptError;
        respond(error, decryptError);
        return;
    }
    LOG_INFO << "[CallbackService] Decryption successful";

    // Parse decrypted JSON
    Json::Value plainJson;
    Json::CharReaderBuilder plainBuilder;
    std::string plainErrors;
    std::unique_ptr<Json::CharReader> plainReader(plainBuilder.newCharReader());
    if (!plainReader
           ->parse(plaintext.data(), plaintext.data() + plaintext.size(), &plainJson, &plainErrors))
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid resource json";
        respond(error, "invalid resource json");
        return;
    }

    // Validate appid and mchid
    const std::string appId = plainJson.get("appid", "").asString();
    const std::string mchId = plainJson.get("mchid", "").asString();
    if (!appId.empty() && wechatClient_ && appId != wechatClient_->getAppId())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "appid mismatch";
        respond(error, "appid mismatch");
        return;
    }
    if (!mchId.empty() && wechatClient_ && mchId != wechatClient_->getMchId())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "mchid mismatch";
        respond(error, "mchid mismatch");
        return;
    }

    // Extract payment details
    const std::string orderNo = plainJson.get("out_trade_no", "").asString();
    const std::string transactionId = plainJson.get("transaction_id", "").asString();
    const std::string tradeState = plainJson.get("trade_state", "").asString();

    if (orderNo.empty() || tradeState.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing out_trade_no/trade_state";
        respond(error, "missing out_trade_no/trade_state");
        return;
    }

    const bool tradeStateValid = tradeState == "SUCCESS" || tradeState == "USERPAYING" ||
                                 tradeState == "NOTPAY" || tradeState == "CLOSED" ||
                                 tradeState == "REVOKED" || tradeState == "REFUND";
    if (!tradeStateValid)
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid trade_state";
        respond(error, "invalid trade_state");
        return;
    }

    if (tradeState == "SUCCESS" && transactionId.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing transaction_id";
        respond(error, "missing transaction_id");
        return;
    }

    // Idempotency check
    std::string idempotencyKey = notifyJson.get("id", "").asString();
    if (idempotencyKey.empty())
    {
        idempotencyKey = orderNo + ":" + tradeState;
    }

    LOG_INFO << "[CallbackService] Preparing to process callback for order: " << orderNo;

    auto cbPtr = std::make_shared<CallbackResult>(std::move(callback));

    auto proceedWithDb = [this,
                          cbPtr,
                          idempotencyKey,
                          orderNo,
                          transactionId,
                          tradeState,
                          plaintext,
                          body,
                          signature,
                          serialNo,
                          plainJson]() {
        LOG_INFO << "[CallbackService] proceedWithDb lambda called for order: " << orderNo;
        drogon::orm::Mapper<PayIdempotencyModel> idempMapper(dbClient_);
        auto idempCriteria = drogon::orm::Criteria(
          PayIdempotencyModel::Cols::_idempotency_key,
          drogon::orm::CompareOperator::EQ,
          idempotencyKey
        );
        idempMapper.findOne(
          idempCriteria,
          [this, cbPtr, orderNo, body, signature, serialNo](const PayIdempotencyModel &) {
              // Already processed - record callback and return success
              LOG_INFO << "[CallbackService] Idempotency key found for order: " << orderNo
                       << ", recording callback";

              auto respondSuccess = [cbPtr]() {
                  Json::Value ok;
                  ok["code"] = "SUCCESS";
                  ok["message"] = "OK";
                  (*cbPtr)(ok, std::error_code());
              };

              auto respondDbError = [cbPtr](const drogon::orm::DrogonDbException &e) {
                  LOG_ERROR << "[CallbackService] DB error recording idempotent callback: "
                            << e.base().what();
                  Json::Value error;
                  error["code"] = "FAIL";
                  error["message"] = std::string("db error: ") + e.base().what();
                  (*cbPtr)(error, pay::makePayError(1400, "db transaction unavailable"));
              };

              drogon::orm::Mapper<PayPaymentModel> paymentLookup(dbClient_);
              paymentLookup.findOne(
                drogon::orm::Criteria(
                  PayPaymentModel::Cols::_order_no, drogon::orm::CompareOperator::EQ, orderNo
                ),
                [this, cbPtr, orderNo, body, signature, serialNo, respondSuccess, respondDbError](
                  const PayPaymentModel &payment
                ) {
                    const std::string paymentNo = payment.getValueOfPaymentNo();

                    drogon::orm::Mapper<PayCallbackModel> callbackMapper(dbClient_);
                    PayCallbackModel callbackRow;
                    callbackRow.setPaymentNo(paymentNo);
                    callbackRow.setRawBody(body);
                    callbackRow.setSignature(signature);
                    callbackRow.setSerialNo(serialNo);
                    callbackRow.setVerified(true);
                    callbackRow.setProcessed(true);
                    callbackRow.setReceivedAt(trantor::Date::now());

                    callbackMapper.insert(
                      callbackRow,
                      [respondSuccess](const PayCallbackModel &) { respondSuccess(); },
                      respondDbError
                    );
                },
                [cbPtr, respondDbError](const drogon::orm::DrogonDbException &e) {
                    LOG_ERROR << "[CallbackService] Payment not found during idempotent callback: "
                              << e.base().what();
                    respondDbError(e);
                }
              );
          },
          [this,
           cbPtr,
           idempotencyKey,
           orderNo,
           transactionId,
           tradeState,
           plaintext,
           body,
           signature,
           serialNo,
           plainJson](const drogon::orm::DrogonDbException &) {
              LOG_INFO << "[CallbackService] Idempotency key not found, processing new callback";
              const std::string requestHash = drogon::utils::getMd5(body);
              PayIdempotencyModel idemp;
              idemp.setIdempotencyKey(idempotencyKey);
              idemp.setRequestHash(requestHash);
              idemp.setResponseSnapshot(plaintext);
              const auto now = trantor::Date::now();
              const auto expiresAt = trantor::Date(
                now.microSecondsSinceEpoch() + static_cast<int64_t>(7) * 24 * 60 * 60 * 1000000
              );
              idemp.setExpireAt(expiresAt);

              // Insert idempotency record on main client (outside transaction)
              // so it's committed and visible to subsequent calls immediately.
              drogon::orm::Mapper<PayIdempotencyModel> idempInsert(dbClient_);
              idempInsert.insert(
                idemp,
                [this,
                 cbPtr,
                 idempotencyKey,
                 orderNo,
                 transactionId,
                 tradeState,
                 plaintext,
                 body,
                 signature,
                 serialNo,
                 plainJson](const PayIdempotencyModel &) {
                    LOG_INFO << "[CallbackService] Creating database transaction for order: "
                             << orderNo;
                    dbClient_->newTransactionAsync([this,
                                                    cbPtr,
                                                    orderNo,
                                                    transactionId,
                                                    tradeState,
                                                    plaintext,
                                                    body,
                                                    signature,
                                                    serialNo,
                                                    plainJson](
                                                     const std::shared_ptr<drogon::orm::Transaction>
                                                       &transPtr
                                                   ) mutable {
                        LOG_INFO << "[CallbackService] Transaction callback for order: " << orderNo
                                 << ", transPtr=" << (transPtr ? "valid" : "null");
                        if (!transPtr)
                        {
                            LOG_ERROR << "[CallbackService] Transaction creation failed for order: "
                                      << orderNo;
                            Json::Value error;
                            error["code"] = "FAIL";
                            error["message"] = "db transaction unavailable";
                            (*cbPtr)(error, pay::makePayError(1400, "db transaction unavailable"));
                            return;
                        }
                        auto respondDbError = [cbPtr](const drogon::orm::DrogonDbException &e) {
                            Json::Value error;
                            error["code"] = "FAIL";
                            error["message"] = std::string("db error: ") + e.base().what();
                            (*cbPtr)(error, pay::makePayError(1400, "db transaction unavailable"));
                        };

                        drogon::orm::Mapper<PayPaymentModel> paymentMapper(transPtr);
                        auto paymentCriteria = drogon::orm::Criteria(
                          PayPaymentModel::Cols::_order_no,
                          drogon::orm::CompareOperator::EQ,
                          orderNo
                        );
                        paymentMapper
                          .orderBy(PayPaymentModel::Cols::_created_at, drogon::orm::SortOrder::DESC)
                          .limit(1)
                          .findBy(
                            paymentCriteria,
                            [this,
                             cbPtr,
                             orderNo,
                             transactionId,
                             tradeState,
                             plaintext,
                             body,
                             signature,
                             serialNo,
                             plainJson,
                             transPtr,
                             respondDbError](const std::vector<PayPaymentModel> &rows) {
                                LOG_INFO << "[CallbackService] Payment query returned "
                                         << rows.size() << " rows for order: " << orderNo;
                                if (rows.empty())
                                {
                                    LOG_ERROR << "[CallbackService] Payment not found for order: "
                                              << orderNo;
                                    transPtr->rollback();
                                    Json::Value error;
                                    error["code"] = "FAIL";
                                    error["message"] = "payment not found";
                                    (*cbPtr)(error, std::error_code(1404, std::system_category()));
                                    return;
                                }

                                auto payment = rows.front();
                                const std::string paymentNo = payment.getValueOfPaymentNo();
                                LOG_INFO << "[CallbackService] Found payment: " << paymentNo
                                         << " for order: " << orderNo;

                                // Skip if payment already in final state
                                const std::string currentStatus = payment.getValueOfStatus();
                                if (currentStatus == "SUCCESS" || currentStatus == "REFUNDED")
                                {
                                    LOG_INFO << "[CallbackService] Payment " << paymentNo
                                             << " already in final state: " << currentStatus
                                             << ", skipping duplicate callback";
                                    transPtr->rollback();
                                    Json::Value ok;
                                    ok["code"] = "SUCCESS";
                                    ok["message"] = "OK";
                                    (*cbPtr)(ok, std::error_code());
                                    return;
                                }

                                const std::string orderAmount = payment.getValueOfAmount();

                                payment.getPayOrder(
                                  transPtr,
                                  [this,
                                   cbPtr,
                                   orderNo,
                                   paymentNo,
                                   orderAmount,
                                   transactionId,
                                   tradeState,
                                   plaintext,
                                   body,
                                   signature,
                                   serialNo,
                                   plainJson,
                                   transPtr,
                                   respondDbError,
                                   payment](PayOrderModel order) mutable {
                                      LOG_INFO << "[CallbackService] Order found for order: "
                                               << orderNo;
                                      const std::string orderCurrency = order.getValueOfCurrency();
                                      const auto &amountJson = plainJson["amount"];
                                      const std::string notifyCurrency =
                                        amountJson.get("currency", "").asString();
                                      const int64_t notifyTotalFen =
                                        amountJson.get("total", 0).asInt64();
                                      int64_t orderTotalFen = 0;
                                      if (
                                        !pay::utils::parseAmountToFen(orderAmount, orderTotalFen) ||
                                        notifyTotalFen <= 0
                                      )
                                      {
                                          transPtr->rollback();
                                          Json::Value error;
                                          error["code"] = "FAIL";
                                          error["message"] = "invalid amount in callback";
                                          (*cbPtr)(
                                            error,
                                            pay::makePayError(400, "invalid amount in callback")
                                          );
                                          return;
                                      }
                                      if (
                                        !notifyCurrency.empty() && notifyCurrency != orderCurrency
                                      )
                                      {
                                          transPtr->rollback();
                                          Json::Value error;
                                          error["code"] = "FAIL";
                                          error["message"] = "currency mismatch";
                                          (*cbPtr)(
                                            error,
                                            pay::makePayError(400, "invalid amount in callback")
                                          );
                                          return;
                                      }
                                      if (notifyTotalFen != orderTotalFen)
                                      {
                                          transPtr->rollback();
                                          Json::Value error;
                                          error["code"] = "FAIL";
                                          error["message"] = "amount mismatch";
                                          (*cbPtr)(
                                            error,
                                            pay::makePayError(400, "invalid amount in callback")
                                          );
                                          return;
                                      }

                                      std::string orderStatus;
                                      std::string paymentStatus;
                                      pay::utils::mapTradeState(
                                        tradeState, orderStatus, paymentStatus
                                      );
                                      LOG_INFO << "[CallbackService] Mapped trade state '"
                                               << tradeState << "' to order status: " << orderStatus
                                               << ", payment status: " << paymentStatus
                                               << " for order: " << orderNo;

                                      PayCallbackModel callbackRow;
                                      callbackRow.setPaymentNo(paymentNo);
                                      callbackRow.setRawBody(body);
                                      callbackRow.setSignature(signature);
                                      callbackRow.setSerialNo(serialNo);
                                      callbackRow.setVerified(true);
                                      callbackRow.setProcessed(true);
                                      callbackRow.setReceivedAt(trantor::Date::now());

                                      drogon::orm::Mapper<PayCallbackModel> callbackMapper(
                                        transPtr
                                      );
                                      LOG_INFO << "[CallbackService] About to insert callback "
                                                  "record for order: "
                                               << orderNo;
                                      callbackMapper.insert(
                                        callbackRow,
                                        [this,
                                         cbPtr,
                                         orderNo,
                                         paymentNo,
                                         orderStatus,
                                         paymentStatus,
                                         transactionId,
                                         plaintext,
                                         transPtr,
                                         respondDbError,
                                         payment,
                                         order](const PayCallbackModel &) mutable {
                                            LOG_INFO << "[CallbackService] Callback record "
                                                        "inserted for order: "
                                                     << orderNo;
                                            auto transDb =
                                              std::static_pointer_cast<drogon::orm::DbClient>(
                                                transPtr
                                              );

                                            payment.setStatus(paymentStatus);
                                            payment.setChannelTradeNo(transactionId);
                                            payment.setResponsePayload(plaintext);
                                            drogon::orm::Mapper<PayPaymentModel> paymentUpdater(
                                              transPtr
                                            );
                                            LOG_INFO << "[CallbackService] About to update payment "
                                                        "record for order: "
                                                     << orderNo;
                                            paymentUpdater.update(
                                              payment,
                                              [cbPtr,
                                               orderStatus,
                                               paymentNo,
                                               transDb,
                                               orderNo,
                                               order,
                                               transPtr](const size_t) mutable {
                                                  LOG_INFO << "[CallbackService] Payment update "
                                                              "callback fired for order: "
                                                           << orderNo;
                                                  drogon::orm::Mapper<PayOrderModel> orderUpdater(
                                                    transPtr
                                                  );
                                                  // Update order fields
                                                  order.setStatus(orderStatus);
                                                  LOG_INFO << "[CallbackService] About to update "
                                                              "order record for order: "
                                                           << orderNo
                                                           << ", status: " << orderStatus;
                                                  orderUpdater.update(
                                                    order,
                                                    [cbPtr,
                                                     orderStatus,
                                                     paymentNo,
                                                     transDb,
                                                     orderNo,
                                                     order,
                                                     transPtr](const size_t) {
                                                        LOG_INFO
                                                          << "[CallbackService] Order updated "
                                                             "successfully for order: "
                                                          << orderNo
                                                          << ", preparing final response";
                                                        if (orderStatus == "PAID")
                                                        {
                                                            insertLedgerEntry(
                                                              transDb,
                                                              order.getValueOfUserId(),
                                                              orderNo,
                                                              paymentNo,
                                                              "PAYMENT",
                                                              order.getValueOfAmount(),
                                                              [cbPtr, orderNo, transPtr]() {
                                                                  LOG_INFO
                                                                    << "[CallbackService] Manually "
                                                                       "committing transaction for "
                                                                       "order: "
                                                                    << orderNo;
                                                                  transPtr->execSqlAsync(
                                                                    "COMMIT",
                                                                    [cbPtr, orderNo](
                                                                      const drogon::orm::Result &
                                                                    ) {
                                                                        LOG_INFO
                                                                          << "[CallbackService] "
                                                                             "Transaction "
                                                                             "committed, calling "
                                                                             "final success "
                                                                             "callback for order: "
                                                                          << orderNo;
                                                                        Json::Value ok;
                                                                        ok["code"] = "SUCCESS";
                                                                        ok["message"] = "OK";
                                                                        (*cbPtr)(
                                                                          ok, std::error_code()
                                                                        );
                                                                    },
                                                                    [cbPtr, orderNo](
                                                                      const drogon::orm::
                                                                        DrogonDbException &e
                                                                    ) {
                                                                        LOG_ERROR
                                                                          << "[CallbackService] "
                                                                             "Failed to commit "
                                                                             "transaction for "
                                                                             "order: "
                                                                          << orderNo << ", error: "
                                                                          << e.base().what();
                                                                        Json::Value error;
                                                                        error["code"] = "FAIL";
                                                                        error["message"] =
                                                                          "Failed to commit "
                                                                          "transaction";
                                                                        (*cbPtr)(
                                                                          error,
                                                                          pay::makePayError(
                                                                            1400,
                                                                            "db transaction "
                                                                            "unavailable"
                                                                          )
                                                                        );
                                                                    }
                                                                  );
                                                              }
                                                            );
                                                        }
                                                        else
                                                        {
                                                            LOG_INFO << "[CallbackService] "
                                                                        "Manually committing "
                                                                        "transaction for order: "
                                                                     << orderNo;
                                                            transPtr->execSqlAsync(
                                                              "COMMIT",
                                                              [cbPtr, orderNo](
                                                                const drogon::orm::Result &
                                                              ) {
                                                                  LOG_INFO
                                                                    << "[CallbackService] "
                                                                       "Transaction committed, "
                                                                       "calling final success "
                                                                       "callback for order: "
                                                                    << orderNo;
                                                                  Json::Value ok;
                                                                  ok["code"] = "SUCCESS";
                                                                  ok["message"] = "OK";
                                                                  (*cbPtr)(ok, std::error_code());
                                                              },
                                                              [cbPtr, orderNo](
                                                                const drogon::orm::DrogonDbException
                                                                  &e
                                                              ) {
                                                                  LOG_ERROR
                                                                    << "[CallbackService] Failed "
                                                                       "to commit transaction for "
                                                                       "order: "
                                                                    << orderNo << ", error: "
                                                                    << e.base().what();
                                                                  Json::Value error;
                                                                  error["code"] = "FAIL";
                                                                  error["message"] =
                                                                    "Failed to commit transaction";
                                                                  (*cbPtr)(
                                                                    error,
                                                                    pay::makePayError(
                                                                      1400,
                                                                      "db transaction unavailable"
                                                                    )
                                                                  );
                                                              }
                                                            );
                                                        }
                                                    },
                                                    [cbPtr, orderNo](
                                                      const drogon::orm::DrogonDbException &e
                                                    ) {
                                                        LOG_ERROR << "[CallbackService] Order "
                                                                     "update failed for order: "
                                                                  << orderNo
                                                                  << ", error: " << e.base().what();
                                                        Json::Value error;
                                                        error["code"] = "FAIL";
                                                        error["message"] =
                                                          std::string("db error: ") +
                                                          e.base().what();
                                                        (*cbPtr)(
                                                          error,
                                                          pay::makePayError(
                                                            1400, "db transaction unavailable"
                                                          )
                                                        );
                                                    }
                                                  );
                                              },
                                              respondDbError
                                            );
                                        },
                                        respondDbError
                                      );
                                  },
                                  respondDbError
                                );
                            },
                            respondDbError
                          );
                    });
                },
                [cbPtr, idempotencyKey](const drogon::orm::DrogonDbException &e) {
                    // Duplicate key = already processed by concurrent call
                    LOG_INFO << "[CallbackService] Idempotency insert failed for key: "
                             << idempotencyKey << ", error: " << e.base().what();
                    Json::Value ok;
                    ok["code"] = "SUCCESS";
                    ok["message"] = "OK";
                    (*cbPtr)(ok, std::error_code());
                }
              );
          }
        );
    };

    // Skip Redis idempotency for now (simpler implementation)
    LOG_INFO << "[CallbackService] About to call proceedWithDb() for order: " << orderNo;
    proceedWithDb();
}

void CallbackService::handleRefundCallback(
  const std::string &body,
  const std::string &signature,
  const std::string &timestamp,
  const std::string &nonce,
  const std::string &serialNo,
  CallbackResult &&callback
)
{
    if (!wechatClient_)
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "wechat client not ready";
        callback(error, std::error_code(1400, std::system_category()));
        return;
    }

    auto respond = [callback](const Json::Value &result, const std::string &errorMsg) {
        if (!errorMsg.empty())
        {
            Json::Value error;
            error["code"] = "FAIL";
            error["message"] = errorMsg;
            callback(error, std::error_code(1400, std::system_category()));
            return;
        }
        callback(result, std::error_code());
    };

    // Verify signature first
    if (!verifySignature(body, signature, timestamp, nonce, serialNo))
    {
        LOG_WARN << "[CallbackService] Signature verification failed";
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "signature verification failed";
        callback(error, std::error_code(1400, std::system_category()));
        return;
    }
    LOG_INFO << "[CallbackService] Signature verified successfully";

    // Parse callback body
    Json::CharReaderBuilder builder;
    Json::Value notifyJson;
    std::string parseErrors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(body.data(), body.data() + body.size(), &notifyJson, &parseErrors))
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid json";
        respond(error, "invalid json");
        return;
    }

    // Validate event_type
    const std::string eventType = notifyJson.get("event_type", "").asString();
    if (eventType.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing event_type";
        respond(error, "missing event_type");
        return;
    }

    if (eventType.rfind("REFUND.", 0) != 0)
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid refund event_type";
        respond(error, "invalid refund event_type");
        return;
    }

    // Validate resource
    if (!notifyJson.isMember("resource"))
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing resource";
        callback(error, std::error_code(1400, std::system_category()));
        return;
    }

    const auto &resource = notifyJson["resource"];
    const std::string resourceType = notifyJson.get("resource_type", "").asString();
    if (resourceType.empty() || resourceType != "encrypt-resource")
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "unsupported resource_type";
        respond(error, "unsupported resource_type");
        return;
    }

    const std::string algorithm = resource.get("algorithm", "").asString();
    if (algorithm.empty() || algorithm != "AEAD_AES_256_GCM")
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "unsupported resource algorithm";
        respond(error, "unsupported resource algorithm");
        return;
    }

    const std::string ciphertext = resource.get("ciphertext", "").asString();
    const std::string nonceStr = resource.get("nonce", "").asString();
    const std::string associatedData = resource.get("associated_data", "").asString();
    if (ciphertext.empty() || nonceStr.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid resource";
        respond(error, "invalid resource");
        return;
    }

    if (associatedData != "refund")
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid refund associated_data";
        respond(error, "invalid refund associated_data");
        return;
    }

    // Decrypt resource
    LOG_INFO << "[CallbackService] Decrypting resource...";
    std::string plaintext;
    std::string decryptError;
    if (!wechatClient_
           ->decryptResource(ciphertext, nonceStr, associatedData, plaintext, decryptError))
    {
        LOG_WARN << "[CallbackService] Decryption failed: " << decryptError;
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = decryptError;
        respond(error, decryptError);
        return;
    }
    LOG_INFO << "[CallbackService] Decryption successful";

    // Parse decrypted JSON
    Json::Value plainJson;
    Json::CharReaderBuilder plainBuilder;
    std::string plainErrors;
    std::unique_ptr<Json::CharReader> plainReader(plainBuilder.newCharReader());
    if (!plainReader
           ->parse(plaintext.data(), plaintext.data() + plaintext.size(), &plainJson, &plainErrors))
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "invalid resource json";
        respond(error, "invalid resource json");
        return;
    }

    // Validate appid and mchid
    const std::string appId = plainJson.get("appid", "").asString();
    const std::string mchId = plainJson.get("mchid", "").asString();
    if (!appId.empty() && wechatClient_ && appId != wechatClient_->getAppId())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "appid mismatch";
        respond(error, "appid mismatch");
        return;
    }
    if (!mchId.empty() && wechatClient_ && mchId != wechatClient_->getMchId())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "mchid mismatch";
        respond(error, "mchid mismatch");
        return;
    }

    // Extract refund details
    const std::string refundNo = plainJson.get("out_refund_no", "").asString();
    const std::string refundStatusRaw = plainJson.get("refund_status", "").asString();
    const std::string refundId = plainJson.get("refund_id", "").asString();

    if (refundNo.empty() || refundStatusRaw.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing refund_no/refund_status";
        respond(error, "missing refund_no/refund_status");
        return;
    }

    if (refundStatusRaw == "SUCCESS" && refundId.empty())
    {
        Json::Value error;
        error["code"] = "FAIL";
        error["message"] = "missing refund_id";
        respond(error, "missing refund_id");
        return;
    }

    // Idempotency check
    std::string idempotencyKey = notifyJson.get("id", "").asString();
    if (idempotencyKey.empty())
    {
        idempotencyKey = refundNo + ":" + refundStatusRaw;
    }

    auto cbPtr = std::make_shared<CallbackResult>(std::move(callback));

    auto proceedRefundDb = [this,
                            cbPtr,
                            idempotencyKey,
                            refundNo,
                            refundStatusRaw,
                            refundId,
                            signature,
                            serialNo,
                            plaintext,
                            body,
                            plainJson]() {
        drogon::orm::Mapper<PayIdempotencyModel> idempMapper(dbClient_);
        auto idempCriteria = drogon::orm::Criteria(
          PayIdempotencyModel::Cols::_idempotency_key,
          drogon::orm::CompareOperator::EQ,
          idempotencyKey
        );
        idempMapper.findOne(
          idempCriteria,
          [this, cbPtr, refundNo, body, signature, serialNo, plainJson](
            const PayIdempotencyModel &
          ) {
              // Already processed - record callback and return success
              LOG_INFO << "[CallbackService] Refund idempotency key found for refund: " << refundNo
                       << ", recording callback";

              auto respondSuccess = [cbPtr]() {
                  Json::Value ok;
                  ok["code"] = "SUCCESS";
                  ok["message"] = "OK";
                  (*cbPtr)(ok, std::error_code());
              };

              auto respondDbError = [cbPtr](const drogon::orm::DrogonDbException &e) {
                  LOG_ERROR << "[CallbackService] DB error recording idempotent refund callback: "
                            << e.base().what();
                  Json::Value error;
                  error["code"] = "FAIL";
                  error["message"] = std::string("db error: ") + e.base().what();
                  (*cbPtr)(error, pay::makePayError(1400, "db transaction unavailable"));
              };

              // Look up payment via out_trade_no from the decrypted plaintext
              const std::string tradeOrderNo = plainJson.get("out_trade_no", "").asString();
              if (tradeOrderNo.empty())
              {
                  LOG_ERROR
                    << "[CallbackService] Missing out_trade_no in idempotent refund callback";
                  Json::Value error;
                  error["code"] = "FAIL";
                  error["message"] = "missing out_trade_no";
                  (*cbPtr)(error, pay::makePayError(1400, "missing out_trade_no"));
                  return;
              }

              drogon::orm::Mapper<PayPaymentModel> paymentLookup(dbClient_);
              paymentLookup.findOne(
                drogon::orm::Criteria(
                  PayPaymentModel::Cols::_order_no, drogon::orm::CompareOperator::EQ, tradeOrderNo
                ),
                [this, cbPtr, body, signature, serialNo, respondSuccess, respondDbError](
                  const PayPaymentModel &payment
                ) {
                    const std::string paymentNo = payment.getValueOfPaymentNo();

                    drogon::orm::Mapper<PayCallbackModel> callbackMapper(dbClient_);
                    PayCallbackModel callbackRow;
                    callbackRow.setPaymentNo(paymentNo);
                    callbackRow.setRawBody(body);
                    callbackRow.setSignature(signature);
                    callbackRow.setSerialNo(serialNo);
                    callbackRow.setVerified(true);
                    callbackRow.setProcessed(true);
                    callbackRow.setReceivedAt(trantor::Date::now());

                    callbackMapper.insert(
                      callbackRow,
                      [respondSuccess](const PayCallbackModel &) { respondSuccess(); },
                      respondDbError
                    );
                },
                [cbPtr, respondDbError](const drogon::orm::DrogonDbException &e) {
                    LOG_ERROR
                      << "[CallbackService] Payment not found during idempotent refund callback: "
                      << e.base().what();
                    respondDbError(e);
                }
              );
          },
          [this,
           cbPtr,
           idempotencyKey,
           refundNo,
           refundStatusRaw,
           refundId,
           signature,
           serialNo,
           plaintext,
           body,
           plainJson](const drogon::orm::DrogonDbException &) {
              const std::string requestHash = drogon::utils::getMd5(body);
              PayIdempotencyModel idemp;
              idemp.setIdempotencyKey(idempotencyKey);
              idemp.setRequestHash(requestHash);
              idemp.setResponseSnapshot(plaintext);
              const auto now = trantor::Date::now();
              const auto expiresAt = trantor::Date(
                now.microSecondsSinceEpoch() + static_cast<int64_t>(7) * 24 * 60 * 60 * 1000000
              );
              idemp.setExpireAt(expiresAt);

              drogon::orm::Mapper<PayIdempotencyModel> idempInsert(dbClient_);
              idempInsert.insert(
                idemp,
                [this,
                 cbPtr,
                 refundNo,
                 refundStatusRaw,
                 refundId,
                 signature,
                 serialNo,
                 plaintext,
                 body,
                 plainJson](const PayIdempotencyModel &) {
                    const std::string refundStatus = pay::utils::mapRefundStatus(refundStatusRaw);
                    if (refundStatus.empty())
                    {
                        Json::Value error;
                        error["code"] = "FAIL";
                        error["message"] = "invalid refund status";
                        (*cbPtr)(error, pay::makePayError(1400, "db transaction unavailable"));
                        return;
                    }

                    drogon::orm::Mapper<PayRefundModel> refundMapper(dbClient_);
                    auto refundCriteria = drogon::orm::Criteria(
                      PayRefundModel::Cols::_refund_no, drogon::orm::CompareOperator::EQ, refundNo
                    );
                    refundMapper.findOne(
                      refundCriteria,
                      [this,
                       cbPtr,
                       refundStatus,
                       refundId,
                       signature,
                       serialNo,
                       refundNo,
                       body,
                       plaintext,
                       plainJson](PayRefundModel refund) {
                          // Already successful - return success
                          if (refund.getValueOfStatus() == "REFUND_SUCCESS")
                          {
                              Json::Value ok;
                              ok["code"] = "SUCCESS";
                              ok["message"] = "OK";
                              (*cbPtr)(ok, std::error_code());
                              return;
                          }

                          const auto orderNo = refund.getValueOfOrderNo();
                          const auto paymentNo = refund.getValueOfPaymentNo();
                          const auto refundAmount = refund.getValueOfAmount();
                          const auto &amountJson = plainJson["amount"];
                          const std::string notifyCurrency =
                            amountJson.get("currency", "").asString();
                          const int64_t notifyRefundFen = amountJson.get("refund", 0).asInt64();
                          int64_t refundTotalFen = 0;
                          if (
                            !pay::utils::parseAmountToFen(refundAmount, refundTotalFen) ||
                            notifyRefundFen <= 0
                          )
                          {
                              Json::Value error;
                              error["code"] = "FAIL";
                              error["message"] = "invalid refund amount in callback";
                              (*cbPtr)(
                                error, pay::makePayError(1400, "db transaction unavailable")
                              );
                              return;
                          }
                          if (notifyRefundFen != refundTotalFen)
                          {
                              Json::Value error;
                              error["code"] = "FAIL";
                              error["message"] = "refund amount mismatch";
                              (*cbPtr)(
                                error, pay::makePayError(1400, "db transaction unavailable")
                              );
                              return;
                          }

                          drogon::orm::Mapper<PayOrderModel> orderMapper(dbClient_);
                          auto orderCriteria = drogon::orm::Criteria(
                            PayOrderModel::Cols::_order_no,
                            drogon::orm::CompareOperator::EQ,
                            orderNo
                          );
                          orderMapper.findOne(
                            orderCriteria,
                            [this,
                             cbPtr,
                             refundStatus,
                             refundId,
                             refundAmount,
                             orderNo,
                             paymentNo,
                             notifyCurrency,
                             signature,
                             serialNo,
                             refundNo,
                             body,
                             plaintext,
                             refund](const PayOrderModel &order) mutable {
                                const std::string orderCurrency = order.getValueOfCurrency();
                                if (!notifyCurrency.empty() && notifyCurrency != orderCurrency)
                                {
                                    Json::Value error;
                                    error["code"] = "FAIL";
                                    error["message"] = "refund currency mismatch";
                                    (*cbPtr)(
                                      error, pay::makePayError(1400, "db transaction unavailable")
                                    );
                                    return;
                                }

                                refund.setStatus(refundStatus);
                                refund.setChannelRefundNo(refundId);
                                dbClient_->newTransactionAsync([this,
                                                                cbPtr,
                                                                refundStatus,
                                                                refundAmount,
                                                                orderNo,
                                                                paymentNo,
                                                                order,
                                                                signature,
                                                                serialNo,
                                                                body,
                                                                refundNo,
                                                                plaintext,
                                                                refund](
                                                                 const std::shared_ptr<
                                                                   drogon::orm::Transaction>
                                                                   &transPtr
                                                               ) mutable {
                                    auto respondDbError =
                                      [cbPtr, transPtr](const drogon::orm::DrogonDbException &e) {
                                          transPtr->rollback();
                                          Json::Value error;
                                          error["code"] = "FAIL";
                                          error["message"] =
                                            std::string("db error: ") + e.base().what();
                                          (*cbPtr)(
                                            error,
                                            pay::makePayError(1400, "db transaction unavailable")
                                          );
                                      };

                                    drogon::orm::Mapper<PayRefundModel> refundUpdater(transPtr);
                                    refundUpdater.update(
                                      refund,
                                      [this,
                                       cbPtr,
                                       refundStatus,
                                       refundAmount,
                                       orderNo,
                                       paymentNo,
                                       order,
                                       signature,
                                       serialNo,
                                       body,
                                       refundNo,
                                       plaintext,
                                       transPtr](const size_t) {
                                          transPtr->execSqlAsync(
                                            "UPDATE pay_refund "
                                            "SET response_payload = $1 "
                                            "WHERE refund_no = $2",
                                            [](const drogon::orm::Result &) {},
                                            [cbPtr,
                                             transPtr](const drogon::orm::DrogonDbException &e) {
                                                transPtr->rollback();
                                                Json::Value error;
                                                error["code"] = "FAIL";
                                                error["message"] =
                                                  std::string("db error: ") + e.base().what();
                                                (*cbPtr)(
                                                  error,
                                                  pay::makePayError(
                                                    1400, "db transaction unavailable"
                                                  )
                                                );
                                            },
                                            plaintext,
                                            refundNo
                                          );

                                          // Lambda to insert callback record and call final
                                          // callback
                                          auto insertCallbackAndFinish = [cbPtr,
                                                                          transPtr,
                                                                          paymentNo,
                                                                          body,
                                                                          signature,
                                                                          serialNo]() {
                                              PayCallbackModel callbackRow;
                                              callbackRow.setPaymentNo(paymentNo);
                                              callbackRow.setRawBody(body);
                                              callbackRow.setSignature(signature);
                                              callbackRow.setSerialNo(serialNo);
                                              callbackRow.setVerified(true);
                                              callbackRow.setProcessed(true);
                                              callbackRow.setReceivedAt(trantor::Date::now());

                                              drogon::orm::Mapper<PayCallbackModel> callbackMapper(
                                                transPtr
                                              );
                                              callbackMapper.insert(
                                                callbackRow,
                                                [cbPtr, transPtr](const PayCallbackModel &) {
                                                    LOG_INFO
                                                      << "[CallbackService] Manually committing "
                                                         "transaction for refund callback";
                                                    transPtr->execSqlAsync(
                                                      "COMMIT",
                                                      [cbPtr](const drogon::orm::Result &) {
                                                          LOG_INFO
                                                            << "[CallbackService] Transaction "
                                                               "committed, calling final success "
                                                               "callback for refund";
                                                          Json::Value ok;
                                                          ok["code"] = "SUCCESS";
                                                          ok["message"] = "OK";
                                                          (*cbPtr)(ok, std::error_code());
                                                      },
                                                      [cbPtr](
                                                        const drogon::orm::DrogonDbException &e
                                                      ) {
                                                          LOG_ERROR
                                                            << "[CallbackService] Failed to commit "
                                                               "transaction for refund, error: "
                                                            << e.base().what();
                                                          Json::Value error;
                                                          error["code"] = "FAIL";
                                                          error["message"] =
                                                            "Failed to commit transaction";
                                                          (*cbPtr)(
                                                            error,
                                                            pay::makePayError(
                                                              1400, "db transaction unavailable"
                                                            )
                                                          );
                                                      }
                                                    );
                                                },
                                                [cbPtr, transPtr](
                                                  const drogon::orm::DrogonDbException &e
                                                ) {
                                                    transPtr->rollback();
                                                    Json::Value error;
                                                    error["code"] = "FAIL";
                                                    error["message"] =
                                                      std::string("db error: ") + e.base().what();
                                                    (*cbPtr)(
                                                      error,
                                                      pay::makePayError(
                                                        1400, "db transaction unavailable"
                                                      )
                                                    );
                                                }
                                              );
                                          };

                                          if (refundStatus == "REFUND_SUCCESS")
                                          {
                                              auto transDb =
                                                std::static_pointer_cast<drogon::orm::DbClient>(
                                                  transPtr
                                                );
                                              insertLedgerEntry(
                                                transDb,
                                                order.getValueOfUserId(),
                                                orderNo,
                                                paymentNo,
                                                "REFUND",
                                                refundAmount,
                                                insertCallbackAndFinish
                                              );
                                          }
                                          else
                                          {
                                              insertCallbackAndFinish();
                                          }
                                      },
                                      respondDbError
                                    );
                                });
                            },
                            [cbPtr](const drogon::orm::DrogonDbException &e) {
                                Json::Value error;
                                error["code"] = "FAIL";
                                error["message"] =
                                  std::string("order not found: ") + e.base().what();
                                (*cbPtr)(error, std::error_code(1404, std::system_category()));
                            }
                          );
                      },
                      [cbPtr](const drogon::orm::DrogonDbException &e) {
                          Json::Value error;
                          error["code"] = "FAIL";
                          error["message"] = std::string("refund not found: ") + e.base().what();
                          (*cbPtr)(error, std::error_code(1404, std::system_category()));
                      }
                    );
                },
                [cbPtr](const drogon::orm::DrogonDbException &e) {
                    Json::Value error;
                    error["code"] = "FAIL";
                    error["message"] = std::string("db error: ") + e.base().what();
                    (*cbPtr)(error, pay::makePayError(1400, "db transaction unavailable"));
                }
              );
          }
        );
    };

    // Skip Redis idempotency for now (simpler implementation)
    proceedRefundDb();
}

bool CallbackService::verifySignature(
  const std::string &body,
  const std::string &signature,
  const std::string &timestamp,
  const std::string &nonce,
  const std::string &serialNo
)
{
    if (!wechatClient_)
    {
        LOG_ERROR << "WechatPayClient is null";
        return false;
    }

    std::string verifyError;
    if (!wechatClient_->verifyCallback(timestamp, nonce, body, signature, serialNo, verifyError))
    {
        LOG_WARN << "Signature verification failed: " << verifyError;
        return false;
    }

    return true;
}
