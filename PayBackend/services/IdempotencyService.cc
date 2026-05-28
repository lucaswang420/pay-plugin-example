#include "IdempotencyService.h"
#include "../utils/OnceCallback.h"
#include "../utils/PayUtils.h"
#include <drogon/drogon.h>
#include <openssl/sha.h>
#include <sstream>

using namespace drogon;

IdempotencyService::IdempotencyService(
  std::shared_ptr<orm::DbClient> dbClient,
  nosql::RedisClientPtr redisClient,
  int64_t ttlSeconds
)
    : dbClient_(dbClient), redisClient_(redisClient), ttlSeconds_(ttlSeconds)
{
}

void IdempotencyService::checkAndSet(
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const Json::Value &request,
  CheckCallback &&callback
)
{
    auto legacyCb = pay::utils::makeOnceCallback<void(bool, const Json::Value &)>(
      std::move(callback)
    );
    checkAndSetStatus(
      idempotencyKey,
      requestHash,
      request,
      [legacyCb](const CheckResult &result) mutable {
          switch (result.status)
          {
              case CheckStatus::Started:
                  legacyCb.call(true, Json::Value());
                  return;
              case CheckStatus::Replay:
                  legacyCb.call(true, result.cachedResult);
                  return;
              case CheckStatus::InProgress:
              case CheckStatus::Conflict:
              case CheckStatus::Error:
                  legacyCb.call(false, Json::Value());
                  return;
          }
      }
    );
}

void IdempotencyService::checkAndSetStatus(
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const Json::Value &request,
  StatusCallback &&callback
)
{
    auto onceCb =
      pay::utils::makeOnceCallback<void(const CheckResult &)>(std::move(callback));
    auto sharedCb =
      std::make_shared<pay::utils::OnceCallback<void(const CheckResult &)>>(onceCb);
    (void)request;

    // DB is the source of truth. Redis read-through is intentionally disabled
    // until it can share the same Started/Replay/InProgress/Conflict contract.
    checkDatabase(
      idempotencyKey,
      requestHash,
      [sharedCb](const CheckResult &result) { sharedCb->call(result); }
    );
}

void IdempotencyService::checkDatabase(
  const std::string &idempotencyKey,
  const std::string &requestHash,
  StatusCallback &&callback
)
{
    if (idempotencyKey.empty())
    {
        CheckResult result;
        result.status = CheckStatus::Started;
        callback(result);
        return;
    }

    auto onceCb =
      pay::utils::makeOnceCallback<void(const CheckResult &)>(std::move(callback));
    auto sharedCb =
      std::make_shared<pay::utils::OnceCallback<void(const CheckResult &)>>(onceCb);
    auto dbClient = dbClient_;
    const auto ttlSeconds = ttlSeconds_;
    const auto now = trantor::Date::now();
    const auto expiresAt =
      trantor::Date(now.microSecondsSinceEpoch() + ttlSeconds * static_cast<int64_t>(1000000));

    dbClient->execSqlAsync(
      "INSERT INTO pay_idempotency (idempotency_key, request_hash, response_snapshot, expire_at) "
      "VALUES ($1, $2, NULL, $3) ON CONFLICT (idempotency_key) DO NOTHING",
      [dbClient, idempotencyKey, requestHash, sharedCb](
        const orm::Result &insertResult
      ) {
          if (insertResult.affectedRows() > 0)
          {
              LOG_INFO << "[IdempotencyService] Reserved idempotency key=" << idempotencyKey;
              CheckResult checkResult;
              checkResult.status = CheckStatus::Started;
              sharedCb->call(checkResult);
              return;
          }

          dbClient->execSqlAsync(
            "SELECT request_hash, response_snapshot FROM pay_idempotency WHERE idempotency_key = $1",
            [idempotencyKey, requestHash, sharedCb](
              const orm::Result &rows
            ) {
          if (!rows.empty())
          {
              std::string cachedHash = rows[0]["request_hash"].c_str();

              if (cachedHash == requestHash)
              {
                  if (rows[0]["response_snapshot"].isNull())
                  {
                      LOG_INFO << "[IdempotencyService] Idempotency key in progress: key="
                               << idempotencyKey;
                      CheckResult checkResult;
                      checkResult.status = CheckStatus::InProgress;
                      checkResult.message = "idempotency request is already in progress";
                      sharedCb->call(checkResult);
                      return;
                  }

                  // Same request - backfill Redis and return
                  Json::Value snapshot;
                  Json::CharReaderBuilder builder;
                  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
                  std::string errors;
                  const char *str = rows[0]["response_snapshot"].c_str();
                  size_t strLen = strlen(str);
                  LOG_INFO << "[IdempotencyService] Loading from DB: key=" << idempotencyKey
                           << ", snapshot length=" << strLen;
                  bool parseSuccess = reader->parse(str, str + strLen, &snapshot, &errors);
                  Json::Value response = snapshot["response"];
                  LOG_INFO << "[IdempotencyService] Parsed from DB: success=" << parseSuccess
                           << ", has_response=" << snapshot.isMember("response")
                           << ", has_data=" << response.isMember("data")
                           << ", isNull=" << response.isNull()
                           << ", members=" << response.getMemberNames().size()
                           << ", errors=" << errors;

                  if (!parseSuccess || response.isNull())
                  {
                      CheckResult checkResult;
                      checkResult.status = CheckStatus::InProgress;
                      checkResult.message = "idempotency response is not ready";
                      sharedCb->call(checkResult);
                      return;
                  }

                  LOG_INFO << "[IdempotencyService] Idempotency hit: key=" << idempotencyKey
                           << ", returning cached response, has_data="
                           << response.isMember("data");
                  CheckResult checkResult;
                  checkResult.status = CheckStatus::Replay;
                  checkResult.cachedResult = response;
                  sharedCb->call(checkResult);
              }
              else
              {
                  // Different request - conflict
                  LOG_INFO << "[IdempotencyService] Idempotency conflict: key=" << idempotencyKey
                           << ", cached_hash=" << cachedHash.substr(0, 8) << "..."
                           << ", request_hash=" << requestHash.substr(0, 8) << "...";
                  CheckResult checkResult;
                  checkResult.status = CheckStatus::Conflict;
                  checkResult.message = "idempotency key conflict";
                  sharedCb->call(checkResult);
              }
              return;
          }

              CheckResult checkResult;
              checkResult.status = CheckStatus::Error;
              checkResult.message = "idempotency key disappeared after conflict";
              sharedCb->call(checkResult);
            },
            [sharedCb](const orm::DrogonDbException &e) {
                LOG_ERROR << "Idempotency DB select error: " << e.base().what();
                CheckResult checkResult;
                checkResult.status = CheckStatus::Error;
                checkResult.message = e.base().what();
                sharedCb->call(checkResult);
            },
            idempotencyKey
          );
      },
      [sharedCb](const orm::DrogonDbException &e) {
          // On database error, fail the idempotency check
          LOG_ERROR << "Idempotency DB reserve error: " << e.base().what();
          CheckResult checkResult;
          checkResult.status = CheckStatus::Error;
          checkResult.message = e.base().what();
          sharedCb->call(checkResult);
      },
      idempotencyKey,
      requestHash,
      expiresAt
    );
}

void IdempotencyService::updateResult(
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const Json::Value &response,
  UpdateCallback &&callback
)
{
    auto onceCb = pay::utils::makeOnceCallback<void(bool)>(std::move(callback));
    auto sharedCb = std::make_shared<pay::utils::OnceCallback<void(bool)>>(onceCb);
    auto dbClient = dbClient_;
    auto redisClient = redisClient_;
    const auto ttlSeconds = ttlSeconds_;

    // Build cache structure for Redis
    Json::Value cached;
    cached["request_hash"] = requestHash;
    cached["response"] = response;
    std::string cacheStr = pay::utils::toJsonString(cached);

    LOG_INFO << "[IdempotencyService] Saving to DB: key=" << idempotencyKey
             << ", hash=" << requestHash.substr(0, 8)
             << "..., has_response_data=" << response.isMember("data");

    // Use INSERT ... ON CONFLICT UPDATE to handle both insert and update cases
    dbClient->execSqlAsync(
      "UPDATE pay_idempotency SET response_snapshot = $3 "
      "WHERE idempotency_key = $1 AND request_hash = $2",
      [redisClient, ttlSeconds, idempotencyKey, requestHash, cacheStr, sharedCb](
        const orm::Result &result
      ) {
          if (result.affectedRows() == 0)
          {
              LOG_ERROR << "Idempotency update skipped: key/hash mismatch for key="
                        << idempotencyKey;
              sharedCb->call(false);
              return;
          }

          // Update Redis cache if available
          if (redisClient)
          {
              std::string redisKey = "idempotency:" + idempotencyKey;
              redisClient->execCommandAsync(
                [sharedCb](const nosql::RedisResult &) {
                    sharedCb->call(true);
                },
                [sharedCb](const nosql::RedisException &) {
                    // Ignore Redis errors - DB is source of truth
                    sharedCb->call(true);
                },
                "SETEX %s %d %s",
                redisKey.c_str(),
                ttlSeconds,
                cacheStr.c_str()
              );
          }
          else
          {
              sharedCb->call(true);
          }
      },
      [sharedCb](const orm::DrogonDbException &e) {
          LOG_ERROR << "Idempotency DB upsert error: " << e.base().what();
          sharedCb->call(false);
      },
      idempotencyKey,
      requestHash,
      cacheStr
    );
}
