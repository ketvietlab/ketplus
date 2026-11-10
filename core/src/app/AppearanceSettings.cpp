#include "app/AppearanceSettings.h"

#include "ui/InterfaceZoom.h"

#include <QSettings>
#include <QtGlobal>

namespace ketplus {
namespace {

constexpr auto interfaceZoomKey = "appearance/interfaceZoomPercent";
constexpr auto interfaceFontSizeKey = "appearance/interfaceFontSizePixels";
constexpr auto terminalFontSizeKey = "terminal/fontSizePixels";
constexpr auto terminalLineHeightKey = "terminal/lineHeightPixels";
constexpr auto terminalFontFamilyKey = "terminal/fontFamily";
constexpr auto previewFontSizeKey = "preview/fontSizePixels";
constexpr auto previewLineHeightKey = "preview/lineHeightPixels";

TextTypographySettings loadTypography(const QSettings& settings, const char* fontSizeKey,
                                      const char* lineHeightKey,
                                      const TextTypographySettings& fallback) {
    return TextTypographySettings{
        .fontSizePixels =
            settings.value(QString::fromLatin1(fontSizeKey), fallback.fontSizePixels).toInt(),
        .lineHeightPixels =
            settings.value(QString::fromLatin1(lineHeightKey), fallback.lineHeightPixels).toInt(),
    }
        .normalized();
}

void saveTypography(QSettings& settings, const char* fontSizeKey, const char* lineHeightKey,
                    const TextTypographySettings& typography) {
    const auto value = typography.normalized();
    settings.setValue(QString::fromLatin1(fontSizeKey), value.fontSizePixels);
    settings.setValue(QString::fromLatin1(lineHeightKey), value.lineHeightPixels);
}

} // namespace

TextTypographySettings TextTypographySettings::normalized() const {
    TextTypographySettings value = *this;
    value.fontSizePixels =
        qBound(minimumFontSizePixels, value.fontSizePixels, maximumFontSizePixels);
    value.lineHeightPixels = qBound(qMax(minimumLineHeightPixels, value.fontSizePixels),
                                    value.lineHeightPixels, maximumLineHeightPixels);
    return value;
}

AppearanceSettings AppearanceSettings::defaults() { return AppearanceSettings{}.normalized(); }

AppearanceSettings AppearanceSettings::load() {
    const QSettings settings;
    return load(settings);
}

AppearanceSettings AppearanceSettings::load(const QSettings& settings) {
    const auto fallback = defaults();
    const QString zoomKey = QString::fromLatin1(interfaceZoomKey);
    const QString fontSizeKey = QString::fromLatin1(interfaceFontSizeKey);
    int zoom = fallback.interfaceZoomPercent;
    int fontSize = fallback.interfaceFontSizePixels;
    if (settings.contains(zoomKey)) {
        zoom = settings.value(zoomKey).toInt();
        fontSize = settings.value(fontSizeKey, fontSize).toInt();
    } else if (settings.contains(fontSizeKey)) {
        // Saved before ADR 0104, when the interface pixels were the zoom.
        zoom = InterfaceZoomMetrics::fromLegacyPixels(settings.value(fontSizeKey).toInt());
    }
    return AppearanceSettings{
        .interfaceZoomPercent = zoom,
        .interfaceFontSizePixels = fontSize,
        .editor = EditorSettings::load(settings),
        .terminal =
            loadTypography(settings, terminalFontSizeKey, terminalLineHeightKey, fallback.terminal),
        .terminalFontFamily =
            settings.value(QString::fromLatin1(terminalFontFamilyKey), fallback.terminalFontFamily)
                .toString(),
        .preview =
            loadTypography(settings, previewFontSizeKey, previewLineHeightKey, fallback.preview),
    }
        .normalized();
}

void AppearanceSettings::save() const {
    QSettings settings;
    save(settings);
}

void AppearanceSettings::save(QSettings& settings) const {
    const auto value = normalized();
    settings.setValue(QString::fromLatin1(interfaceZoomKey), value.interfaceZoomPercent);
    settings.setValue(QString::fromLatin1(interfaceFontSizeKey), value.interfaceFontSizePixels);
    value.editor.save(settings);
    saveTypography(settings, terminalFontSizeKey, terminalLineHeightKey, value.terminal);
    settings.setValue(QString::fromLatin1(terminalFontFamilyKey), value.terminalFontFamily);
    saveTypography(settings, previewFontSizeKey, previewLineHeightKey, value.preview);
}

AppearanceSettings AppearanceSettings::normalized() const {
    AppearanceSettings value = *this;
    value.interfaceZoomPercent = InterfaceZoomMetrics::nearestStep(value.interfaceZoomPercent);
    value.interfaceFontSizePixels =
        qBound(minimumInterfaceFontSizePixels, value.interfaceFontSizePixels,
               maximumInterfaceFontSizePixels);
    value.editor = value.editor.normalized();
    // Terminal text follows the editor and Markdown the interface (Jay, 2026-11-10); there
    // are no separate controls, so stored terminal and preview sizes are not read.
    value.terminal =
        TextTypographySettings{value.editor.fontSizePixels, value.editor.lineHeightPixels}
            .normalized();
    value.terminalFontFamily = value.terminalFontFamily.trimmed();
    const int markdownPixels = InterfaceZoomMetrics::scale(
        value.interfaceFontSizePixels + markdownOffsetPixels, value.interfaceZoomPercent);
    value.preview =
        TextTypographySettings{markdownPixels, qRound(markdownPixels * markdownLineHeightRatio)}
            .normalized();
    return value;
}

} // namespace ketplus
