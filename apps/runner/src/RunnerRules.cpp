#include "RunnerRules.h"

#include <QRegularExpression>
#include <QSet>

namespace ketplus::runner::app {

bool isRelayId(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    return pattern.match(text).hasMatch();
}

std::optional<QString> normalizedName(const QString& name) {
    const QString trimmed = name.trimmed();
    for (const QChar c : trimmed) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return std::nullopt;
    }
    for (qsizetype i = 0; i < trimmed.size(); ++i) {
        // An unpaired surrogate has no code point to count or send.
        if (trimmed.at(i).isHighSurrogate() && i + 1 < trimmed.size() &&
            trimmed.at(i + 1).isLowSurrogate()) {
            ++i;
            continue;
        }
        if (trimmed.at(i).isSurrogate())
            return std::nullopt;
    }
    const qsizetype codePoints = trimmed.toUcs4().size();
    if (codePoints < 1 || codePoints > maxNameCharacters)
        return std::nullopt;
    return trimmed;
}

QString defaultName(const QString& hostName) {
    const QList<uint> codePoints = hostName.trimmed().toUcs4();
    return QString::fromUcs4(reinterpret_cast<const char32_t*>(codePoints.constData()),
                             std::min<qsizetype>(codePoints.size(), maxNameCharacters));
}

QString labelsError(const QStringList& labels) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9._-]*$"));
    if (labels.size() > maxLabels)
        return QStringLiteral("At most %1 labels are allowed").arg(maxLabels);
    QSet<QString> seen;
    for (const QString& label : labels) {
        if (label.size() > maxLabelCharacters || !pattern.match(label).hasMatch())
            return QStringLiteral("Label \"%1\" must be 1 to 64 characters of a-z, 0-9, '.', "
                                  "'_' and '-', starting with a letter or digit")
                .arg(label.left(maxLabelCharacters));
        if (seen.contains(label))
            return QStringLiteral("Label \"%1\" is given twice").arg(label);
        seen.insert(label);
    }
    return {};
}

} // namespace ketplus::runner::app
