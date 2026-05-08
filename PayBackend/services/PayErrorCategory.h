#pragma once

#include <system_error>
#include <string>
#include <unordered_map>
#include <mutex>

namespace pay {

class PayErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override {
        return "pay";
    }

    std::string message(int ev) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = messages_.find(ev);
        if (it != messages_.end()) {
            return it->second;
        }
        return "pay error " + std::to_string(ev);
    }

    static PayErrorCategory& instance() {
        static PayErrorCategory cat;
        return cat;
    }

    // ✅ 按值传递字符串，确保拷贝存储
    void setMessage(int code, std::string msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_[code] = std::move(msg);
    }

private:
    PayErrorCategory() = default;
    mutable std::mutex mutex_;
    std::unordered_map<int, std::string> messages_;
};

// ✅ 按值传递，确保字符串内容被拷贝存储
inline std::error_code makePayError(int code, std::string message) {
    PayErrorCategory::instance().setMessage(code, std::move(message));
    return std::error_code(code, PayErrorCategory::instance());
}

}  // namespace pay
