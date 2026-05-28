#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace pay::utils
{

template <typename Signature>
class OnceCallback;

template <typename R, typename... Args>
class OnceCallback<R(Args...)>
{
    static_assert(std::is_void_v<R>, "OnceCallback only supports void-returning callbacks");

  public:
    OnceCallback() = default;

    template <
      typename F,
      typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, OnceCallback>>>
    explicit OnceCallback(F &&callback)
        : state_(std::make_shared<State>(std::function<R(Args...)>(std::forward<F>(callback))))
    {
    }

    bool valid() const
    {
        if (!state_)
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        return static_cast<bool>(state_->callback);
    }

    explicit operator bool() const
    {
        return valid();
    }

    template <typename... CallArgs>
    bool call(CallArgs &&...args) const
    {
        if (!state_)
        {
            return false;
        }

        std::function<R(Args...)> callback;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->callback)
            {
                return false;
            }
            callback = std::move(state_->callback);
            state_->callback = nullptr;
        }

        callback(std::forward<CallArgs>(args)...);
        return true;
    }

  private:
    struct State
    {
        explicit State(std::function<R(Args...)> cb) : callback(std::move(cb))
        {
        }

        mutable std::mutex mutex;
        std::function<R(Args...)> callback;
    };

    std::shared_ptr<State> state_;
};

template <typename Signature, typename F>
OnceCallback<Signature> makeOnceCallback(F &&callback)
{
    return OnceCallback<Signature>(std::forward<F>(callback));
}

}  // namespace pay::utils
