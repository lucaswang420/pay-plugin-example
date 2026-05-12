#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class HealthCheckController : public drogon::HttpController<HealthCheckController>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HealthCheckController::health, "/health", Get, Options);
    METHOD_LIST_END

    void health(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback);
};
