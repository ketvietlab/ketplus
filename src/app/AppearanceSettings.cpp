#include "app/AppearanceSettings.h"

#include <QSettings>
#include <QtGlobal>

namespace ketplus {
namespace {

constexpr auto interfaceFontSizeKey = "appearance/interfaceFontSizePixels";
constexpr auto terminalFontSizeKey = "terminal/fontSizePixels";
constexpr auto terminalLineHeightKey = "terminal/lineHeightPixels";
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

AppearanceSettings AppearanceSettings::defaults() { return {}; }

AppearanceSettings AppearanceSettings::load() {
    const QSettings settings;
    return load(settings);
}

AppearanceSettings AppearanceSettings::load(const QSettings& settings) {
    const auto fallback = defaults();
    return AppearanceSettings{
        .interfaceFontSizePixels =
            settings
                .value(QString::fromLatin1(interfaceFontSizeKey), fallback.interfaceFontSizePixels)
                .toInt(),
        .editor = EditorSettings::load(settings),
        .terminal =
            loadTypography(settings, terminalFontSizeKey, terminalLineHeightKey, fallback.terminal),
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
    settings.setValue(QString::fromLatin1(interfaceFontSizeKey), value.interfaceFontSizePixels);
    value.editor.save(settings);
    saveTypography(settings, terminalFontSizeKey, terminalLineHeightKey, value.terminal);
    saveTypography(settings, previewFontSizeKey, previewLineHeightKey, value.preview);
}

AppearanceSettings AppearanceSettings::normalized() const {
    AppearanceSettings value = *this;
    value.interfaceFontSizePixels =
        qBound(minimumInterfaceFontSizePixels, value.interfaceFontSizePixels,
               maximumInterfaceFontSizePixels);
    value.editor = value.editor.normalized();
    value.terminal = value.terminal.normalized();
    value.preview = value.preview.normalized();
    return value;
}

} // namespace ketplus
