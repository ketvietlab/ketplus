#include "app/WaitingBadge.h"

#include <QGuiApplication>
#include <QVariantMap>

#include <algorithm>

namespace ketplus {

WaitingBadge& WaitingBadge::instance() {
    static WaitingBadge badge;
    return badge;
}

int WaitingBadge::countWaiting(const QVariantList& conversations) {
    return static_cast<int>(
        std::count_if(conversations.cbegin(), conversations.cend(), [](const QVariant& value) {
            return value.toMap().value(QStringLiteral("awaitingReply")).toBool();
        }));
}

void WaitingBadge::report(QObject* owner, const QString& worktreePath, int waiting) {
    auto& owners = waiting_[worktreePath];
    if (waiting > 0)
        owners.insert(owner, waiting);
    else
        owners.remove(owner);
    if (owners.isEmpty())
        waiting_.remove(worktreePath);
    apply();
}

void WaitingBadge::forget(QObject* owner) {
    for (auto it = waiting_.begin(); it != waiting_.end();) {
        it->remove(owner);
        it = it->isEmpty() ? waiting_.erase(it) : std::next(it);
    }
    apply();
}

int WaitingBadge::total() const {
    int sum = 0;
    for (const auto& owners : waiting_)
        sum += *std::max_element(owners.cbegin(), owners.cend());
    return sum;
}

void WaitingBadge::apply() {
    const int count = total();
    if (count == applied_)
        return;
    applied_ = count;
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        app->setBadgeNumber(count);
}

} // namespace ketplus
