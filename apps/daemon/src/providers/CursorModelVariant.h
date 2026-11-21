#pragma once
#include <QStringList>
namespace ketplus::daemon::cursor_model {
struct CursorVariant final {
    QString id;
    QString label;
    QString family;
    QString effort;
    bool fast{false};
};

inline const QStringList& cursorEffortSuffixes() {
    static const QStringList suffixes{QStringLiteral("extra-high"), QStringLiteral("minimal"),
                                      QStringLiteral("medium"),     QStringLiteral("xhigh"),
                                      QStringLiteral("none"),       QStringLiteral("high"),
                                      QStringLiteral("low"),        QStringLiteral("max")};
    return suffixes;
}

inline CursorVariant parseCursorVariant(const QString& id, const QString& label) {
    CursorVariant variant;
    variant.id = id;
    variant.label = label;
    QString rest = id;
    if (rest.endsWith(QStringLiteral("-fast"))) {
        variant.fast = true;
        rest.chop(5);
    }
    for (const QString& suffix : cursorEffortSuffixes()) {
        const QString token = QLatin1Char('-') + suffix;
        if (rest.size() > token.size() && rest.endsWith(token)) {
            rest.chop(token.size());
            variant.family = rest;
            variant.effort = suffix;
            return variant;
        }
    }
    variant.family = rest;
    variant.effort = variant.fast ? QStringLiteral("fast") : QStringLiteral("medium");
    return variant;
}

} // namespace ketplus::daemon::cursor_model
