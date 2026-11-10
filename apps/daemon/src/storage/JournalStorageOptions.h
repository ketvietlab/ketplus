#pragma once
#include <QJsonObject>
#include <QString>
#include <optional>
namespace ketplus::daemon {
// The Storage page's remaining choices (ADR 0108). They live apart from the detail and
// size policies, so a client that saves one of those never resets them.
struct JournalStorageOptions final {
    enum class GeneratedImages { Full, Preview, Caption };
    int archivedHistoryDays{90}; // zero keeps archived conversations until deleted
    GeneratedImages generatedImages{GeneratedImages::Preview};
    bool warnNearLimit{true};
    qint64 revision{0};
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static std::optional<JournalStorageOptions> fromJson(const QJsonObject& value);
};
} // namespace ketplus::daemon
