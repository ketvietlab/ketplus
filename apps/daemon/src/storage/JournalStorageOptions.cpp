#include "storage/JournalStorageOptions.h"
namespace ketplus::daemon {
namespace {
QString imagesName(JournalStorageOptions::GeneratedImages images) {
    switch (images) {
    case JournalStorageOptions::GeneratedImages::Full:
        return QStringLiteral("full");
    case JournalStorageOptions::GeneratedImages::Caption:
        return QStringLiteral("caption");
    case JournalStorageOptions::GeneratedImages::Preview:
        break;
    }
    return QStringLiteral("preview");
}
} // namespace
QJsonObject JournalStorageOptions::toJson() const {
    return {{"archived_history_days", archivedHistoryDays},
            {"generated_images", imagesName(generatedImages)},
            {"warn_near_limit", warnNearLimit}};
}
std::optional<JournalStorageOptions> JournalStorageOptions::fromJson(const QJsonObject& value) {
    if (value.size() != 3 || !value.value("archived_history_days").isDouble() ||
        !value.value("generated_images").isString() || !value.value("warn_near_limit").isBool())
        return std::nullopt;
    const double days = value.value("archived_history_days").toDouble(-1);
    if (days != 0 && days != 30 && days != 90 && days != 365)
        return std::nullopt;
    JournalStorageOptions options;
    options.archivedHistoryDays = int(days);
    options.warnNearLimit = value.value("warn_near_limit").toBool();
    const auto images = value.value("generated_images").toString();
    if (images == QStringLiteral("full"))
        options.generatedImages = GeneratedImages::Full;
    else if (images == QStringLiteral("caption"))
        options.generatedImages = GeneratedImages::Caption;
    else if (images != QStringLiteral("preview"))
        return std::nullopt;
    return options;
}
} // namespace ketplus::daemon
