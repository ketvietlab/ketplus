#pragma once

#include "KetProfile.h"
#include "ui/InterfaceZoom.h"

#include <QRegularExpression>
#include <QString>
#include <QVariantMap>
#include <QtGlobal>

namespace ketplus {

// Compatibility names for existing product adapters. Each resolves a published Két
// profile role; new surfaces consume the kit component/recipe directly.
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
        const auto& p = ket::Profile::current();
        switch (step) {
        case TypeStep::Micro:
            return qRound(p.dayDividerPixels);
        case TypeStep::Caption:
            return qRound(p.messageTimePixels);
        case TypeStep::Body:
            return qRound(p.buttonFontPixels);
        case TypeStep::Reading:
            return qRound(p.readingPixels);
        case TypeStep::Title:
            return qRound(p.dialogTitlePixels);
        case TypeStep::Heading:
            return qRound(p.markdownHeading2Pixels);
        case TypeStep::Display:
            return qRound(p.markdownHeading1Pixels);
        case TypeStep::GlyphSmall:
            return qRound(p.iconSize);
        case TypeStep::GlyphLarge:
            return qRound(p.emptyIconSize);
        }
        return InterfaceZoomMetrics::mediumPixels;
    }

    [[nodiscard]] static int pixels(const TypeStep step, const int basePixels) {
        return InterfaceZoomMetrics::scale(designed(step), basePixels);
    }

    // Scales literal QSS geometry and expands {{micro}}, {{caption}}, … font steps.
    [[nodiscard]] static QString apply(QString sheet, const int basePixels,
                                       const int /*legacyBiasPixels*/ = 0) {
        // Explicit product-composition values (for example a 30 px page title)
        // are still scaled here, rather than introducing another type ladder.
        static const QRegularExpression productPixels(QStringLiteral(R"(\{\{px:(\d+)\}\})"));
        sheet.replace(productPixels, QStringLiteral("\\1"));
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
            sheet.replace(QLatin1String(key), QString::number(pixels(step, basePixels)));
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
    // shared Theme.cpp compatibility sheet. Does not snap literals onto
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
    [[nodiscard]] static QVariantMap tokens(const int basePixels,
                                            const int /*legacyBiasPixels*/ = 0) {
        const auto text = [basePixels](const TypeStep step) { return pixels(step, basePixels); };
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
