#include "agent/MermaidLabelContrast.h"

#include "KetTokens.h"

#include <QColor>
#include <QRegularExpression>
#include <QStringList>

#include <cmath>
#include <initializer_list>
#include <utility>

namespace ketplus::agent_message {

namespace {

qreal luminance(const QColor& colour) {
    const auto channel = [](const qreal value) {
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(colour.redF()) + 0.7152 * channel(colour.greenF()) +
           0.0722 * channel(colour.blueF());
}

qreal contrast(const QColor& first, const QColor& second) {
    const qreal a = luminance(first);
    const qreal b = luminance(second);
    return (qMax(a, b) + 0.05) / (qMin(a, b) + 0.05);
}

// The label with the most contrast on the fill, from Két DS tokens: the main text of the
// light and dark themes, and black or white when neither reaches 4.5:1 (mid-grey fills).
// Black or white always reach it.
QColor labelOn(const QColor& fill) {
    const auto best = [&fill](std::initializer_list<QColor> inks) {
        QColor chosen;
        qreal highest = 0;
        for (const QColor& ink : inks) {
            if (const qreal ratio = contrast(fill, ink); ratio > highest) {
                chosen = ink;
                highest = ratio;
            }
        }
        return std::pair{chosen, highest};
    };
    const auto [text, ratio] = best({ket::Tokens::color(u"text-main", ket::Theme::Light),
                                     ket::Tokens::color(u"text-main", ket::Theme::Dark)});
    if (ratio >= minimumLabelContrast)
        return text;
    return best({ket::Tokens::color(u"ref-black", ket::Theme::Light),
                 ket::Tokens::color(u"ref-white", ket::Theme::Light)})
        .first;
}

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
    QString trimmed = line;
    while (trimmed.endsWith(QLatin1Char(';')) || trimmed.endsWith(QLatin1Char(' ')))
        trimmed.chop(1);
    return trimmed + QStringLiteral(",color:") + labelOn(background).name();
}

} // namespace

QString withReadableMermaidLabels(const QString& diagram) {
    QStringList lines = diagram.split(QLatin1Char('\n'));
    for (QString& line : lines)
        line = readableLine(line);
    return lines.join(QLatin1Char('\n'));
}

} // namespace ketplus::agent_message
