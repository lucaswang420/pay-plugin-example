#pragma once

#include <string>
#include <vector>

struct ValidationResult
{
    bool ok = false;
    std::vector<std::string> missingVars;
};

class StartupValidator
{
  public:
    static bool isPlaceholder(const std::string &value);
    static ValidationResult validateRequired(
      const std::vector<std::string> &requiredVars
    );
    static void validate(const std::vector<std::string> &requiredVars);
};
