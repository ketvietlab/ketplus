#pragma once

#include <QJsonObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace ketplus {

// Lets the callbacks an owner hands to a longer-lived server go quiet once the owner is going
// away. A call keeps its place in the server (the server still counts it and its reply is still
// consumed); only the owner's own callback no longer runs. Wrap each callback with wrap().
class CallbackGuard final {
  public:
    using Callback = std::function<void(const QJsonObject&, const QString&)>;

    CallbackGuard() = default;
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;

    // Call first thing in the owner's destructor: its members and children may already be
    // gone when the server later answers or fails the call.
    void invalidate() noexcept { *alive_ = false; }

    template <class F> [[nodiscard]] Callback wrap(F&& callback) const {
        return [alive = std::weak_ptr<bool>(alive_),
                callback = Callback(std::forward<F>(callback))](const QJsonObject& result,
                                                                const QString& error) {
            const auto flag = alive.lock();
            if (flag && *flag) {
                callback(result, error);
                return;
            }
            ++dropped_;
        };
    }

    // Callbacks that arrived after their owner was invalidated and were dropped (for tests).
    [[nodiscard]] static int droppedCount() noexcept { return dropped_.load(); }

  private:
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
    inline static std::atomic<int> dropped_{0};
};

} // namespace ketplus
