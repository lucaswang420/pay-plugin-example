#include "StartupValidator.h"
#include "ConfigLoader.h"
#include <drogon/drogon.h>
#include <cstdlib>

bool StartupValidator::isPlaceholder(const std::string &value)
{
    if (value.empty())
    {
        return true;
    }
    if (value.find("__env_var") == 0)
    {
        return true;
    }
    if (value.size() >= 3 && value.front() == '$' && value[1] == '{' &&
        value.back() == '}')
    {
        return true;
    }
    return false;
}

ValidationResult StartupValidator::validateRequired(
  const std::vector<std::string> &requiredVars
)
{
    ValidationResult result;
    result.ok = true;

    for (const auto &varName : requiredVars)
    {
        const char *envValue = std::getenv(varName.c_str());
        if (!envValue || isPlaceholder(std::string(envValue)))
        {
            result.ok = false;
            result.missingVars.push_back(varName);
        }
    }

    return result;
}

void StartupValidator::validate(
  const std::vector<std::string> &requiredVars
)
{
    auto result = validateRequired(requiredVars);

    if (!result.ok)
    {
        for (const auto &varName : result.missingVars)
        {
            LOG_ERROR << "Missing or invalid required environment variable: "
                      << varName;
        }
        LOG_ERROR << "Startup validation failed. Exiting.";
        exit(1);
    }

    LOG_INFO << "Startup validation passed. Loaded sensitive config:";
    for (const auto &varName : requiredVars)
    {
        const char *envValue = std::getenv(varName.c_str());
        if (envValue)
        {
            LOG_INFO << "  " << varName << " = "
                     << ConfigLoader::maskSensitive(std::string(envValue));
        }
    }
}
