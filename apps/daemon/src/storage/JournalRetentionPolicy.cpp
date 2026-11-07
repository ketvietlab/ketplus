#include "storage/JournalRetentionPolicy.h"
namespace ketplus::daemon {
QJsonObject JournalRetentionPolicy::toJson() const {
    return {{"full_detail_days", fullDetailDays},
            {"automatic_detail_retention", automaticDetailRetention}};
}
std::optional<JournalRetentionPolicy> JournalRetentionPolicy::fromJson(const QJsonObject& value) {
    if (value.size() != 2 || !value.value("full_detail_days").isDouble() ||
        !value.value("automatic_detail_retention").isBool())
        return std::nullopt;
    const auto days = value.value("full_detail_days").toDouble(-1);
    if (days != 0 && days != 7 && days != 30 && days != 90)
        return std::nullopt;
    return JournalRetentionPolicy{int(days), value.value("automatic_detail_retention").toBool(), 0};
}
std::optional<QDateTime> JournalRetentionPolicy::cutoff(const QDateTime& now) const {
    if (!now.isValid() || !automaticDetailRetention || fullDetailDays == 0)
        return std::nullopt;
    return now.toUTC().addDays(-fullDetailDays);
}
} // namespace ketplus::daemon
