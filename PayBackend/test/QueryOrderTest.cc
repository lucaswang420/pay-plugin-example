#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/utils/Utilities.h>
#include "../models/PayOrder.h"
#include "../models/PayPayment.h"
#include "../plugins/PayPlugin.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>

namespace
{
bool loadConfig(Json::Value &root)
{
    const auto cwd = std::filesystem::current_path();
    const std::vector<std::filesystem::path> candidates =
      {cwd / "config.json",
       cwd / "test" / "Release" / "config.json",
       cwd / "test" / "Debug" / "config.json",
       cwd / "Release" / "config.json",
       cwd / "Debug" / "config.json",
       cwd.parent_path() / "config.json",
       cwd.parent_path() / "test" / "Release" / "config.json",
       cwd.parent_path() / "test" / "Debug" / "config.json",
       cwd.parent_path() / "Release" / "config.json",
       cwd.parent_path() / "Debug" / "config.json"};

    std::filesystem::path configPath;
    for (const auto &candidate : candidates)
    {
        if (std::filesystem::exists(candidate))
        {
            configPath = candidate;
            break;
        }
    }

    if (configPath.empty())
    {
        return false;
    }

    std::ifstream in(configPath.string());
    if (!in)
    {
        return false;
    }

    Json::CharReaderBuilder builder;
    std::string errors;
    const bool ok = Json::parseFromStream(builder, in, &root, &errors);
    return ok;
}

std::string buildPgConnInfo(const Json::Value &db)
{
    const std::string host = db.get("host", "").asString();
    const int port = db.get("port", 5432).asInt();
    const std::string dbname = db.get("dbname", "").asString();
    const std::string user = db.get("user", "").asString();
    const std::string passwd = db.get("passwd", "").asString();

    std::string connInfo =
      "host=" + host + " port=" + std::to_string(port) + " dbname=" + dbname + " user=" + user;
    if (!passwd.empty())
    {
        connInfo += " password=" + passwd;
    }
    return connInfo;
}

}  // namespace

DROGON_TEST(PayPlugin_QueryOrder_NoWechatClient)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) NOT NULL UNIQUE,"
      "user_id BIGINT NOT NULL,"
      "amount DECIMAL(18,2) NOT NULL,"
      "currency VARCHAR(16) NOT NULL,"
      "status VARCHAR(24) NOT NULL,"
      "channel VARCHAR(16) NOT NULL,"
      "title VARCHAR(128) NOT NULL,"
      "created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string amount = "19.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(20001);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Query Order");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    PayPlugin plugin;
    plugin.setTestClients(nullptr, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->queryOrder(
      orderNo,
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();

    CHECK(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto error = errorFuture.get();
    CHECK(!error);

    const auto result = resultFuture.get();
    CHECK(result.isMember("data"));
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["amount"].asString() == amount);
    CHECK(result["data"]["currency"].asString() == "CNY");
    CHECK(result["data"]["status"].asString() == "PAYING");
    CHECK(result["data"]["channel"].asString() == "wechat");
    CHECK(result["data"]["title"].asString() == "Query Order");

    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryOrder_WechatQueryError)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) NOT NULL UNIQUE,"
      "user_id BIGINT NOT NULL,"
      "amount DECIMAL(18,2) NOT NULL,"
      "currency VARCHAR(16) NOT NULL,"
      "status VARCHAR(24) NOT NULL,"
      "channel VARCHAR(16) NOT NULL,"
      "title VARCHAR(128) NOT NULL,"
      "created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string amount = "29.99";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(20002);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Query Order Error");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    wechatConfig["app_id"] = "wx_app";
    wechatConfig["mch_id"] = "";
    wechatConfig["notify_url"] = "https://notify.invalid";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->queryOrder(
      orderNo,
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();

    CHECK(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto error = errorFuture.get();
    const auto result = resultFuture.get();

    // Should successfully return order data from database
    // even though WeChat query will fail due to invalid config
    CHECK(result.isMember("data"));
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["status"].asString() == "PAYING");

    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryOrder_WechatSuccess)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) NOT NULL UNIQUE,"
      "user_id BIGINT NOT NULL,"
      "amount DECIMAL(18,2) NOT NULL,"
      "currency VARCHAR(16) NOT NULL,"
      "status VARCHAR(24) NOT NULL,"
      "channel VARCHAR(16) NOT NULL,"
      "title VARCHAR(128) NOT NULL,"
      "created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL UNIQUE,"
      "channel_trade_no VARCHAR(64),"
      "status VARCHAR(24) NOT NULL,"
      "amount DECIMAL(18,2) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "49.90";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(20003);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Query Order Success");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->queryOrder(
      orderNo,
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();

    CHECK(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto error = errorFuture.get();
    CHECK(!error);

    const auto result = resultFuture.get();
    CHECK(result.isMember("data"));
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["status"].asString() == "PAYING");
    CHECK(result["data"]["wechat_query_error"].asString().find("missing") != std::string::npos);

    // Order/payment status unchanged since WeChat query failed
    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryOrder_WechatSuccess_PaymentAlreadySuccess)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) NOT NULL UNIQUE,"
      "user_id BIGINT NOT NULL,"
      "amount DECIMAL(18,2) NOT NULL,"
      "currency VARCHAR(16) NOT NULL,"
      "status VARCHAR(24) NOT NULL,"
      "channel VARCHAR(16) NOT NULL,"
      "title VARCHAR(128) NOT NULL,"
      "created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL UNIQUE,"
      "channel_trade_no VARCHAR(64),"
      "status VARCHAR(24) NOT NULL,"
      "amount DECIMAL(18,2) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "59.90";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(20005);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Query Order Paid");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("SUCCESS");
    payment.setChannelTradeNo("wx_txn_prev");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->queryOrder(
      orderNo,
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();

    CHECK(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto error = errorFuture.get();
    CHECK(!error);

    const auto result = resultFuture.get();
    CHECK(result.isMember("data"));
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["status"].asString() == "PAYING");
    CHECK(result["data"]["wechat_query_error"].asString().find("missing") != std::string::npos);

    // Order/payment status unchanged since WeChat query failed
    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "SUCCESS");
    CHECK(updatedPayment.getValueOfChannelTradeNo() == "wx_txn_prev");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryOrder_WechatUserPaying)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) NOT NULL UNIQUE,"
      "user_id BIGINT NOT NULL,"
      "amount DECIMAL(18,2) NOT NULL,"
      "currency VARCHAR(16) NOT NULL,"
      "status VARCHAR(24) NOT NULL,"
      "channel VARCHAR(16) NOT NULL,"
      "title VARCHAR(128) NOT NULL,"
      "created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL UNIQUE,"
      "channel_trade_no VARCHAR(64),"
      "status VARCHAR(24) NOT NULL,"
      "amount DECIMAL(18,2) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "19.00";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(20004);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Query Order Paying");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->queryOrder(
      orderNo,
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();

    CHECK(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto error = errorFuture.get();
    CHECK(!error);

    const auto result = resultFuture.get();
    CHECK(result.isMember("data"));
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["status"].asString() == "PAYING");
    CHECK(result["data"]["wechat_query_error"].asString().find("missing") != std::string::npos);

    // Order/payment status unchanged since WeChat query failed
    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryOrder_WechatNotPay)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) NOT NULL UNIQUE,"
      "user_id BIGINT NOT NULL,"
      "amount DECIMAL(18,2) NOT NULL,"
      "currency VARCHAR(16) NOT NULL,"
      "status VARCHAR(24) NOT NULL,"
      "channel VARCHAR(16) NOT NULL,"
      "title VARCHAR(128) NOT NULL,"
      "created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL UNIQUE,"
      "channel_trade_no VARCHAR(64),"
      "status VARCHAR(24) NOT NULL,"
      "amount DECIMAL(18,2) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "9.50";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(20005);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Query Order Notpay");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->queryOrder(
      orderNo,
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();

    CHECK(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto error = errorFuture.get();
    CHECK(!error);

    const auto result = resultFuture.get();
    CHECK(result.isMember("data"));
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["status"].asString() == "PAYING");
    CHECK(result["data"]["wechat_query_error"].asString().find("missing") != std::string::npos);

    // Order/payment status unchanged since WeChat query failed
    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}

DROGON_TEST(PayPlugin_QueryOrder_WechatClosed)
{
    Json::Value root;
    CHECK(loadConfig(root));
    CHECK(root.isMember("db_clients"));
    CHECK(root["db_clients"].isArray());
    CHECK(!root["db_clients"].empty());

    const auto &db = root["db_clients"][0];
    const std::string connInfo = buildPgConnInfo(db);
    CHECK(!connInfo.empty());

    auto client = drogon::orm::DbClient::newPgClient(connInfo, 1);
    CHECK(client != nullptr);

    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_order ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) NOT NULL UNIQUE,"
      "user_id BIGINT NOT NULL,"
      "amount DECIMAL(18,2) NOT NULL,"
      "currency VARCHAR(16) NOT NULL,"
      "status VARCHAR(24) NOT NULL,"
      "channel VARCHAR(16) NOT NULL,"
      "title VARCHAR(128) NOT NULL,"
      "created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())"
    );
    client->execSqlSync(
      "CREATE TABLE IF NOT EXISTS pay_payment ("
      "id BIGSERIAL PRIMARY KEY,"
      "order_no VARCHAR(64) NOT NULL,"
      "payment_no VARCHAR(64) NOT NULL UNIQUE,"
      "channel_trade_no VARCHAR(64),"
      "status VARCHAR(24) NOT NULL,"
      "amount DECIMAL(18,2) NOT NULL,"
      "request_payload TEXT,"
      "response_payload TEXT,"
      "created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
      "updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())"
    );

    const std::string orderNo = "ord_" + drogon::utils::getUuid();
    const std::string paymentNo = "pay_" + drogon::utils::getUuid();
    const std::string amount = "23.00";

    using PayOrder = drogon_model::pay_test::PayOrder;
    drogon::orm::Mapper<PayOrder> orderMapper(client);
    PayOrder order;
    order.setOrderNo(orderNo);
    order.setUserId(20006);
    order.setAmount(amount);
    order.setCurrency("CNY");
    order.setStatus("PAYING");
    order.setChannel("wechat");
    order.setTitle("Query Order Closed");
    order.setCreatedAt(trantor::Date::now());
    order.setUpdatedAt(trantor::Date::now());
    orderMapper.insert(order);

    using PayPayment = drogon_model::pay_test::PayPayment;
    drogon::orm::Mapper<PayPayment> paymentMapper(client);
    PayPayment payment;
    payment.setOrderNo(orderNo);
    payment.setPaymentNo(paymentNo);
    payment.setStatus("PROCESSING");
    payment.setAmount(amount);
    payment.setCreatedAt(trantor::Date::now());
    payment.setUpdatedAt(trantor::Date::now());
    paymentMapper.insert(payment);

    Json::Value wechatConfig;
    wechatConfig["api_v3_key"] = "0123456789abcdef0123456789abcdef";
    auto wechatClient = std::make_shared<WechatPayClient>(wechatConfig);

    PayPlugin plugin;
    plugin.setTestClients(wechatClient, nullptr, client);

    std::promise<Json::Value> resultPromise;
    std::promise<std::error_code> errorPromise;

    auto paymentService = plugin.paymentService();
    paymentService->queryOrder(
      orderNo,
      [&resultPromise, &errorPromise](const Json::Value &result, const std::error_code &error) {
          resultPromise.set_value(result);
          errorPromise.set_value(error);
      }
    );

    auto resultFuture = resultPromise.get_future();
    auto errorFuture = errorPromise.get_future();

    CHECK(resultFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(errorFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    const auto error = errorFuture.get();
    CHECK(!error);

    const auto result = resultFuture.get();
    CHECK(result.isMember("data"));
    CHECK(result["data"]["order_no"].asString() == orderNo);
    CHECK(result["data"]["status"].asString() == "PAYING");
    CHECK(result["data"]["wechat_query_error"].asString().find("missing") != std::string::npos);

    // Order/payment status unchanged since WeChat query failed
    const auto updatedOrder = orderMapper.findByPrimaryKey(order.getValueOfId());
    CHECK(updatedOrder.getValueOfStatus() == "PAYING");

    const auto updatedPayment = paymentMapper.findByPrimaryKey(payment.getValueOfId());
    CHECK(updatedPayment.getValueOfStatus() == "PROCESSING");

    client->execSqlSync("DELETE FROM pay_payment WHERE payment_no = $1", paymentNo);
    client->execSqlSync("DELETE FROM pay_order WHERE order_no = $1", orderNo);
}
