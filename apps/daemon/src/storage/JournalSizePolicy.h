#pragma once
#include <QJsonObject>
#include <optional>
namespace ketplus::daemon {
struct JournalSizePolicy final {
    qint64 limitBytes{5'000'000'000LL}; // soft SQLite-page target; zero disables pressure
    int warningPercent{80};
    qint64 revision{0};
    // The default target applies without a save (ADR 0108 supersedes the opt-in of ADR 0046).
    [[nodiscard]] bool pressureConfigured() const { return limitBytes > 0; }
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static std::optional<JournalSizePolicy> fromJson(const QJsonObject& value);
};
} // namespace ketplus::daemon
