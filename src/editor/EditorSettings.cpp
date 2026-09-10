#include "editor/EditorSettings.h"

#include <QFontDatabase>
#include <QSettings>
#include <QtGlobal>

namespace ketplus {
namespace {

constexpr auto fontFamilyKey = "editor/fontFamily";
constexpr auto fontSizeKey = "editor/fontSizePixels";
constexpr auto lineHeightKey = "editor/lineHeightPixels";

} // namespace

EditorSettings EditorSettings::defaults() {
    return {
        .fontFamily = QFontDatabase::systemFont(QFontDatabase::FixedFont).family(),
        .fontSizePixels = 13,
        .lineHeightPixels = 24,
    };
}

EditorSettings EditorSettings::load() {
    const QSettings settings;
    return load(settings);
}

EditorSettings EditorSettings::load(const QSettings& settings) {
    const auto fallback = defaults();
    return EditorSettings{
        .fontFamily = settings.value(QString::fromLatin1(fontFamilyKey), fallback.fontFamily)
                          .toString(),
        .fontSizePixels = settings.value(QString::fromLatin1(fontSizeKey),
                                         fallback.fontSizePixels)
                              .toInt(),
        .lineHeightPixels = settings.value(QString::fromLatin1(lineHeightKey),
                                           fallback.lineHeightPixels)
                                .toInt(),
    }
        .normalized();
}

void EditorSettings::save() const {
    QSettings settings;
    save(settings);
}

void EditorSettings::save(QSettings& settings) const {
    const auto value = normalized();
    settings.setValue(QString::fromLatin1(fontFamilyKey), value.fontFamily);
    settings.setValue(QString::fromLatin1(fontSizeKey), value.fontSizePixels);
    settings.setValue(QString::fromLatin1(lineHeightKey), value.lineHeightPixels);
}

EditorSettings EditorSettings::normalized() const {
    EditorSettings value = *this;
    if (value.fontFamily.trimmed().isEmpty()) {
        value.fontFamily = defaults().fontFamily;
    } else {
        value.fontFamily = value.fontFamily.trimmed();
    }
    value.fontSizePixels =
        qBound(minimumFontSizePixels, value.fontSizePixels, maximumFontSizePixels);
    value.lineHeightPixels =
        qBound(qMax(minimumLineHeightPixels, value.fontSizePixels), value.lineHeightPixels,
               maximumLineHeightPixels);
    return value;
}

QFont EditorSettings::font() const {
    const auto value = normalized();
    QFont font(value.fontFamily);
    font.setPixelSize(value.fontSizePixels);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    return font;
}

} // namespace ketplus
