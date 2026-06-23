#include <drogon/drogon.h>
#include "utils/ConfigLoader.h"
#include "utils/StartupValidator.h"
#include <fstream>
#include <json/json.h>
#include <string>

using namespace drogon;

void setupCors()
{
    auto isAllowed = [](const std::string &origin) -> bool {
        if (origin.empty())
            return false;

        const auto &customConfig = drogon::app().getCustomConfig();
        const auto &allowOrigins = customConfig["cors"]["allow_origins"];

        if (allowOrigins.isArray())
        {
            for (const auto &allowed : allowOrigins)
            {
                if (allowed.asString() == origin)
                    return true;
            }
        }
        return false;
    };

    drogon::app().registerSyncAdvice(
      [isAllowed](const drogon::HttpRequestPtr &req) -> drogon::HttpResponsePtr {
          if (req->method() == drogon::HttpMethod::Options)
          {
              const auto &origin = req->getHeader("Origin");
              if (isAllowed(origin))
              {
                  auto resp = drogon::HttpResponse::newHttpResponse();
                  resp->addHeader("Access-Control-Allow-Origin", origin);

                  const auto &requestMethod =
                    req->getHeader("Access-Control-Request-Method");
                  if (!requestMethod.empty())
                  {
                      resp->addHeader(
                        "Access-Control-Allow-Methods", requestMethod
                      );
                  }

                  resp->addHeader("Access-Control-Allow-Credentials", "true");

                  const auto &requestHeaders =
                    req->getHeader("Access-Control-Request-Headers");
                  if (!requestHeaders.empty())
                  {
                      resp->addHeader(
                        "Access-Control-Allow-Headers", requestHeaders
                      );
                  }
                  return resp;
              }
          }
          return {};
      }
    );

    drogon::app().registerPostHandlingAdvice(
      [isAllowed](const drogon::HttpRequestPtr &req,
                  const drogon::HttpResponsePtr &resp) {
          const auto &origin = req->getHeader("Origin");
          if (isAllowed(origin))
          {
              resp->addHeader("Access-Control-Allow-Origin", origin);
              resp->addHeader(
                "Access-Control-Allow-Methods", "GET, POST, OPTIONS"
              );
              resp->addHeader(
                "Access-Control-Allow-Headers",
                "Content-Type, Authorization"
              );
              resp->addHeader("Access-Control-Allow-Credentials", "true");
          }
      }
    );
}

int main()
{
    // 1. Load .env file into process environment
    ConfigLoader::loadEnvFile(".env");

    // 2. Validate required environment variables
    // PAY_REDIS_PASSWORD is intentionally optional: the bundled docker-compose
    // runs Redis without auth, and many deployments use a no-auth Redis. Only
    // the DB password and API key are mandatory.
    StartupValidator::validate({"PAY_DB_PASSWORD", "PAY_API_KEY"});

    // 3. Read config.json and replace __env_var:XXX__ placeholders
    std::ifstream configFile("./config.json");
    if (!configFile.is_open())
    {
        LOG_ERROR << "Failed to open config.json";
        return 1;
    }
    Json::Value config;
    Json::CharReaderBuilder builder;
    std::string errors;
    if (!Json::parseFromStream(builder, configFile, &config, &errors))
    {
        LOG_ERROR << "Failed to parse config.json: " << errors;
        return 1;
    }
    Json::Value processedConfig = ConfigLoader::loadConfig(config);

    // 4. Load processed config into Drogon
    drogon::app().loadConfigJson(std::move(processedConfig));
    setupCors();
    drogon::app().run();
    return 0;
}
