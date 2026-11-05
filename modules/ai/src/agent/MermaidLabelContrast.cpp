#include "agent/MermaidLabelContrast.h"

#include <QColor>
#include <QRegularExpression>
#include <QStringList>

#include <cmath>

namespace ketplus::agent_message {

namespace {

constexpr auto darkLabel = "#1f2328";
constexpr auto lightLabel = "#f0f3f6";

QString readableLine(const QString& line) {
    static const QRegularExpression styling(
        QStringLiteral(R"(^\s*(?:style|classDef)\s+\S+\s+(.*)$)"));
    static const QRegularExpression fill(QStringLiteral(R"((?:^|[,;\s])fill\s*:\s*([^,;\s]+))"));
    static const QRegularExpression colour(QStringLiteral(R"((?:^|[,;\s])color\s*:)"));
    const auto declaration = styling.match(line);
    if (!declaration.hasMatch())
        return line;
    const QString properties = declaration.captured(1);
    const auto fillMatch = fill.match(properties);
    if (!fillMatch.hasMatch() || colour.match(properties).hasMatch())
        return line;
    const QColor background(fillMatch.captured(1));
    if (!background.isValid() || background.alpha() == 0)
        return line;
    // Relative luminance picks whichever text keeps the label legible on that fill.
    const auto channel = [](const qreal value) {
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const qreal luminance = 0.2126 * channel(background.redF()) +
                            0.7152 * channel(background.greenF()) +
                            0.0722 * channel(background.blueF());
    QString trimmed = line;
    while (trimmed.endsWith(QLatin1Char(';')) || trimmed.endsWith(QLatin1Char(' ')))
        trimmed.chop(1);
    return trimmed + QStringLiteral(",color:") +
           QLatin1StringView(luminance > 0.18 ? darkLabel : lightLabel);
}

} // namespace

QString withReadableMermaidLabels(const QString& diagram) {
    QStringList lines = diagram.split(QLatin1Char('\n'));
    for (QString& line : lines)
        line = readableLine(line);
    return lines.join(QLatin1Char('\n'));
}

} // namespace ketplus::agent_message
