#pragma once

#include <drogon/HttpController.h>
#include <atomic>

using namespace drogon;

class HealthCheckController : public drogon::HttpController<HealthCheckController>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HealthCheckController::healthz, "/healthz", Get, Options);
    ADD_METHOD_TO(HealthCheckController::readyz, "/readyz", Get, Options);
    ADD_METHOD_TO(HealthCheckController::health, "/health", Get, Options);
    METHOD_LIST_END

    void healthz(
      const HttpRequestPtr &req,
      std::function<void(const HttpResponsePtr &)> &&callback
    );

    void readyz(
      const HttpRequestPtr &req,
      std::function<void(const HttpResponsePtr &)> &&callback
    );

    void health(
      const HttpRequestPtr &req,
      std::function<void(const HttpResponsePtr &)> &&callback
    );

  private:
    std::atomic<int> consecutiveFailures_{0};
    std::atomic<bool> lastReadyState_{true};
    static constexpr int FAILURE_THRESHOLD = 3;
};
