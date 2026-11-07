#include "storage/JournalSizePolicy.h"
namespace ketplus::daemon {
QJsonObject JournalSizePolicy::toJson() const {
    return {{"limit_bytes", limitBytes}, {"warning_percent", warningPercent}};
}
std::optional<JournalSizePolicy> JournalSizePolicy::fromJson(const QJsonObject& value) {
    if (value.size() != 2 || !value.value("limit_bytes").isDouble() ||
        !value.value("warning_percent").isDouble())
        return std::nullopt;
    const double limit = value.value("limit_bytes").toDouble(-1);
    const double warning = value.value("warning_percent").toDouble(-1);
    if ((limit != 0 && limit != 1'000'000'000LL && limit != 2'000'000'000LL &&
         limit != 5'000'000'000LL && limit != 10'000'000'000LL) ||
        warning < 1 || warning > 100 || warning != int(warning))
        return std::nullopt;
    return JournalSizePolicy{qint64(limit), int(warning), 0};
}
} // namespace ketplus::daemon
