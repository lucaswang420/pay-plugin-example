#pragma once

#include <drogon/HttpController.h>
#include "../plugins/PayPlugin.h"

using namespace drogon;

class AlipayCallbackController : public drogon::HttpController<AlipayCallbackController>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AlipayCallbackController::notify, "/api/pay/notify/alipay", Post);
    METHOD_LIST_END

    void notify(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);
};
