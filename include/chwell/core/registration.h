#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace chwell { namespace core {

// Move-only cancellation ownership. Reset/destruction cancels exactly once.
class Registration {
public:
    Registration() = default;
    explicit Registration(std::function<void()> cancel) : cancel_(std::move(cancel)) {}
    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;
    Registration(Registration&& other) noexcept : cancel_(std::move(other.cancel_)) {
        other.cancel_ = {};
    }
    Registration& operator=(Registration&& other) noexcept {
        if (this != &other) {
            reset();
            cancel_ = std::move(other.cancel_);
            other.cancel_ = {};
        }
        return *this;
    }
    ~Registration() { reset(); }
    explicit operator bool() const noexcept { return static_cast<bool>(cancel_); }
    void reset() noexcept {
        auto cancel = std::move(cancel_);
        cancel_ = {};
        if (cancel) {
            // Cleanup must continue even when a user-supplied cancellation throws.
            try { cancel(); } catch (...) {}
        }
    }
private:
    std::function<void()> cancel_;
};

// Owner-thread collection. Registrations are released in reverse acquisition order.
class RegistrationGroup {
public:
    ~RegistrationGroup() { clear(); }
    bool add(Registration registration) {
        if (!registration || clearing_) return false;
        registrations_.push_back(std::move(registration));
        return true;
    }
    void clear() noexcept {
        if (clearing_) return;
        clearing_ = true;
        auto registrations = std::move(registrations_);
        registrations_.clear();
        for (auto it = registrations.rbegin(); it != registrations.rend(); ++it) it->reset();
        clearing_ = false;
    }
private:
    std::vector<Registration> registrations_;
    bool clearing_ = false;
};

namespace detail {

// Snapshots retain only a weak reference to this slot, not the user's callback.
// Cancellation waits for an invocation on another thread; recursive publication
// and self-cancellation are supported. A self-cancelled invocation may finish.
template <typename... Args>
class CallbackSlot {
public:
    explicit CallbackSlot(std::function<void(Args...)> callback)
        : callback_(std::make_shared<std::function<void(Args...)>>(std::move(callback))) {}
    void invoke(Args... args) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!callback_ || !*callback_) return;
        auto callback = callback_; // Keep self-cancellation from destroying an executing function.
        (*callback)(std::forward<Args>(args)...);
    }
    void cancel() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        callback_.reset();
    }
    std::function<void(Args...)> wrapper(const std::shared_ptr<CallbackSlot>& self) {
        std::weak_ptr<CallbackSlot> weak = self;
        return [weak](Args... args) {
            if (auto slot = weak.lock()) slot->invoke(std::forward<Args>(args)...);
        };
    }
private:
    std::recursive_mutex mutex_;
    std::shared_ptr<std::function<void(Args...)>> callback_;
};

// Allows a registration to outlive its source without dereferencing that source.
// Sources detach before destroying their registration tables.
template <typename T>
struct RegistrationSource {
    explicit RegistrationSource(T* value) : source(value) {}
    void detach() {
        std::lock_guard<std::mutex> lock(mutex);
        source = nullptr;
    }
    std::mutex mutex;
    T* source;
};

} // namespace detail
}} // namespace chwell::core
