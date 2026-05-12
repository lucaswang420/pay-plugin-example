#include "HealthCheckController.h"
#include "../plugins/PayPlugin.h"
#include <drogon/orm/DbClient.h>
#include <drogon/nosql/RedisClient.h>
#include <json/json.h>
#include <chrono>

void HealthCheckController::health(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    if (req->method() == Options)
    {
        auto resp = HttpResponse::newHttpResponse();
        callback(resp);
        return;
    }

    Json::Value response;
    bool allHealthy = true;

    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto timestamp =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    response["timestamp"] = static_cast<Json::Int64>(timestamp);

    // Check database connectivity
    Json::Value services;
    try
    {
        auto dbClient = drogon::app().getDbClient();

        if (dbClient)
        {
            // Database client exists - mark as ok
            // In production, you might want to do an actual query
            services["database"] = "ok";
        }
        else
        {
            services["database"] = "error: No database client";
            allHealthy = false;
        }
    }
    catch (const std::exception &e)
    {
        services["database"] = "error: " + std::string(e.what());
        allHealthy = false;
    }

    // Check Redis connectivity
    try
    {
        auto redisClient = drogon::app().getRedisClient();

        if (redisClient)
        {
            // Redis client exists - mark as ok
            services["redis"] = "ok";
        }
        else
        {
            // Redis is optional - mark as not configured but not a failure
            services["redis"] = "not_configured";
        }
    }
    catch (const std::exception &)
    {
        // Redis is optional - don't fail health check
        services["redis"] = "not_configured";
    }

    // Set overall status
    response["services"] = services;
    response["status"] = allHealthy ? "healthy" : "unhealthy";

    // Create response
    auto resp = HttpResponse::newHttpJsonResponse(response);

    // Set HTTP status code
    if (allHealthy)
    {
        resp->setStatusCode(k200OK);
    }
    else
    {
        resp->setStatusCode(k503ServiceUnavailable);
    }

    callback(resp);
}
