#include "agent/ReasoningHeading.h"

#include <QJsonArray>
#include <QRegularExpression>

namespace ketplus::agent_message {

QString reasoningHeading(const QJsonObject& item) {
    static const QRegularExpression bold(QStringLiteral(R"(^\s*\*\*([^*\n]{1,120})\*\*)"));
    const QJsonArray summary = item.value(QStringLiteral("summary")).toArray();
    // The latest summary part names what the provider is doing now.
    for (qsizetype index = summary.size() - 1; index >= 0; --index) {
        const QJsonValue part = summary.at(index);
        const QString text = part.isObject()
                                 ? part.toObject().value(QStringLiteral("text")).toString()
                                 : part.toString();
        const auto match = bold.match(text);
        if (!match.hasMatch())
            continue;
        QString heading = match.captured(1).trimmed();
        if (heading.isEmpty())
            continue;
        // "Profiling" reads as "profiling"; an acronym such as "API" keeps its case.
        if (heading.size() == 1 || heading.at(1).isLower())
            heading[0] = heading.at(0).toLower();
        return heading;
    }
    return {};
}

QString workingLine(const QString& status, const QString& detail) {
    if (detail.isEmpty() ||
        (status != QStringLiteral("Working…") && status != QStringLiteral("Reasoning…")))
        return status;
    return QStringLiteral("Working · %1").arg(detail);
}

} // namespace ketplus::agent_message
