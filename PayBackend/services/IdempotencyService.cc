#include "IdempotencyService.h"
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
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<CheckCallback>(std::move(callback));

    // Step 1: Check Redis first (fast path) - only if Redis client is available
    if (redisClient_)
    {
        std::string redisKey = "idempotency:" + idempotencyKey;
        redisClient_->execCommandAsync(
          [this, idempotencyKey, requestHash, request, sharedCb](const nosql::RedisResult &result) {
              if (!result.isNil())
              {
                  // Cache hit - return cached result
                  try
                  {
                      std::string redisStr = result.asString();
                      LOG_INFO << "[IdempotencyService] Redis cache hit: key=" << idempotencyKey
                               << ", str length=" << redisStr.length();
                      LOG_DEBUG << "[IdempotencyService] Redis content: " << redisStr;

                      Json::Value cached = Json::Value();
                      Json::CharReaderBuilder builder;
                      std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
                      std::string errors;
                      const char *str = redisStr.c_str();
                      bool parseSuccess =
                        reader->parse(str, str + redisStr.length(), &cached, &errors);

                      LOG_INFO << "[IdempotencyService] Redis parse: success=" << parseSuccess
                               << ", has_request_hash=" << cached.isMember("request_hash")
                               << ", has_response=" << cached.isMember("response");
                      if (cached.isMember("response"))
                      {
                          LOG_INFO << "[IdempotencyService] Redis response field: has_data="
                                   << cached["response"].isMember("data")
                                   << ", members=" << cached["response"].getMemberNames().size();
                      }

                      if (*sharedCb)
                      {
                          if (cached["request_hash"].asString() == requestHash)
                          {
                              // Same request - return cached response
                              LOG_INFO << "[IdempotencyService] Returning cached response for key="
                                       << idempotencyKey;
                              (*sharedCb)(true, cached["response"]);
                          }
                          else
                          {
                              // Different request - idempotency conflict
                              LOG_WARN << "[IdempotencyService] Idempotency hash mismatch";
                              (*sharedCb)(false, Json::Value());
                          }
                      }
                  }
                  catch (const std::exception &e)
                  {
                      LOG_ERROR << "[IdempotencyService] Redis cache parse error: " << e.what();
                      if (*sharedCb)
                      {
                          (*sharedCb)(false, Json::Value());
                      }
                  }
                  return;
              }

              // Cache miss - check database
              checkDatabase(idempotencyKey, requestHash, request, std::move(*sharedCb));
          },
          [this, idempotencyKey, requestHash, request, sharedCb](const std::exception &e) {
              // Redis error - fall back to database
              LOG_WARN << "Redis idempotency check failed: " << e.what();
              checkDatabase(idempotencyKey, requestHash, request, std::move(*sharedCb));
          },
          "GET %s",
          redisKey.c_str()
        );
    }
    else
    {
        // Redis not available - check database directly
        checkDatabase(idempotencyKey, requestHash, request, std::move(*sharedCb));
    }
}

void IdempotencyService::checkDatabase(
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const Json::Value &request,
  CheckCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<CheckCallback>(std::move(callback));

    // Step 2: Check database
    dbClient_->execSqlAsync(
      "SELECT request_hash, response_snapshot FROM pay_idempotency WHERE idempotency_key = $1",
      [this, idempotencyKey, requestHash, request, sharedCb](const orm::Result &rows) {
          if (!rows.empty())
          {
              std::string cachedHash = rows[0]["request_hash"].c_str();

              if (cachedHash == requestHash)
              {
                  // Same request - backfill Redis and return
                  Json::Value snapshot;
                  Json::CharReaderBuilder builder;
                  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
                  std::string errors;
                  const char *str = rows[0]["response_snapshot"].c_str();
                  size_t strLen = strlen(str);
                  LOG_INFO << "[IdempotencyService] Loading from DB: key=" << idempotencyKey
                           << ", snapshot length=" << strLen;
                  LOG_DEBUG << "[IdempotencyService] Snapshot content: " << str;
                  bool parseSuccess = reader->parse(str, str + strLen, &snapshot, &errors);
                  Json::Value response = snapshot["response"];
                  LOG_INFO << "[IdempotencyService] Parsed from DB: success=" << parseSuccess
                           << ", has_response=" << snapshot.isMember("response")
                           << ", has_data=" << response.isMember("data")
                           << ", isNull=" << response.isNull()
                           << ", members=" << response.getMemberNames().size()
                           << ", errors=" << errors;

                  // Update Redis cache if available
                  if (redisClient_)
                  {
                      Json::Value cached;
                      cached["request_hash"] = requestHash;
                      cached["response"] = response;

                      std::string redisKey = "idempotency:" + idempotencyKey;
                      std::string cacheStr = pay::utils::toJsonString(cached);

                      redisClient_->execCommandAsync(
                        [sharedCb, response](const nosql::RedisResult &) {
                            if (*sharedCb)
                            {
                                (*sharedCb)(true, response);
                            }
                        },
                        [sharedCb, response](const nosql::RedisException &) {
                            // Ignore Redis errors - DB is source of truth
                            if (*sharedCb)
                            {
                                (*sharedCb)(true, response);
                            }
                        },
                        "SETEX %s %d %s",
                        redisKey.c_str(),
                        ttlSeconds_,
                        cacheStr.c_str()
                      );
                  }
                  else
                  {
                      LOG_INFO << "[IdempotencyService] Idempotency hit: key=" << idempotencyKey
                               << ", returning cached response, has_data="
                               << response.isMember("data");
                      if (*sharedCb)
                      {
                          (*sharedCb)(true, response);
                      }
                  }
              }
              else
              {
                  // Different request - conflict
                  LOG_INFO << "[IdempotencyService] Idempotency conflict: key=" << idempotencyKey
                           << ", cached_hash=" << cachedHash.substr(0, 8) << "..."
                           << ", request_hash=" << requestHash.substr(0, 8) << "...";
                  if (*sharedCb)
                  {
                      (*sharedCb)(false, Json::Value());
                  }
              }
              return;
          }

          // Step 3: First request - return success without inserting
          // The service will call updateResult later to save the actual response
          LOG_INFO << "[IdempotencyService] First request, allowing to proceed: key="
                   << idempotencyKey;
          if (*sharedCb)
          {
              (*sharedCb)(true, Json::Value());
          }
      },
      [sharedCb](const orm::DrogonDbException &e) {
          // On database error, fail the idempotency check
          LOG_ERROR << "Idempotency DB check error: " << e.base().what();
          if (*sharedCb)
          {
              (*sharedCb)(false, Json::Value());
          }
      },
      idempotencyKey
    );
}

void IdempotencyService::updateResult(
  const std::string &idempotencyKey,
  const std::string &requestHash,
  const Json::Value &response,
  UpdateCallback &&callback
)
{
    // Wrap callback in shared_ptr to prevent it from being destroyed during async operations
    auto sharedCb = std::make_shared<UpdateCallback>(std::move(callback));

    // Build cache structure for Redis
    Json::Value cached;
    cached["request_hash"] = requestHash;
    cached["response"] = response;
    std::string cacheStr = pay::utils::toJsonString(cached);

    LOG_INFO << "[IdempotencyService] Saving to DB: key=" << idempotencyKey
             << ", hash=" << requestHash.substr(0, 8)
             << "..., has_response_data=" << response.isMember("data");

    // Use INSERT ... ON CONFLICT UPDATE to handle both insert and update cases
    dbClient_->execSqlAsync(
      "INSERT INTO pay_idempotency (idempotency_key, request_hash, response_snapshot) VALUES ($1, "
      "$2, $3) "
      "ON CONFLICT (idempotency_key) DO UPDATE SET "
      "response_snapshot = EXCLUDED.response_snapshot, "
      "request_hash = EXCLUDED.request_hash",
      [this, idempotencyKey, requestHash, response, cacheStr, sharedCb](const orm::Result &result) {
          // Update Redis cache if available
          if (redisClient_)
          {
              std::string redisKey = "idempotency:" + idempotencyKey;
              redisClient_->execCommandAsync(
                [sharedCb](const nosql::RedisResult &) {
                    if (*sharedCb)
                    {
                        (*sharedCb)();
                    }
                },
                [sharedCb](const nosql::RedisException &) {
                    // Ignore Redis errors - DB is source of truth
                    if (*sharedCb)
                    {
                        (*sharedCb)();
                    }
                },
                "SETEX %s %d %s",
                redisKey.c_str(),
                ttlSeconds_,
                cacheStr.c_str()
              );
          }
          else
          {
              if (*sharedCb)
              {
                  (*sharedCb)();
              }
          }
      },
      [sharedCb](const orm::DrogonDbException &e) {
          LOG_ERROR << "Idempotency DB upsert error: " << e.base().what();
          if (*sharedCb)
          {
              (*sharedCb)();
          }
      },
      idempotencyKey,
      requestHash,
      cacheStr
    );
}
