#include "WechatCallbackController.h"
#include "../services/CallbackService.h"

void WechatCallbackController::notify(
  const HttpRequestPtr &req,
  std::function<void(const HttpResponsePtr &)> &&callback
)
{
    // Extract callback parameters from request
    std::string body = std::string(req->body());
    std::string signature = std::string(req->getHeader("Wechatpay-Signature"));
    std::string timestamp = std::string(req->getHeader("Wechatpay-Timestamp"));
    std::string nonce = std::string(req->getHeader("Wechatpay-Nonce"));
    std::string serialNo = std::string(req->getHeader("Wechatpay-Serial"));

    // Get CallbackService from Plugin
    auto plugin = drogon::app().getPlugin<PayPlugin>();
    auto callbackService = plugin->callbackService();

    // Route to appropriate callback handler based on event_type
    // Parse body to determine callback type
    Json::Value bodyJson;
    std::string eventType;

    // Use CharReaderBuilder instead of deprecated Reader
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errors;
    const char *str = body.c_str();
    if (
      reader->parse(str, str + body.length(), &bodyJson, &errors) && bodyJson.isMember("event_type")
    )
    {
        eventType = bodyJson["event_type"].asString();
    }

    // Route to payment or refund callback handler
    if (eventType.find("REFUND") != std::string::npos)
    {
        // Handle refund callback
        callbackService->handleRefundCallback(
          body,
          signature,
          timestamp,
          nonce,
          serialNo,
          [callback](const Json::Value &result, const std::error_code &error) {
              auto resp = drogon::HttpResponse::newHttpJsonResponse(result);
              if (error)
              {
                  resp->setStatusCode(drogon::k500InternalServerError);
              }
              callback(resp);
          }
        );
    }
    else
    {
        // Handle payment callback (default)
        callbackService->handlePaymentCallback(
          body,
          signature,
          timestamp,
          nonce,
          serialNo,
          [callback](const Json::Value &result, const std::error_code &error) {
              auto resp = drogon::HttpResponse::newHttpJsonResponse(result);
              if (error)
              {
                  resp->setStatusCode(drogon::k500InternalServerError);
              }
              callback(resp);
          }
        );
    }
}
