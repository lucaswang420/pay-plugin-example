#pragma once

#include <json/json.h>
#include <string>
#include <unordered_map>

/**
 * @brief Configuration loader with environment variable support
 *
 * Loads configuration from JSON file and replaces placeholder values
 * with environment variables. This allows sensitive data to be stored
 * in .env files (not committed to git) while config.json can be
 * safely committed.
 *
 * Placeholder format in config.json:
 * - "__env_var__" - Load from environment variable
 * - "__env_var:VAR_NAME__" - Load from specific environment variable
 * - "DEFAULT_VALUE" - Use as-is if no __env_var__ prefix
 */
class ConfigLoader
{
  public:
    /**
     * @brief Load and replace environment variable placeholders
     *
     * @param config Original config JSON
     * @return Json::Value Config with placeholders replaced
     */
    static Json::Value loadConfig(const Json::Value &config);

    /**
     * @brief Load .env file and set environment variables
     *
     * @param envPath Path to .env file (default: "./.env")
     * @return true if loaded successfully
     */
    static bool loadEnvFile(const std::string &envPath = "./.env");

    /**
     * @brief Mask sensitive value for logging (first 4 + *** + last 4)
     */
    static std::string maskSensitive(const std::string &value);

  private:
    /**
     * @brief Replace placeholders in a JSON value recursively
     */
    static Json::Value replacePlaceholders(const Json::Value &value);

    /**
     * @brief Get environment variable value
     *
     * @param placeholder Placeholder string (e.g., "__env_var:MY_VAR__")
     * @return std::string Environment variable value or empty string
     */
    static std::string getEnvValue(const std::string &placeholder);

    /**
     * @brief Parse environment variable name from placeholder
     *
     * Examples:
     * - "__env_var__" -> Use key name as env var
     * - "__env_var:MY_VAR__" -> Use "MY_VAR" as env var
     */
    static std::string parseEnvVarName(const std::string &placeholder, const std::string &key);
};
