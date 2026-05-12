#pragma once

#include <drogon/HttpController.h>
#include "../plugins/PayPlugin.h"

using namespace drogon;

class PayController : public drogon::HttpController<PayController>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(PayController::createPayment, "/api/pay/create", Post, Options, "PayAuthFilter");
    ADD_METHOD_TO(
      PayController::createQRPayment,
      "/api/qrpay/create",
      Post,
      Options,
      "PayAuthFilter"
    );
    ADD_METHOD_TO(PayController::queryOrder, "/api/pay/query", Get, Options, "PayAuthFilter");
    ADD_METHOD_TO(PayController::refund, "/api/pay/refund", Post, Options, "PayAuthFilter");
    ADD_METHOD_TO(
      PayController::queryRefund,
      "/api/pay/refund/query",
      Get,
      Options,
      "PayAuthFilter"
    );
    ADD_METHOD_TO(PayController::queryOrderList, "/api/pay/orders", Get, Options, "PayAuthFilter");
    ADD_METHOD_TO(
      PayController::reconcileSummary,
      "/api/pay/reconcile/summary",
      Get,
      Options,
      "PayAuthFilter"
    );
    METHOD_LIST_END

    void createPayment(
      const HttpRequestPtr &req,
      std::function<void(const HttpResponsePtr &)> &&callback
    );

    void createQRPayment(
      const HttpRequestPtr &req,
      std::function<void(const HttpResponsePtr &)> &&callback
    );

    void queryOrder(
      const HttpRequestPtr &req,
      std::function<void(const HttpResponsePtr &)> &&callback
    );

    void refund(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);

    void queryRefund(
      const HttpRequestPtr &req,
      std::function<void(const HttpResponsePtr &)> &&callback
    );

    void queryOrderList(
      const HttpRequestPtr &req,
      std::function<void(const HttpResponsePtr &)> &&callback
    );

    void reconcileSummary(
      const HttpRequestPtr &req,
      std::function<void(const HttpResponsePtr &)> &&callback
    );
};
