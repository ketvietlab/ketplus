#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <optional>
namespace ketplus::daemon {
struct JournalRetentionPolicy final {
    int fullDetailDays{30}; // zero means no age-based detail trimming
    bool automaticDetailRetention{true};
    qint64 revision{0};
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static std::optional<JournalRetentionPolicy> fromJson(const QJsonObject& value);
    [[nodiscard]] std::optional<QDateTime> cutoff(const QDateTime& now) const;
};
} // namespace ketplus::daemon
