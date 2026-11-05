#pragma once

#include "ui/InterfaceZoom.h"

#include <QRegularExpression>
#include <QString>
#include <QVariantMap>
#include <QtGlobal>

namespace ketplus {

// Text and glyph size steps designed at Medium (14px). Call sites use only these
// named steps; temporary px* aliases were collapsed onto the ladder in PR D.
enum class TypeStep {
    Micro,
    Caption,
    Body,
    Reading,
    Title,
    Heading,
    Display,
    GlyphSmall,
    GlyphLarge,
};

struct TypeScale final {
    [[nodiscard]] static int designed(const TypeStep step) {
        switch (step) {
        case TypeStep::Micro:
            return 10;
        case TypeStep::Caption:
            return 11;
        case TypeStep::Body:
            return 12;
        case TypeStep::Reading:
            return 14;
        case TypeStep::Title:
            return 16;
        case TypeStep::Heading:
            return 22;
        case TypeStep::Display:
            return 29;
        case TypeStep::GlyphSmall:
            return 16;
        case TypeStep::GlyphLarge:
            return 34;
        }
        return InterfaceZoomMetrics::mediumPixels;
    }

    [[nodiscard]] static int pixels(const TypeStep step, const int basePixels) {
        return InterfaceZoomMetrics::scale(designed(step), basePixels);
    }

    // Scales literal QSS geometry and expands {{micro}}, {{caption}}, … font steps.
    // `biasPixels` is added after zoom scaling (ActivityBar uses +2).
    [[nodiscard]] static QString apply(QString sheet, const int basePixels,
                                       const int biasPixels = 0) {
        // Only literal geometry is present before expanding font placeholders.
        static const QRegularExpression geometry(QStringLiteral(R"((?<![\w.])(\d+)px)"));
        auto matches = geometry.globalMatch(sheet);
        QList<QRegularExpressionMatch> replacements;
        while (matches.hasNext())
            replacements.append(matches.next());
        for (auto it = replacements.crbegin(); it != replacements.crend(); ++it)
            sheet.replace(it->capturedStart(1), it->capturedLength(1),
                          QString::number(
                              InterfaceZoomMetrics::geometry(it->captured(1).toInt(), basePixels)));
        const auto put = [&](const char* key, const TypeStep step) {
            sheet.replace(QLatin1String(key),
                          QString::number(pixels(step, basePixels) + biasPixels));
        };
        put("{{micro}}", TypeStep::Micro);
        put("{{caption}}", TypeStep::Caption);
        put("{{body}}", TypeStep::Body);
        put("{{reading}}", TypeStep::Reading);
        put("{{title}}", TypeStep::Title);
        put("{{heading}}", TypeStep::Heading);
        put("{{display}}", TypeStep::Display);
        put("{{glyphSmall}}", TypeStep::GlyphSmall);
        put("{{glyphLarge}}", TypeStep::GlyphLarge);
        return sheet;
    }

    // Rewrites every `font-size: <n>px` in a live stylesheet to the zoom-scaled
    // pixel value. At Medium this is a no-op (same numbers). Used to scale the
    // CM Theme.cpp sheet without editing vendor. Does not snap literals onto
    // the type ladder — pure zoom of whatever number is present.
    [[nodiscard]] static QString scaleLiteralFontSizes(QString sheet, const int basePixels) {
        static const QRegularExpression re(QStringLiteral(R"(font-size:\s*(\d+)px)"));
        QString out;
        out.reserve(sheet.size() + 32);
        qsizetype pos = 0;
        auto it = re.globalMatch(sheet);
        while (it.hasNext()) {
            const auto match = it.next();
            out.append(sheet.mid(pos, match.capturedStart(1) - pos));
            const int designed = match.captured(1).toInt();
            out.append(QString::number(InterfaceZoomMetrics::scale(designed, basePixels)));
            pos = match.capturedEnd(1);
        }
        out.append(sheet.mid(pos));
        return out;
    }

    // Merged into Agent Workspace QML tokens. Named steps only (7 text + 2 glyph).
    // `biasPixels` applies to text steps only (glyphs stay on the icon ladder).
    [[nodiscard]] static QVariantMap tokens(const int basePixels, const int biasPixels = 0) {
        const auto text = [basePixels, biasPixels](const TypeStep step) {
            return pixels(step, basePixels) + biasPixels;
        };
        return QVariantMap{
            {QStringLiteral("zoomFactor"), InterfaceZoomMetrics::factor(basePixels)},
            {QStringLiteral("micro"), text(TypeStep::Micro)},
            {QStringLiteral("caption"), text(TypeStep::Caption)},
            {QStringLiteral("body"), text(TypeStep::Body)},
            {QStringLiteral("reading"), text(TypeStep::Reading)},
            {QStringLiteral("title"), text(TypeStep::Title)},
            {QStringLiteral("heading"), text(TypeStep::Heading)},
            {QStringLiteral("display"), text(TypeStep::Display)},
            {QStringLiteral("glyphSmall"), pixels(TypeStep::GlyphSmall, basePixels)},
            {QStringLiteral("glyphLarge"), pixels(TypeStep::GlyphLarge, basePixels)},
        };
    }
};

} // namespace ketplus
