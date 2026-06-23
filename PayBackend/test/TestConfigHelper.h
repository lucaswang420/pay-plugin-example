#pragma once

// ============================================================================
// Shared test helpers for loading config.json and building a libpq connection
// string. Replaces the per-file duplicates that lived in each integration test.
//
// loadConfig() resolves __env_var:VAR__ placeholders via ConfigLoader, mirroring
// main.cc / test_main.cc, so credentials in config.json (e.g. the DB password)
// are pulled from the environment that test_main loaded from .env. Without this,
// the raw placeholder string was passed to libpq and DB auth failed.
// ============================================================================

#include <json/json.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "../utils/ConfigLoader.h"

namespace pay::test_util
{

inline bool loadConfig(Json::Value &root)
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
    if (!ok)
    {
        return false;
    }

    // Resolve __env_var:VAR__ placeholders so credentials come from the
    // environment (loaded from .env by test_main), matching main.cc.
    root = ConfigLoader::loadConfig(root);
    return true;
}

inline std::string buildPgConnInfo(const Json::Value &db)
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

}  // namespace pay::test_util
