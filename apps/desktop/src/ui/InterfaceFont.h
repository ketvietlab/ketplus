#pragma once

class QApplication;
class QString;

namespace ketplus {

// Private chrome stack: embedded KET Sans, SF Pro (named, or macOS system UI
// face), bundled Inter, then the platform UI font. CM still embeds Inter; this overlay
// rewrites the live application font and QWidget family after ThemeManager
// applies.
struct InterfaceUiFont final {
    [[nodiscard]] static QString resolvedFamily();
    [[nodiscard]] static QString styleSheetFamilyList();
    [[nodiscard]] static QString settingsLabel();
    static void apply(QApplication& application, int pixelSize);
};

} // namespace ketplus
