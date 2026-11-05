#pragma once

#include <QRegularExpression>
#include <QString>

namespace ketplus::conversation_title {

// First-message titles are a UI summary, never a copy of a pasted handoff.
// Prefer the request explicitly identified inside a handoff when present.
inline QString suggest(const QString& text, const qsizetype maximum = 56) {
    static const QRegularExpression requestLabel(
        QStringLiteral(R"((?:^|\b)(?:latest\s+)?user request\s*:\s*)"),
        QRegularExpression::CaseInsensitiveOption);
    QString candidate;
    auto matches = requestLabel.globalMatch(text);
    while (matches.hasNext())
        candidate = text.mid(matches.next().capturedEnd()).section(u'\n', 0, 0);
    if (candidate.isEmpty()) {
        for (const QString& line : text.split(u'\n')) {
            const QString trimmed = line.trimmed();
            if (!trimmed.isEmpty() && !trimmed.startsWith(u'<')) {
                candidate = trimmed;
                break;
            }
        }
    }
    candidate = candidate.section(u'<', 0, 0);
    candidate.remove(QRegularExpression(QStringLiteral(R"(^[\s#*>-]+)")));
    candidate = candidate.simplified();
    if (candidate.isEmpty())
        return QStringLiteral("New conversation");
    if (maximum > 1 && candidate.size() > maximum)
        candidate = candidate.left(maximum - 1).trimmed() + QString::fromUtf8("…");
    return candidate;
}

} // namespace ketplus::conversation_title
